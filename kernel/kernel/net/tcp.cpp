/*
 * Copyright (c) 2020 - 2022 Pedro Falcato
 * This file is part of Onyx, and is released under the terms of the MIT License
 * check LICENSE at the root directory for more information
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdio.h>

#include <onyx/byteswap.h>
#include <onyx/net/inet_proto.h>
#include <onyx/net/ip.h>
#include <onyx/net/socket_table.h>
#include <onyx/net/tcp.h>
#include <onyx/poll.h>
#include <onyx/random.h>
#include <onyx/timer.h>

socket_table tcp_table;

const inet_proto tcp_proto{"tcp", &tcp_table};

uint16_t tcpv4_calculate_checksum(tcp_header *header, uint16_t packet_length, uint32_t srcip,
                                  uint32_t dstip, bool calc_data = true);

#define TCP_MAKE_DATA_OFF(off) (off << TCP_DATA_OFFSET_SHIFT)

int tcp_init_netif(struct netif *netif)
{
    return 0;
}

int tcp_socket::bind(struct sockaddr *addr, socklen_t addrlen)
{
    auto fam = get_proto_fam();

    return fam->bind(addr, addrlen, this);
}

bool validate_tcp_packet(const tcp_header *header, size_t size)
{
    if (sizeof(tcp_header) > size) [[unlikely]]
        return false;

    auto flags = ntohs(header->data_offset_and_flags);

    uint16_t data_off = flags >> TCP_DATA_OFFSET_SHIFT;
    size_t off_bytes = tcp_header_data_off_to_length(data_off);

    if (off_bytes > size) [[unlikely]]
        return false;

    if (off_bytes < sizeof(tcp_header)) [[unlikely]]
        return false;

    return true;
}

/**
 * @brief Handle packet recv on SYN_SENT
 *
 * @param data Packet handling data
 * @return 0 on success, negative error codes
 */
int tcp_socket::do_receive_syn_sent(const packet_handling_data &data)
{
    auto tcphdr = data.header;
    const auto flags = htons(tcphdr->data_offset_and_flags);
    constexpr int valid_flags = TCP_FLAG_SYN | TCP_FLAG_ACK;
    if ((flags & 0xff) != valid_flags)
        return -1;

    window_size = ntohs(tcphdr->window_size) << window_size_shift;

    if (!parse_options(tcphdr))
    {
        /* Invalid packet */
        state = tcp_state::TCP_STATE_CLOSED;
        return -EIO;
    }

    auto starting_seq_number = ntohl(tcphdr->sequence_number);
    uint32_t seqs = 1;
    ack_number = starting_seq_number + seqs;

    do_ack(data.buffer);

    tcp_packet pkt{{}, this, TCP_FLAG_ACK, src_addr};

    auto res = pkt.result();

    if (!res)
    {
        state = tcp_state::TCP_STATE_CLOSED;
        sock_err = ENOBUFS;
        return -ENOBUFS;
    }

    auto ex = sendpbuf(res, true);

    if (ex.has_error())
    {
        state = tcp_state::TCP_STATE_CLOSED;
        sock_err = -ex.error();
        return ex.error();
    }

    state = tcp_state::TCP_STATE_ESTABLISHED;

    return 0;
}

/**
 * @brief Does acknowledgement of packets
 *
 * @param buf Packetbuf of the ack packet we got
 */
void tcp_socket::do_ack(packetbuf *buf)
{
    tcp_header *tcphdr = (tcp_header *) buf->transport_header;
    auto ack = ntohl(tcphdr->ack_number);

    scoped_lock g{pending_out_lock};

    list_for_every_safe (&pending_out_packets)
    {
        auto pkt = list_head_cpp<tcp_pending_out>::self_from_list_head(l);

        if (!pkt->ack_for_packet(last_ack_number, ack))
            continue;

        pkt->acked = true;

        wait_queue_wake_all(&pkt->wq);

        pkt->remove();

        /* Unref *must* be the last thing we do */
        pkt->unref();
    }

    last_ack_number = ack;

    g.unlock();
}

/**
 * @brief Handle packet recv on ESTABLISHED
 *
 * @param data Packet handling data
 * @return 0 on success, negative error codes
 */
int tcp_socket::do_established_rcv(const packet_handling_data &data)
{
    const auto tcphdr = (const tcp_header *) data.header;
    auto flags = htons(tcphdr->data_offset_and_flags);

    if (!(flags & TCP_FLAG_ACK))
    {
        // Every segment received after established needs to have ACK set
        return 0;
    }

    if (flags & TCP_FLAG_SYN)
    {
        // SYN is not a valid flag in this state
        return 0;
    }

    /* ack_number holds the other side of the connection's sequence number */
    auto starting_seq_number = ntohl(data.header->sequence_number);
    auto data_off = TCP_GET_DATA_OFF(ntohs(data.header->data_offset_and_flags));
    uint16_t data_size = data.tcp_segment_size - tcp_header_data_off_to_length(data_off);
    uint32_t seqs = data_size;
    if (flags & TCP_FLAG_FIN)
        seqs++;

    ack_number = starting_seq_number + seqs;

    if (data_size || flags & TCP_FLAG_FIN)
    {
        // If this wasn't a FIN packet, it has data
        // so append it to the receive buffers
        if (!(flags & TCP_FLAG_FIN))
        {
            auto buf = data.buffer;
            append_inet_rx_pbuf(buf);
        }

        // Now ack it

        tcp_packet pkt{{}, this, TCP_FLAG_ACK, src_addr};
        auto pbuf = pkt.result();

        if (!pbuf)
        {
            sock_err = ENOBUFS;
            return 0;
        }

        if (auto ex = sendpbuf(pbuf, true); ex.has_error())
        {
            sock_err = ex.error();
            return 0;
        }
    }
    else if (data_size == 0 && (flags & 0xff) == TCP_FLAG_ACK)
    {
        // Process the ACK
        do_ack(data.buffer);
    }

    return 0;
}

int tcp_socket::handle_packet(const tcp_socket::packet_handling_data &data)
{
    auto data_off = TCP_GET_DATA_OFF(ntohs(data.header->data_offset_and_flags));
    uint16_t header_size = tcp_header_data_off_to_length(data_off);

    if (data.tcp_segment_size < header_size)
        return -1;
#if 0
	printk("segment size: %u\n", data.tcp_segment_size);
	printk("header size: %u\n", header_size);
	printk("ack number %u\n", ack_number);
#endif

    uint16_t data_size = data.tcp_segment_size - header_size;
    data.buffer->data += header_size;
    cul::slice<uint8_t> buf{(uint8_t *) data.header + header_size, data_size};

    auto flags = htons(data.header->data_offset_and_flags);

    if (flags & TCP_FLAG_RST)
    {
        sock_err = ECONNRESET;

        scoped_lock g{pending_out_lock};

        list_for_every_safe (&pending_out_packets)
        {
            auto pkt = list_head_cpp<tcp_pending_out>::self_from_list_head(l);
            pkt->reset = true;
            wait_queue_wake_all(&pkt->wq);
            pkt->remove();

            /* Unref *must* be the last thing we do */
            pkt->unref();
        }

        return 0;
    }

    if (state == tcp_state::TCP_STATE_SYN_SENT)
        return do_receive_syn_sent(data);

    if (state == tcp_state::TCP_STATE_ESTABLISHED)
        return do_established_rcv(data);

    if (flags & TCP_FLAG_SYN)
    {
        if (state == tcp_state::TCP_STATE_LISTEN)
        {
            // TODO
#if 0
            // Only syn should be set
            if (flags & ~TCP_FLAG_SYN)
                return -1;

            tcp_socket *sock = new tcp_socket();

            if (!sock)
                return -1;

            sock->state = tcp_state::TCP_STATE_SYN_RECEIVED;
            sock->connect(struct sockaddr *addr, socklen_t addrlen)
#endif
            return 0;
        }
    }

    return 0;
}

int tcp_send_rst_no_socket(const sockaddr_in_both &dstaddr, in_port_t srcport, int domain,
                           netif *nif)
{
    auto buf = make_refc<packetbuf>();
    if (!buf)
        return -ENOMEM;

    auto ip_size = domain == AF_INET ? sizeof(ip_header) : sizeof(ip6_hdr);

    if (!buf->allocate_space(MAX_TCP_HEADER_LENGTH + ip_size))
        return -ENOMEM;

    buf->reserve_headers(MAX_TCP_HEADER_LENGTH + ip_size);

    auto hdr = (tcp_header *) buf->push_header(sizeof(tcp_header));

    memset(hdr, 0, sizeof(tcp_header));
    hdr->dest_port = dstaddr.in4.sin_port;
    hdr->source_port = srcport;
    hdr->data_offset_and_flags =
        htons(TCP_MAKE_DATA_OFF(tcp_header_length_to_data_off(sizeof(tcp_header))) | TCP_FLAG_RST);
    hdr->checksum = tcpv4_calculate_checksum(hdr, sizeof(tcp_header), nif->local_ip.sin_addr.s_addr,
                                             dstaddr.in4.sin_addr.s_addr);
    /* TODO: Don't assume IPv4 */

    inet_sock_address from{nif->local_ip.sin_addr, dstaddr.in4.sin_port};
    inet_sock_address to{dstaddr.in4};

    auto res = ip::v4::get_v4_proto()->route(from, to, domain);

    if (res.has_error())
        return res.error();

    iflow flow{res.value(), IPPROTO_TCP, false};

    return netif_send_packet(flow.nif, buf.get());
}

int tcp_handle_packet(netif *netif, packetbuf *buf)
{
    auto ip_header = (struct ip_header *) buf->net_header;
    int st = 0;
    auto header = reinterpret_cast<tcp_header *>(buf->data);

    if (!validate_tcp_packet(header, buf->length())) [[unlikely]]
        return 0;

    buf->transport_header = (unsigned char *) header;

    auto socket =
        inet_resolve_socket<tcp_socket>(ip_header->source_ip, header->source_port,
                                        header->dest_port, IPPROTO_TCP, netif, false, &tcp_proto);
    uint16_t tcp_payload_len =
        static_cast<uint16_t>(ntohs(ip_header->total_len) - ip_header_length(ip_header));

    if (!socket)
    {
        sockaddr_in_both addr;
        addr.in4.sin_addr.s_addr = ip_header->source_ip;
        addr.in4.sin_family = AF_INET;
        addr.in4.sin_port = header->source_port;

        auto flags = htons(header->data_offset_and_flags);

        if (!(flags & TCP_FLAG_RST))
            tcp_send_rst_no_socket(addr, header->dest_port, AF_INET, netif);
        /* No socket bound, bad packet. */
        return 0;
    }

    sockaddr_in_both both;
    ipv4_to_sockaddr(ip_header->source_ip, header->source_port, both.in4);

    const tcp_socket::packet_handling_data handle_data{buf, header, tcp_payload_len, &both,
                                                       AF_INET};

    socket->socket_lock.lock_bh();
    st = socket->handle_packet(handle_data);
    socket->socket_lock.unlock_bh();
    socket->unref();

    return st;
}

uint16_t tcpv4_calculate_checksum(tcp_header *header, uint16_t packet_length, uint32_t srcip,
                                  uint32_t dstip, bool calc_data)
{
    uint32_t proto = ((packet_length + IPPROTO_TCP) << 8);
    uint16_t buf[2];
    memcpy(&buf, &proto, sizeof(buf));

    auto r = __ipsum_unfolded(&srcip, sizeof(srcip), 0);
    r = __ipsum_unfolded(&dstip, sizeof(dstip), r);
    r = __ipsum_unfolded(buf, sizeof(buf), r);

    if (calc_data)
        r = __ipsum_unfolded(header, packet_length, r);

    return ipsum_fold(r);
}

uint16_t tcp_packet::options_length() const
{
    uint16_t len = 0;
    list_for_every_safe (&option_list)
    {
        tcp_option *opt = container_of(l, tcp_option, list_node);
        len += opt->length;
    }

    /* TCP options have padding to make sure it ends on a 32-bit boundary */
    if (len & (4 - 1))
        len = ALIGN_TO(len, 4);

    return len;
}

void tcp_packet::put_options(char *opts)
{
    list_for_every (&option_list)
    {
        tcp_option *opt = container_of(l, tcp_option, list_node);

        opts[0] = opt->kind;
        opts[1] = opt->length;
        /* Take off 2 bytes to account for the overhead of kind and length */
        memcpy(&opts[2], &opt->data, opt->length - 2);
        opts += opt->length;
    }
}

ref_guard<packetbuf> tcp_packet::result()
{
    buf = make_refc<packetbuf>();
    if (!buf)
        return {};

    if (!buf->allocate_space(payload.size_bytes() + socket->get_headers_len() +
                             MAX_TCP_HEADER_LENGTH))
        return {};
    buf->reserve_headers(socket->get_headers_len() + MAX_TCP_HEADER_LENGTH);

    uint16_t options_len = options_length();
    auto header_size = sizeof(tcp_header) + options_len;

    tcp_header *header = (tcp_header *) buf->push_header(header_size);

    buf->transport_header = (unsigned char *) header;

    memset(header, 0, header_size);

    auto &dest = socket->daddr();

    auto data_off = TCP_MAKE_DATA_OFF(tcp_header_length_to_data_off(header_size));

    /* Assume the max window size as the window size, for now */
    header->window_size = htons(socket->window_size);
    header->source_port = socket->saddr().port;
    header->sequence_number = htonl(socket->sequence_nr());
    header->data_offset_and_flags = htons(data_off | flags);
    header->dest_port = dest.port;
    header->urgent_pointer = 0;

    if (flags & TCP_FLAG_ACK)
        header->ack_number = htonl(socket->acknowledge_nr());
    else
        header->ack_number = 0;

    put_options(reinterpret_cast<char *>(header + 1));

    auto length = payload.size_bytes();

    if (length != 0)
    {
        auto ptr = buf->put(length);
        memcpy(ptr, payload.data(), length);
    }

    auto &route = socket->route_cache;
    auto nif = route.nif;

    if (nif->flags & NETIF_SUPPORTS_CSUM_OFFLOAD && !socket->needs_fragmenting(nif, buf.get()))
    {
        /* TODO: Don't assume IPv4 */
        header->checksum =
            ~tcpv4_calculate_checksum(header, static_cast<uint16_t>(header_size + length),
                                      route.src_addr.in4.s_addr, route.dst_addr.in4.s_addr, false);
        buf->csum_offset = &header->checksum;
        buf->csum_start = (unsigned char *) header;
        buf->needs_csum = 1;
    }
    else
    {
        header->checksum =
            tcpv4_calculate_checksum(header, static_cast<uint16_t>(header_size + length),
                                     route.src_addr.in4.s_addr, route.dst_addr.in4.s_addr);
    }

    starting_seq_number = socket->sequence_nr();
    uint32_t seqs = length;
    if (flags & TCP_FLAG_SYN)
        seqs++;

    socket->sequence_nr() += seqs;

    return buf;
}

int tcp_packet::wait_for_ack()
{
    return wait_for_event_interruptible(&ack_wq, acked);
}

int tcp_packet::wait_for_ack_timeout(hrtime_t _timeout)
{
    return wait_for_event_timeout_interruptible(&ack_wq, acked, _timeout);
}

static constexpr uint16_t min_header_size = sizeof(tcp_header);

bool tcp_socket::parse_options(tcp_header *packet)
{
    auto flags = ntohs(packet->data_offset_and_flags);

    bool syn_set = flags & TCP_FLAG_SYN;
    (void) syn_set;

    uint16_t data_off = flags >> TCP_DATA_OFFSET_SHIFT;

    if (data_off == tcp_header_length_to_data_off(min_header_size))
        return true;

    auto data_off_bytes = tcp_header_data_off_to_length(data_off);

    uint8_t *options = reinterpret_cast<uint8_t *>(packet + 1);
    uint8_t *end = options + (data_off_bytes - min_header_size);

    while (options != end)
    {
        uint8_t opt_byte = *options;

        /* The layout of TCP options is [byte 0 - option kind]
         * [byte 1 - option length ] [byte 2...length - option data]
         */

        if (opt_byte == TCP_OPTION_END_OF_OPTIONS)
            break;

        if (opt_byte == TCP_OPTION_NOP)
        {
            options++;
            continue;
        }

        uint8_t length = *(options + 1);

        switch (opt_byte)
        {
            case TCP_OPTION_MSS:
                if (!syn_set)
                    return false;

                mss = *(uint16_t *) (options + 2);
                mss = ntohs(mss);
                break;
            case TCP_OPTION_WINDOW_SCALE:
                if (!syn_set)
                    return false;

                uint8_t wss = *(options + 2);
                window_size_shift = wss;
                break;
        }

        options += length;
    }

    return true;
}

/* TODO: This doesn't apply to IPv6 */
constexpr uint16_t tcp_headers_overhead =
    sizeof(struct tcp_header) + sizeof(struct eth_header) + IPV4_MIN_HEADER_LEN;

void tcp_out_timeout(clockevent *ev)
{
    tcp_pending_out *t = (tcp_pending_out *) ev->priv;

    if (t->acked)
    {
        ev->flags &= ~CLOCKEVENT_FLAG_PULSE;
        return;
    }

    if (t->transmission_try == tcp_retransmission_max)
    {
        wait_queue_wake_all(&t->wq);
        ev->flags &= ~CLOCKEVENT_FLAG_PULSE;
        scoped_lock g{t->sock->pending_out_lock};
        list_remove(&t->node);
        return;
    }

    t->transmission_try++;

    iflow flow{t->sock->route_cache, IPPROTO_TCP, false};

    // Since the packet has already been pre-prepared by the network stack
    // we can just send it straight through the network interface
    int st = netif_send_packet(flow.nif, t->buf.get());
    // TODO: signal error
    (void) st;
    hrtime_t next_timeout = 200;
    for (unsigned int i = 0; i < t->transmission_try; i++)
    {
        next_timeout *= 2;
    }

    ev->deadline = clocksource_get_time() + next_timeout * NS_PER_MS;
}

/**
 * @brief Sends a packetbuf
 *
 * @param buf Packetbuf to send
 * @param noack True if no ack is needed
 * @return Expected of a ref_guard to a tcp_pending_out, or a negative error code
 */
expected<ref_guard<tcp_pending_out>, int> tcp_socket::sendpbuf(ref_guard<packetbuf> buf, bool noack)
{
    iflow flow{route_cache, IPPROTO_TCP, false};
    ref_guard<tcp_pending_out> pending;
    if (!noack)
    {
        scoped_lock g{pending_out_lock};

        pending = make_refc<tcp_pending_out>(this);
        if (!pending)
        {
            return unexpected{-ENOBUFS};
        }

        pending->buf = buf;
        pending->timer.deadline = clocksource_get_time() + 200 * NS_PER_MS;
        pending->timer.priv = pending.get();
        pending->timer.flags = CLOCKEVENT_FLAG_PULSE;
        pending->timer.callback = tcp_out_timeout;
        append_pending_out(pending.get());
    }

    int st = ip::v4::send_packet(flow, buf.get());

    if (st < 0)
        return unexpected{st};

    if (!noack)
    {
        timer_queue_clockevent(&pending->timer);
    }

    if (noack)
        return ref_guard<tcp_pending_out>{};

    return pending;
}

int tcp_socket::start_handshake(netif *nif)
{
    tcp_packet first_packet{{}, this, TCP_FLAG_SYN, src_addr};
    first_packet.set_packet_flags(TCP_PACKET_FLAG_ON_STACK | TCP_PACKET_FLAG_WANTS_ACK_HEADER);

    tcp_option opt{TCP_OPTION_MSS, 4};

    uint16_t our_mss = nif->mtu - tcp_headers_overhead;
    opt.data.mss = htons(our_mss);

    first_packet.append_option(&opt);

    auto buf = first_packet.result();

    if (!buf)
        return -ENOBUFS;

    auto ex = sendpbuf(buf);

    if (ex.has_error())
        return ex.error();

    auto val = ex.value();

    state = tcp_state::TCP_STATE_SYN_SENT;

    int st = val->wait();

    if (st < 0)
        return st;

#if 0
	printk("ack received\n");
#endif

    if (st < 0)
    {
        /* wait_for_ack returns the error code in st */
        state = tcp_state::TCP_STATE_CLOSED;
        return st;
    }

    state = tcp_state::TCP_STATE_SYN_RECEIVED;

#if 0
	/* TODO: Add this */
	auto packet = ack->get_packet();

	ack_number = ntohl(packet->sequence_number);
	seq_number++;
	window_size = ntohs(packet->window_size) << window_size_shift;

	if(!parse_options(packet))
	{
		delete ack;
		/* Invalid packet */
		state = tcp_state::TCP_STATE_CLOSED;
		return -EIO;
	}

	delete ack;
#endif

    return 0;
}

int tcp_socket::finish_handshake(netif *nif)
{
    tcp_packet packet{{}, this, TCP_FLAG_ACK, src_addr};
    packet.set_packet_flags(TCP_PACKET_FLAG_ON_STACK);

    return 0;
}

int tcp_socket::start_connection()
{
    seq_number = arc4random();

    auto fam = get_proto_fam();

    auto result = fam->route(src_addr, dest_addr, domain);

    if (result.has_error())
        return result.error();

    route_cache = result.value();
    route_cache_valid = 1;

    int st = start_handshake(route_cache.nif);
    if (st < 0)
        return st;

    st = finish_handshake(route_cache.nif);

    state = tcp_state::TCP_STATE_ESTABLISHED;

    expected_ack = ack_number;

    return st;
}

int tcp_socket::connect(struct sockaddr *addr, socklen_t addrlen)
{
    if (!bound)
    {
        auto fam = get_proto_fam();
        int st = fam->bind_any(this);
        if (st < 0)
            return st;
    }

    if (connected)
        return -EISCONN;

    if (!validate_sockaddr_len_pair(addr, addrlen))
        return -EINVAL;

    struct sockaddr_in *in = (struct sockaddr_in *) addr;
    struct sockaddr_in6 *in6 = (sockaddr_in6 *) addr;

    if (domain == AF_INET)
        dest_addr = inet_sock_address{*in};
    else
        dest_addr = inet_sock_address{*in6};
    connected = true;

    return start_connection();
}

ssize_t tcp_socket::queue_data(iovec *vec, int vlen, size_t len)
{
    if (current_pos + len > send_buffer.buf_size())
    {
        if (!send_buffer.alloc_buf(current_pos + len))
        {
            return -EINVAL;
        }
    }

    uint8_t *ptr = send_buffer.get_buf() + current_pos;

    for (int i = 0; i < vlen; i++, vec++)
    {
        if (copy_from_user(ptr, vec->iov_base, vec->iov_len) < 0)
            return -EINVAL;

        ptr += vec->iov_len;
    }

    current_pos += len;

    return 0;
}

ssize_t tcp_socket::get_max_payload_len(uint16_t tcp_header_len)
{
    return 0;
}

void tcp_socket::try_to_send()
{
/* TODO: Implement Nagle's algorithm.
 * Before we do that, we should probably have retransmission implemented.
 */
#if 0
	if(window_size >= mss && current_pos >= mss)
	{
		cul::slice<const uint8_t> data{send_buffer.begin(), mss};
		tcp_packet packet{data, this, TCP_FLAG_ACK | TCP_FLAG_PSH};

		packet.send();

		/* TODO: Implement retries in general? */
		/* TODO: There's lots of room for improvement - maybe a list of buffer
		 * would be a better idea and would avoid this gigantic memcpy we have below
		 * due to vector.
		 */
		auto old_pos = current_pos;
		current_pos -= data.size_bytes();

		if(current_pos != 0)
			memcpy(send_buffer.begin(), send_buffer.end() + 1, old_pos - current_pos);
	}
	else
#endif
    {
        auto fam = get_proto_fam();

        auto netif = fam->route(src_addr, dest_addr, domain);
        /* TODO: Support TCP segmentation instead of relying on IPv4 segmentation */
        cul::slice<const uint8_t> data{send_buffer.begin(), current_pos};
        current_pos = 0;

        tcp_packet packet{data, this, TCP_FLAG_ACK | TCP_FLAG_PSH, src_addr};

        auto buf = packet.result();
        if (!buf)
        {
            sock_err = ENOBUFS;
            return;
        }

        auto ex = sendpbuf(buf);

        if (ex.has_error())
        {
            sock_err = -ex.error();
        }
    }
}

ssize_t tcp_socket::sendmsg(const msghdr *msg, int flags)
{
    if (msg->msg_name)
        return -EISCONN;

    auto len = iovec_count_length(msg->msg_iov, msg->msg_iovlen);

    if (len < 0)
        return len;

    if (len > UINT16_MAX)
        return -EINVAL;

    mutex_lock(&send_lock);

    auto st = queue_data(msg->msg_iov, msg->msg_iovlen, (size_t) len);
    if (st < 0)
    {
        mutex_unlock(&send_lock);
        return st;
    }

    try_to_send();

    mutex_unlock(&send_lock);

    return len;
}

void tcp_socket::append_pending_out(tcp_pending_out *pckt)
{
    list_add_tail(&pckt->node, &pending_out_packets);

    /* Don't forget to ref the packet! */
    pckt->ref();
}

void tcp_socket::remove_pending_out(tcp_pending_out *pkt)
{
    list_remove(&pkt->node);

    /* And also don't forget to unref it back! */
    pkt->unref();
}

int tcp_socket::setsockopt(int level, int opt, const void *optval, socklen_t optlen)
{
    if (level == SOL_SOCKET)
        return setsockopt_socket_level(opt, optval, optlen);

    if (is_inet_level(level))
        return setsockopt_inet(level, opt, optval, optlen);

    return -ENOPROTOOPT;
}

int tcp_socket::getsockopt(int level, int opt, void *optval, socklen_t *optlen)
{
    if (level == SOL_SOCKET)
        return getsockopt_socket_level(opt, optval, optlen);
    return -ENOPROTOOPT;
}

struct socket *tcp_create_socket(int type)
{
    auto sock = new tcp_socket();

    if (sock)
    {
        sock->proto_info = &tcp_proto;
    }

    return sock;
}

int tcp_socket::shutdown(int how)
{
    return 0;
}

void tcp_socket::close()
{
    shutdown(SHUT_RDWR);
    unref();
}

expected<packetbuf *, int> tcp_socket::get_segment(int flags)
{
    scoped_lock g{rx_packet_list_lock};

    int st = 0;
    packetbuf *buf = nullptr;

    do
    {
        if (st == -EINTR)
            return unexpected<int>{st};

        buf = get_rx_head();
        if (!buf && flags & MSG_DONTWAIT)
            return unexpected<int>{-EWOULDBLOCK};

        st = wait_for_segments();
    } while (!buf);

    g.keep_locked();

    return buf;
}

ssize_t tcp_socket::recvmsg(msghdr *msg, int flags)
{
    auto iovlen = iovec_count_length(msg->msg_iov, msg->msg_iovlen);
    if (iovlen < 0)
        return iovlen;

    auto st = get_segment(flags);
    if (st.has_error())
        return st.error();

    auto buf = st.value();
    ssize_t read = min(iovlen, (long) buf->length());
    ssize_t was_read = 0;
    ssize_t to_ret = read;

    if (iovlen < buf->length())
        msg->msg_flags = MSG_TRUNC;

    if (flags & MSG_TRUNC)
    {
        to_ret = buf->length();
    }

    const unsigned char *ptr = buf->data;

    if (msg->msg_name)
    {
        auto hdr = (tcp_header *) buf->transport_header;
        ip::copy_msgname_to_user(msg, buf, domain == AF_INET6, hdr->source_port);
    }

    for (int i = 0; i < msg->msg_iovlen; i++)
    {
        auto iov = msg->msg_iov[i];
        auto to_copy = min((ssize_t) iov.iov_len, read - was_read);
        // TODO: Replace rx_packet_list_lock with the socket hybrid lock
        if (copy_to_user(iov.iov_base, ptr, to_copy) < 0)
        {
            spin_unlock(&rx_packet_list_lock);
            return -EFAULT;
        }

        was_read += to_copy;

        ptr += to_copy;

        buf->data += to_copy;
    }

    msg->msg_controllen = 0;

    if (!(flags & MSG_PEEK))
    {
        if (buf->length() == 0)
        {
            list_remove(&buf->list_node);
            buf->unref();
        }
    }

    spin_unlock(&rx_packet_list_lock);

#if 0
	printk("recv success %ld bytes\n", read);
	printk("iovlen %ld\n", iovlen);
#endif

    return to_ret;
}

short tcp_socket::poll(void *poll_file, short events)
{
    short avail_events = POLLOUT;

    scoped_lock g{rx_packet_list_lock};

    if (events & POLLIN)
    {
        if (has_data_available())
            avail_events |= POLLIN;
        else
            poll_wait_helper(poll_file, &rx_wq);
    }

    // printk("avail events: %u\n", avail_events);

    return avail_events & events;
}

int tcp_socket::getsockname(sockaddr *addr, socklen_t *len)
{
    copy_addr_to_sockaddr(src_addr, addr, len);

    return 0;
}

int tcp_socket::getpeername(sockaddr *addr, socklen_t *len)
{
    copy_addr_to_sockaddr(dest_addr, addr, len);
    return 0;
}

int tcp_socket::listen()
{
    if (!bound)
    {
        int st = get_proto_fam()->bind_any(this);

        if (st < 0)
            return -EADDRINUSE;
    }

    if (connected)
        return -EINVAL;

    state = tcp_state::TCP_STATE_LISTEN;

    return 0;
}
