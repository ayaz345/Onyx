/*
 * Copyright (c) 2022 Pedro Falcato
 * This file is part of Onyx, and is released under the terms of the MIT License
 * check LICENSE at the root directory for more information
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>

#include <onyx/device_tree.h>
#include <onyx/page.h>
#include <onyx/panic.h>
#include <onyx/serial.h>

namespace device_tree
{

void *fdt_ = nullptr;

#define DEVICE_TREE_MAX_DEPTH 32

int nr_memory_ranges = 0;
size_t memory_size = 0;
unsigned long maxpfn = 0;

/**
 * @brief Process any possible memory reservations in the device tree
 *
 */
void process_reservations()
{
    int nr = fdt_num_mem_rsv(fdt_);

    for (int i = 0; i < nr; i++)
    {
        uint64_t address, size;
        if (int err = fdt_get_mem_rsv(fdt_, i, &address, &size); err < 0)
        {
            panic("device_tree: Error getting memory reservation: %s\n", fdt_strerror(err));
        }

        printf("device_tree: Memory reservation [%016lx, %016lx]\n", address, address + size - 1);

        bootmem_reserve(address, size);
    }

    printf("device_tree: Added %d memory reservations\n", nr);
}

// Taken from fdt_addresses.c since it's useful to us.
// fdt_address_cells and size_cells is not useful since it may break compatibility with
// older/broken device trees
int fdt_get_cells(const void *fdt, int nodeoffset, const char *name)
{
    const fdt32_t *c;
    uint32_t val;
    int len;

    c = (const fdt32_t *) fdt_getprop(fdt, nodeoffset, name, &len);
    if (!c)
        return len;

    if (len != sizeof(*c))
        return -FDT_ERR_BADNCELLS;

    val = fdt32_to_cpu(*c);
    if (val > FDT_MAX_NCELLS)
        return -FDT_ERR_BADNCELLS;

    return (int) val;
}

/**
 * @brief Retrieve a value from a reg field
 *
 */
uint64_t read_reg(const void *reg, int reg_offset, int cell_size)
{
    auto reg32 = (const uint32_t *) ((char *) reg + (reg_offset * sizeof(uint32_t)));
    auto reg64 = (const uint64_t *) ((char *) reg + (reg_offset * sizeof(uint32_t)));

    switch (cell_size)
    {
        case 1: {
            uint32_t ret;
            memcpy(&ret, reg32, sizeof(uint32_t));
            return fdt32_to_cpu(ret);
        }
        case 2: {
            uint64_t ret;
            memcpy(&ret, reg64, sizeof(uint64_t));
            return fdt64_to_cpu(ret);
        }
        default:
            panic("Bogus cell size");
    }
}

/**
 * @brief Gets a property of the node from the device tree
 *
 * @param name Name of the property
 * @param buf Pointer to a buffer
 * @param length Size of the buffer (needs to be the same as the length of the property)
 * @return 0 on success, negative error codes
 */
int node::get_property(const char *name, void *buf, size_t length)
{
    const void *c;
    int len;

    c = (const void *) fdt_getprop(fdt_, offset, name, &len);
    if (!c)
        return len;

    if (len != (int) length)
        return -FDT_ERR_BADLAYOUT;
    memcpy(buf, c, len);

    return 0;
}

/**
 * @brief Handle memory@ nodes in the device tree
 *
 */
void handle_memory_node(int offset, int addr_cells, int size_cells)
{
    int reg_len;
    const void *reg;
    if (reg = fdt_getprop(fdt_, offset, "reg", &reg_len); !reg)
    {
        panic("device_tree: error parsing memory node: %s\n", fdt_strerror(reg_len));
    }

    int nr_ranges = reg_len / ((addr_cells + size_cells) * sizeof(uint32_t));
    unsigned int reg_offset = 0;

    for (int i = 0; i < nr_ranges; i++)
    {
        uint64_t start, size;
        start = read_reg(reg, reg_offset, addr_cells);
        size = read_reg(reg, reg_offset + addr_cells, size_cells);

        bootmem_add_range(start, size);
        memory_size += size;

        maxpfn = cul::max((start + size) >> PAGE_SHIFT, maxpfn);
        base_pfn = cul::min(start >> PAGE_SHIFT, base_pfn);

        reg_offset += addr_cells + size_cells;
    }
}

/**
 * @brief Walk the device tree and look for interesting things
 *
 */
void early_walk()
{
    int address_cell_stack[DEVICE_TREE_MAX_DEPTH];
    int size_cell_stack[DEVICE_TREE_MAX_DEPTH];

    // We need to take special care with #address-cells and #size-cells.
    // The default is 1 for both, but each node inherits the parent's #-cells.
    // Because of that, we have to keep a stack of address and size cells.
    // Because this is early boot code, we don't have access to dynamic memory,
    // so we choose a relatively safe MAX_DEPTH of 32. Hopefully, no crazy device trees come our
    // way.

    // 2 is the default for #address-cells, 1 is the default for #size-cells
    address_cell_stack[0] = fdt_address_cells(fdt_, 0);
    size_cell_stack[0] = fdt_size_cells(fdt_, 0);

    int depth = 0;
    int offset = 0;

    while (true)
    {
        offset = fdt_next_node(fdt_, offset, &depth);

        if (offset < 0 || depth < 0)
            break;

        if (depth >= DEVICE_TREE_MAX_DEPTH)
        {
            printf("device_tree: error: Depth %d exceeds max depth\n", depth);
            return;
        }

        if (depth > 0)
        {
            // Use the parent's cell sizes
            address_cell_stack[depth] = address_cell_stack[depth - 1];
            size_cell_stack[depth] = size_cell_stack[depth - 1];
        }

        // Try to fetch #address-cells and #size-cells

        if (int cells = fdt_get_cells(fdt_, offset, "#address-cells"); cells > 0)
        {
            address_cell_stack[depth] = cells;
        }

        if (int cells = fdt_get_cells(fdt_, offset, "#size-cells"); cells > 0)
        {
            size_cell_stack[depth] = cells;
        }

        const char *name = fdt_get_name(fdt_, offset, NULL);
        if (!name)
            continue;

        if (!strncmp(name, "memory@", strlen("memory@")))
        {
            handle_memory_node(offset, address_cell_stack[depth], size_cell_stack[depth]);
        }
    }
}

/**
 * @brief Initialise the device tree subsystem of the kernel
 *
 * @param fdt Pointer to the flattened device tree
 */
void init(void *fdt)
{
    fdt_ = PHYS_TO_VIRT(fdt);

    if (int error = fdt_check_header(fdt_); error < 0)
    {
        printf("fdt: Bad header: %s\n", fdt_strerror(error));
        return;
    }

    // Reserve the FDT in case the device tree hasn't done that
    bootmem_reserve((unsigned long) fdt, fdt_totalsize(fdt_));

    process_reservations();

    early_walk();

    page_init(memory_size, maxpfn);
}

node *root_node;
/**
 * @brief Get the root dt node
 *
 * @return Pointer to the root node
 */
node *get_root()
{
    return root_node;
}

/**
 * @brief Enumerate the device tree
 *        Note: Requires dynamic memory allocation
 */
void enumerate()
{
    root_node = new node{"", 0, 0};
    if (!root_node)
        panic("Failed to allocate a device tree node");

    int address_cell_stack[DEVICE_TREE_MAX_DEPTH];
    int size_cell_stack[DEVICE_TREE_MAX_DEPTH];
    node *parents[DEVICE_TREE_MAX_DEPTH];

    // 2 is the default for #address-cells, 1 is the default for #size-cells
    address_cell_stack[0] = fdt_address_cells(fdt_, 0);
    size_cell_stack[0] = fdt_size_cells(fdt_, 0);

    int depth = 0;
    int offset = 0;
    parents[0] = root_node;

    while (true)
    {
        offset = fdt_next_node(fdt_, offset, &depth);

        if (offset < 0 || depth < 0)
            break;

        if (depth >= DEVICE_TREE_MAX_DEPTH)
        {
            printf("device_tree: error: Depth %d exceeds max depth\n", depth);
            return;
        }

        if (depth > 0)
        {
            // Use the parent's cell sizes
            address_cell_stack[depth] = address_cell_stack[depth - 1];
            size_cell_stack[depth] = size_cell_stack[depth - 1];
        }

        // Try to fetch #address-cells and #size-cells

        if (int cells = fdt_get_cells(fdt_, offset, "#address-cells"); cells > 0)
        {
            address_cell_stack[depth] = cells;
        }

        if (int cells = fdt_get_cells(fdt_, offset, "#size-cells"); cells > 0)
        {
            size_cell_stack[depth] = cells;
        }

        const char *name_ = fdt_get_name(fdt_, offset, NULL);
        if (!name_)
            continue;

        cul::string name{name_};
        if (!name)
            panic("Failed to allocate memory for the device tree node");

        auto dev_node = new node{cul::move(name), offset, depth, parents[depth - 1]};

        if (!dev_node)
            panic("Failed to allocate a device tree node");

        dev_node->address_cells = address_cell_stack[depth];
        dev_node->size_cells = size_cell_stack[depth];
        if (!parents[depth - 1]->children.push_back(dev_node))
            panic("Failed to allocate memory for the device tree");

        parents[depth] = dev_node;
    }
}

/**
 * @brief Open a device tree node
 *
 * @param path Path of the node
 * @return Pointer to the node
 */
node *open_node(std::string_view path, node *base_node)
{
    size_t pos = 0;
    if (!base_node)
        base_node = root_node;

    if (path[0] == '/')
    {
        pos++;
        base_node = root_node;
    }

    while (pos < path.length())
    {
        auto path_elem_end = path.find('/', pos);
        if (path_elem_end == std::string_view::npos) [[unlikely]]
        {
            path_elem_end = path.length();
        }

        std::string_view v = path.substr(pos, path_elem_end - pos);
        pos += v.length() + 1;

        base_node = base_node->open_node(v);
        if (!base_node)
            return nullptr;
    }

    return base_node;
}
} // namespace device_tree
