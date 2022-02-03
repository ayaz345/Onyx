/*
 * Copyright (c) 2017 - 2021 Pedro Falcato
 * This file is part of Onyx, and is released under the terms of the MIT License
 * check LICENSE at the root directory for more information
 *
 * SPDX-License-Identifier: MIT
 */
#include <assert.h>
#include <cpuid.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <onyx/arch.h>
#include <onyx/compiler.h>
#include <onyx/cpu.h>
#include <onyx/vm.h>

static uintptr_t vm_calculate_virtual_address(uintptr_t bits)
{
    /* The bits reported by CPUID are 1-based */
    return -((uintptr_t) 1 << (bits - 1));
}

/* We don't support more than 48-bits(PML5) right now. */

#define VM_SUPPORTED_VM_BITS 48

void arch_vm_init(void)
{
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    assert(__get_cpuid(CPUID_ADDR_SPACE_SIZE, &eax, &ebx, &ecx, &edx) == 1);

    /* Layout of %eax: 7-0 Physical Addr bits implemented;
     * 16-8 Virtual Addr bits implemented, rest is reserved
     */
    uint8_t vm_bits = (uint8_t) (eax >> 8);

    (void) vm_bits;
    vm_update_addresses(vm_calculate_virtual_address(VM_SUPPORTED_VM_BITS));
}

/* Dummy function to keep the kernel happy, since x86 reports every platform
 * memory region as far as I know
 */
bool platform_page_is_used(void *page)
{
    return false;
}

size_t arch_heap_get_size(void)
{
    return 0x200000000000;
}

/* TODO: Is this needed? */
size_t arch_get_initial_heap_size(void)
{
    return 0x400000;
}
