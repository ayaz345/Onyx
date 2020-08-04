/*
* Copyright (c) 2018 Pedro Falcato
* This file is part of Onyx, and is released under the terms of the MIT License
* check LICENSE at the root directory for more information
*/
#include <errno.h>

#include <onyx/file.h>
#include <onyx/utils.h>
#include <onyx/poll.h>
#include <onyx/dentry.h>

#include <onyx/net/socket.h>
#include <onyx/net/ip.h>
#include <onyx/scoped_lock.h>

socket *file_to_socket(struct file *f)
{
	return static_cast<socket *>(f->f_ino->i_helper);
}

/* Most of these default values don't make much sense, but we have them as placeholders */
int socket::listen()
{
	return 0;
}

socket *socket::accept(socket_conn_request *req)
{
	(void) req;
	return errno = EIO, nullptr;
}

int socket::bind(struct sockaddr *addr, socklen_t addrlen)
{
	(void) addr;
	(void) addrlen;
	return -EIO;
}

int socket::connect(struct sockaddr *addr, socklen_t addrlen)
{
	(void) addr;
	(void) addrlen;
	return -EIO;
}

ssize_t socket::sendto(const void *buf, size_t len, int flags,
		struct sockaddr *addr, socklen_t addrlen)
{
	(void) buf;
	(void) len;
	(void) flags;
	(void) addr;
	(void) addrlen;
	return -EIO;
}

ssize_t recv_queue::recvfrom(void *_buf, size_t len, int flags, sockaddr *src_addr, socklen_t *slen)
{
	char *buf = (char *) _buf;
	bool storing_src = src_addr != nullptr;
	bool remove_data = !(flags & MSG_PEEK);
	ssize_t total_read = 0;

	int st = 0;

	auto list = get_recv_packet_list(flags, len, st);
	if(!list)
	{
		return st;
	}

	list_for_every_safe(&recv_list)
	{
		auto packet = list_head_cpp<recv_packet>::self_from_list_head(l);

		if(storing_src)
		{
			//printk("packet src: %u\n", ((sockaddr_in *) &packet->src_addr)->sin_addr.s_addr);
			if(copy_to_user(src_addr, &packet->src_addr, packet->addr_len) < 0)
			{
				spin_unlock(&recv_queue_lock);
				return -EFAULT;
			}

			socklen_t length = packet->addr_len;
			if(copy_to_user(slen, &length, sizeof(socklen_t)) < 0)
			{
				spin_unlock(&recv_queue_lock);
				return -EFAULT;
			}

			/* don't store src twice. Although I'm not sure how defined storing_src is with !SOCK_DGRAM */
			storing_src = false;
		}

		auto avail = packet->size - packet->read;

		ssize_t to_copy = min(len, avail);

		if(copy_to_user(buf, (char *) packet->payload + packet->read, to_copy) < 0)
		{
			spin_unlock(&recv_queue_lock);
			return -EFAULT;
		}

		buf += to_copy;
		total_read += to_copy;
		len -= to_copy;

		if(remove_data)
		{
			packet->read += to_copy;
			total_data_in_buffers -= to_copy;

			if(packet->read == packet->size || sock->type == SOCK_DGRAM)
			{
				total_data_in_buffers -= packet->size - packet->read; 
				list_remove(&packet->list_node);
				delete packet;
			}
		}

		if(total_read == (ssize_t) len || sock->type == SOCK_DGRAM)
			break;
	}

	spin_unlock(&recv_queue_lock);

	return total_read;
}

void recv_queue::clear_packets()
{
	scoped_lock guard{&recv_queue_lock};

	list_for_every_safe(&recv_list)
	{
		auto packet = list_head_cpp<recv_packet>::self_from_list_head(l);
		list_remove(&packet->list_node);
		total_data_in_buffers -= (packet->size - packet->read);
		delete packet;
	}
}

recv_queue::~recv_queue()
{
	clear_packets();

	assert(total_data_in_buffers == 0);
}

bool recv_queue::has_data_available(int msg_flags, size_t required_data)
{
	if(msg_flags & MSG_WAITALL)
	{
		return total_data_in_buffers >= required_data;
	}

	return !list_is_empty(&recv_list);
}

bool recv_queue::poll(void *poll_file)
{
	scoped_lock guard{&recv_queue_lock};

	if(has_data_available(0, 0))
		return true;
	
	poll_wait_helper(poll_file, &recv_wait);
	return false;
}

/* Returns with recv_queue_lock held on success */
struct list_head *recv_queue::get_recv_packet_list(int msg_flags, size_t required_data, int &error)
{
	spin_lock(&recv_queue_lock);

	if(msg_flags & MSG_DONTWAIT && !has_data_available(msg_flags, required_data))
	{
		spin_unlock(&recv_queue_lock);
		error = -EAGAIN;
		return nullptr;
	}

	/* TODO: Add recv timeout support */
	error = wait_for_event_locked_interruptible(&recv_wait, has_data_available(msg_flags, required_data),
	                                            &recv_queue_lock);
	if(error == 0)
	{
		return &recv_list;
	}
	else
	{
		spin_unlock(&recv_queue_lock);
		return nullptr;
	}
}

int fd_flags_to_msg_flags(struct file *f)
{
	int flags = 0;
	if(f->f_flags & O_NONBLOCK)
		flags |= MSG_DONTWAIT;
	return flags;
}

void recv_queue::add_packet(recv_packet *p)
{
	scoped_lock guard{&recv_queue_lock};

	list_add_tail(&p->list_node, &recv_list);
	total_data_in_buffers += p->size;
	wait_queue_wake_all(&recv_wait);

}

ssize_t socket::socket::recvfrom(void *buf, size_t len, int flags, sockaddr *src_addr, socklen_t *slen)
{
	recv_queue &q = (flags & MSG_OOB) ? oob_data_queue : in_band_queue;

	return q.recvfrom(buf, len, flags, src_addr, slen);
}

size_t socket_write(size_t offset, size_t len, void* buffer, struct file *file)
{
	socket *s = file_to_socket(file);

	return s->sendto(buffer, len, fd_flags_to_msg_flags(file), nullptr, 0);
}

size_t socket_read(size_t offset, size_t len, void *buffer, file *file)
{
	socket *s = file_to_socket(file);

	return s->recvfrom(buffer, len, fd_flags_to_msg_flags(file), nullptr, nullptr);
}

short socket::poll(void *poll_file, short events)
{
	short avail_events = POLLOUT;

	if(events & POLLPRI)
	{
		if(oob_data_queue.poll(poll_file))
			avail_events |= POLLPRI;
	}

	if(events & POLLIN)
	{
		if(in_band_queue.poll(poll_file))
			avail_events |= POLLIN;
	}

	//printk("avail events: %u\n", avail_events);

	return avail_events & events;
}

short socket_poll(void *poll_file, short events, struct file *node)
{
	socket *s = file_to_socket(node);

#if 0
	if(s->s_ops->poll)
		return s->s_ops->poll(poll_file, events, s);
#endif
	return s->poll(poll_file, events);	
}

void socket_close(struct inode *ino);

struct file_ops socket_ops = 
{
	.read = socket_read,
	.write = socket_write,
	.close = socket_close,
	.poll = socket_poll
};

struct file *get_socket_fd(int fd)
{
	struct file *desc = get_file_description(fd);
	if(!desc)
		return errno = EBADF, nullptr;

	if(desc->f_ino->i_fops->write != socket_write)
	{
		fd_put(desc);
		return errno = ENOTSOCK, nullptr;
	}

	return desc;
}

extern "C"
ssize_t sys_sendto(int sockfd, const void *buf, size_t len, int flags,
	struct sockaddr *addr, socklen_t addrlen)
{
	struct file *desc = get_socket_fd(sockfd);
	if(!desc)
		return -errno;

	socket *s = file_to_socket(desc);
	ssize_t ret = s->sendto(buf, len, flags, addr, addrlen);

	fd_put(desc);
	return ret;
}

extern "C"
int sys_connect(int sockfd, const struct sockaddr *uaddr, socklen_t addrlen)
{
	sockaddr_storage addr;
	if(addrlen > sizeof(sockaddr_storage))
		return -EINVAL;

	if(copy_from_user(&addr, uaddr, addrlen) < 0)
		return -EFAULT;

	struct file *desc = get_socket_fd(sockfd);
	if(!desc)
		return -errno;
	
	int ret = -EINTR;
	socket *s = file_to_socket(desc);

	/* See the comment below in sys_bind for explanation */
	if(mutex_lock_interruptible(&s->connection_state_lock) < 0)
		goto out;

	if(s->connected)
	{
		ret = -EISCONN;
		goto out2;
	}

	ret = s->connect((sockaddr *) &addr, addrlen);

out2:
	mutex_unlock(&s->connection_state_lock);
out:
	fd_put(desc);
	return ret;
}

extern "C"
int sys_bind(int sockfd, const struct sockaddr *uaddr, socklen_t addrlen)
{
	sockaddr_storage addr;
	if(addrlen > sizeof(sockaddr_storage))
		return -EINVAL;

	if(copy_from_user(&addr, uaddr, addrlen) < 0)
		return -EFAULT;

	struct file *desc = get_socket_fd(sockfd);
	if(!desc)
		return -errno;

	socket *s = file_to_socket(desc);
	int ret = -EINTR;

	/* We use mutex_lock_interruptible here as we can be held up for quite a
	 * big amount of time for things like TCP connect()s that are timing out.
	 */
	if(mutex_lock_interruptible(&s->connection_state_lock) < 0)
		goto out;
	
	if(s->bound)
	{
		ret = -EINVAL;
		goto out2;
	}

	ret = s->bind((sockaddr *) &addr, addrlen);

out2:
	mutex_unlock(&s->connection_state_lock);

out:
	fd_put(desc);
	return ret;
}

extern "C"
ssize_t sys_recvfrom(int sockfd, void *buf, size_t len, int flags,
                     struct sockaddr *src_addr, socklen_t *addrlen)
{
	struct file *desc = get_socket_fd(sockfd);
	if(!desc)
		return -errno;

	socket *s = file_to_socket(desc);

	flags |= fd_flags_to_msg_flags(desc);

	ssize_t ret = s->recvfrom(buf, len, flags, src_addr, addrlen);

	fd_put(desc);
	return ret;
}

#define BACKLOG_FOR_LISTEN_0			16
const int backlog_limit = 4096;

extern "C"
int sys_listen(int sockfd, int backlog)
{
	int st = 0;
	struct file *f = get_socket_fd(sockfd);
	if(!f)
		return -errno;

	socket *sock = file_to_socket(f);

	if(sock->type != SOCK_DGRAM || sock->type != SOCK_SEQPACKET)
	{
		st = -EOPNOTSUPP;
		goto out;
	}

	/* POSIX specifies that if backlog = 0, we can (and should) set the backlog value
	 * to a implementation specified minimum
	 */

	if(backlog == 0)
	{
		backlog = BACKLOG_FOR_LISTEN_0;
	}

	/* We should also set a backlog limit to stop DDOS attacks, and clamp the value */
	if(backlog > backlog_limit)
		backlog = backlog_limit;
	
	if(mutex_lock_interruptible(&sock->connection_state_lock) < 0)
	{
		st = -EINTR;
		goto out;
	}

	/* Big note: the backlog value in the socket structure is used both to determine
	 * the backlog size **and** if the socket is in a listening state, with != 0 repre-
	 * senting that state.
	*/
	
	sock->backlog = backlog;

	if((st = sock->listen()) < 0)
	{
		/* Don't forget to reset the backlog to 0 to show that it's not in a
		 * listening state
		*/
		sock->backlog = 0;
		goto out2;
	}

out2:
	mutex_unlock(&sock->connection_state_lock);
out:
	fd_put(f);
	return st;
}

extern "C"
int sys_shutdown(int sockfd, int how)
{
	return 0;
}

int check_af_support(int domain)
{
	switch(domain)
	{
		case AF_INET:
			return 0;
		case AF_UNIX:
			return 0;
		default:
			return -1;
	}
}

static const int type_mask = ~(SOCK_NONBLOCK | SOCK_CLOEXEC);
static const int sock_flag_mask = ~type_mask;

int net_check_type_support(int type)
{
	(void) sock_flag_mask;
	return 1;
}

int net_autodetect_protocol(int type, int domain)
{
	switch(type & type_mask)
	{
		case SOCK_DGRAM:
		{
			if(domain == AF_UNIX)
				return PROTOCOL_UNIX;
			else if(domain == AF_INET || domain == AF_INET6)
				return IPPROTO_UDP;
			else
				return -1;
		}
		case SOCK_RAW:
		{
			if(domain == AF_INET)
				return IPPROTO_IP;
			else if(domain == AF_INET6)
				return IPPROTO_IPV6;
			else if(domain == AF_UNIX)
				return PROTOCOL_UNIX;
			return -1;
		}
		case SOCK_STREAM:
		{
			if(domain == AF_INET || domain == AF_INET6)
				return IPPROTO_TCP;
			else
				return -1;
		}
	}

	return -1;
}


socket *unix_create_socket(int type, int protocol);

socket *socket_create(int domain, int type, int protocol)
{
	socket *socket = nullptr;
	switch(domain)
	{
		case AF_INET:
			socket = ip::v4::create_socket(type, protocol);
			break;
		case AF_UNIX:
			// socket = unix_create_socket(type, protocol);
			/* TODO: Fix unix sockets */
			socket = nullptr;
			break;
		default:
			return errno = EAFNOSUPPORT, nullptr;
	}

	if(!socket)
		return nullptr;

	socket->type = type;
	socket->domain = domain;
	socket->proto = protocol;
	INIT_LIST_HEAD(&socket->conn_request_list);

	return socket;
}

void socket_close(struct inode *ino)
{
	socket *s = static_cast<socket *>(ino->i_helper);

	s->unref();
}

struct inode *socket_create_inode(socket *socket)
{
	struct inode *inode = inode_create(false);

	if(!inode)
		return nullptr;
	
	inode->i_fops = &socket_ops;

	inode->i_type = VFS_TYPE_UNIX_SOCK;
	inode->i_flags = INODE_FLAG_NO_SEEK;
	inode->i_helper = socket;

	return inode;
}

file *socket_inode_to_file(inode *ino)
{
	auto f = inode_to_file(ino);
	if(!f)
		return nullptr;

	auto dent = dentry_create("<socket>", ino, nullptr);
	if(!dent)
	{
		fd_put(f);
		return nullptr;
	}

	f->f_dentry = dent;
	return f;
}

extern "C"
int sys_socket(int domain, int type, int protocol)
{
	int dflags;
	dflags = O_RDWR;

	if(check_af_support(domain) < 0)
		return -EAFNOSUPPORT;

	if(net_check_type_support(type) < 0)
		return -EINVAL;

	if(protocol == 0)
	{
		/* If protocol == 0, auto-detect the proto */
		if((protocol = net_autodetect_protocol(type, domain)) < 0)
			return -EINVAL;
	}

	/* Create the socket */
	socket *socket = socket_create(domain, type & type_mask, protocol);
	if(!socket)
		return -errno;
	
	struct inode *inode = socket_create_inode(socket);
	if(!inode)
		return -errno;

	struct file *f = socket_inode_to_file(inode);
	if(!f)
	{
		close_vfs(inode);
		return -ENOMEM;
	}

	if(type & SOCK_CLOEXEC)
		dflags |= O_CLOEXEC;
	if(type & SOCK_NONBLOCK)
		dflags |= O_NONBLOCK;

	/* Open a file descriptor with the socket vnode */
	int fd = open_with_vnode(f, dflags);
	/* If we failed, close the socket and return */
	if(fd < 0)
		close_vfs(inode);
	fd_put(f);

	return fd;
}

#define ACCEPT4_VALID_FLAGS		(SOCK_CLOEXEC | SOCK_NONBLOCK)		

socket_conn_request *dequeue_conn_request(socket *sock)
{
	spin_lock(&sock->conn_req_list_lock);

	assert(list_is_empty(&sock->conn_request_list) == false);
	struct list_head *first_elem = list_first_element(&sock->conn_request_list);

	list_remove(first_elem);

	spin_unlock(&sock->conn_req_list_lock);

	socket_conn_request *req = container_of(first_elem, socket_conn_request, list_node);

	return req;
}

extern "C"
int sys_accept4(int sockfd, struct sockaddr *addr, socklen_t *slen, int flags)
{
	int st = 0;
	if(flags & ~ACCEPT4_VALID_FLAGS)
		return -EINVAL;
	
	file *f = get_socket_fd(sockfd);
	if(!f)
		return -errno;
	
	socket *sock = file_to_socket(f);
	socket_conn_request *req = nullptr;
	socket *new_socket = nullptr;
	inode *inode = nullptr;
	file *newf = nullptr;
	int dflags = 0, fd = -1;

	if(mutex_lock_interruptible(&sock->connection_state_lock) < 0)
	{
		st = -EINTR;
		goto out_no_lock;
	}

	if(!sock->listening())
	{
		st = -EINVAL;
		goto out;
	}

	if(sock->type != SOCK_STREAM)
	{
		st = -EOPNOTSUPP;
		goto out;
	}

	sem_wait(&sock->listener_sem);

	req = dequeue_conn_request(sock);

	new_socket = sock->accept(req);
	free(req);

	if(!new_socket)
	{
		st = -errno;
		goto out;
	}

	inode = socket_create_inode(new_socket);
	if(!inode)
	{
		new_socket->unref();
		st = -errno;
		goto out;
	}

	newf = socket_inode_to_file(inode);
	if(!newf)
	{
		close_vfs(inode);
		st = -errno;
		goto out;
	}

	if(flags & SOCK_CLOEXEC)
		dflags |= O_CLOEXEC;

	/* Open a file descriptor with the socket vnode */
	fd = open_with_vnode(newf, dflags);

	fd_put(newf);

	st = fd;
out:
	mutex_unlock(&sock->connection_state_lock);
out_no_lock:
	fd_put(f);
	return st;
}

extern "C"
int sys_accept(int sockfd, struct sockaddr *addr, socklen_t *slen)
{
	return sys_accept4(sockfd, addr, slen, 0);
}

int socket::getsockopt_socket_level(int optname, void *optval, socklen_t *optlen)
{
	socklen_t length;
	if(copy_from_user(&length, optlen, sizeof(length)) < 0)
		return -EFAULT;

	/* Lessens the dupping of code */
	auto put_opt = [&](const auto &val) -> int
	{
		return put_option(val, length, optlen, optval);
	};

	switch(optname)
	{
		/* TODO: Add more options */
		case SO_ACCEPTCONN:
		{
			int val = (int) listening();
			return put_opt(val);
		}

		case SO_DOMAIN:
		{
			return put_opt(domain);
		}

		case SO_ERROR:
		{
			auto err = sock_err;
			sock_err = 0;
			return put_opt(err);
		}

		case SO_TYPE:
		{
			return put_opt(type);
		}

		case SO_PROTOCOL:
		{
			return put_opt(proto);
		}

		default:
			return -ENOPROTOOPT;
	}
}

int socket::setsockopt_socket_level(int optname, const void *optval, socklen_t optlen)
{
	return -ENOPROTOOPT;
}

extern "C"
int sys_getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen)
{
	file *f = get_socket_fd(sockfd);
	if(!f)
		return -errno;
	
	socket *sock = file_to_socket(f);

	return sock->getsockopt(level, optname, optval, optlen);
}

extern "C"
int sys_setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen)
{
	file *f = get_socket_fd(sockfd);
	if(!f)
		return -errno;
	
	socket *sock = file_to_socket(f);

	return sock->setsockopt(level, optname, optval, optlen);
}
