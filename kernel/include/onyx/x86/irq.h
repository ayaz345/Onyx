/*
* Copyright (c) 2018 Pedro Falcato
* This file is part of Onyx, and is released under the terms of the MIT License
* check LICENSE at the root directory for more information
*/

#ifndef _X86_IRQ_H
#define _X86_IRQ_H

#include <onyx/registers.h>

#define NR_IRQ 				221
#define PCI_MSI_BASE_ADDRESS 		0xFEE00000
#define PCI_MSI_APIC_ID_SHIFT		12
#define PCI_MSI_REDIRECTION_HINT	(1 << 3)

struct irq_context
{
	registers_t *registers;
};


static inline unsigned long x86_save_flags(void)
{
	unsigned long flags;
	__asm__ __volatile__("pushf; pop %0" : "=rm"(flags) :: "memory");
	return flags;
}

static inline void irq_disable(void)
{
	__asm__ __volatile__("cli");
}

static inline void irq_enable(void)
{
	__asm__ __volatile__("sti");
}

static inline unsigned long irq_save_and_disable(void)
{
	unsigned long old = x86_save_flags();
	irq_disable();

	return old;
}

static inline bool irq_is_disabled(void)
{
	return !(x86_save_flags() & 0x200);
}

static inline void irq_restore(unsigned long flags)
{
	if(flags & 0x200)
		irq_enable();
}

#endif
