// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * irq.c - Exception and interrupt handling
 *
 * RTL8196E stage-2 bootloader
 *
 * Copyright (c) 2009-2020 Realtek Semiconductor Corp.
 * Copyright (c) 2024-2026 J. Nilo
 */

#include "boot_common.h"
#include "boot_soc.h"
#include "boot_irq.h"
#include "main.h"
#include "ramtest_trace.h"

/* Indexed by CP0 Cause.ExcCode; read by exception_matrix (head.S). */
unsigned long exception_handlers[32];

static struct irqaction *irq_action[NR_IRQS];

#define ALLINTS (IE_IRQ0 | IE_IRQ1 | IE_IRQ2 | IE_IRQ3 | IE_IRQ4 | IE_IRQ5)

static void unmask_irq(unsigned int irq)
{
	REG32(GIMR_REG) |= 1 << irq;
	REG32(GIMR_REG); /* read back: the write has reached the controller */
}

extern asmlinkage void do_IRQ(int irq, struct pt_regs *regs);

/**
 * irq_dispatch - Walk pending IRQ bits and dispatch handlers
 * @irq_nr: bitmask of pending interrupt lines (from GIMR & GISR)
 * @regs: saved CPU register state
 *
 * Scans all 32 bits of @irq_nr; for each set bit, calls do_IRQ()
 * with the corresponding IRQ number.
 */
void irq_dispatch(int irq_nr, struct pt_regs *regs)
{
	int i, irq = 0;
	for (i = 0; i <= 31; i++) {
		if (irq_nr & 0x01) {
			do_IRQ(irq, regs);
		}
		irq++;
		irq_nr = irq_nr >> 1;
	}
}

static inline unsigned int clear_cp0_status(unsigned int clear)
{
	unsigned int res;

	res = read_32bit_cp0_register(CP0_STATUS);
	res &= ~clear;
	write_32bit_cp0_register(CP0_STATUS, res);

	return res;
}

static inline unsigned int change_cp0_status(unsigned int change,
					     unsigned int newvalue)
{
	unsigned int res;

	res = read_32bit_cp0_register(CP0_STATUS);
	res &= ~change;
	res |= (newvalue & change);
	write_32bit_cp0_register(CP0_STATUS, res);

	return res;
}

static void set_except_vector(int n, void *addr)
{
	exception_handlers[n] = (unsigned long)addr;
}

static void ExceptionToIrq_setup(void)
{
	extern asmlinkage void IRQ_finder(void);

	/* Disable all hardware interrupts */
	change_cp0_status(ST0_IM, 0x00);

	/* Set up the external interrupt exception vector */
	/* First exception is Interrupt*/
	set_except_vector(0, IRQ_finder);

	/* Enable all interrupts */
	change_cp0_status(ST0_IM, ALLINTS);
}

void init_IRQ(void) { ExceptionToIrq_setup(); }

/**
 * request_IRQ - Register and enable an interrupt handler
 * @irq: GIMR line (0 to NR_IRQS-1)
 * @action: irqaction describing the handler
 * @dev_id: device identifier passed to the handler
 *
 * Return: 0 on success, -EINVAL if @irq is out of range
 */
int request_IRQ(unsigned long irq, struct irqaction *action, void *dev_id)
{
	unsigned long flags;

	if (irq >= NR_IRQS)
		return -EINVAL;

	action->dev_id = dev_id;

	save_and_cli(flags);
	irq_action[irq] = action;
	restore_flags(flags);

	unmask_irq(irq);
	return 0;
}

/**
 * do_IRQ - Dispatch a single hardware interrupt
 * @irqnr: IRQ number (0-31)
 * @regs: saved CPU register state
 *
 * Looks up the registered irqaction for @irqnr and calls its handler.
 * An interrupt with no handler is a configuration error: report and reset.
 */
asmlinkage void do_IRQ(int irqnr, struct pt_regs *regs)
{
	struct irqaction *action;

	action = irq_action[irqnr];

	if (action) {
		action->handler(irqnr, action->dev_id, regs);
	} else {
		rt_set(RT_EXC_CAUSE, read_32bit_cp0_register(CP0_CAUSE));
		rt_set(RT_EXC_EPC, read_32bit_cp0_register(CP0_EPC));
		rt_set(RT_EXC_BADVA, irqnr);
		prom_printf("cp0_cause=%X, cp0_epc=%X, irq=%d\n",
			    read_32bit_cp0_register(CP0_CAUSE),
			    read_32bit_cp0_register(CP0_EPC), irqnr);
		fatal("unregistered interrupt");
	}
}

/*
 * Every exception other than an interrupt lands here, straight from
 * exception_matrix (head.S) — which jumps to the handler without saving a
 * register frame, so @regs is meaningless and must not be touched: reading
 * through it raised a second exception inside the first, and the board
 * hung silently instead of reporting.  Only CP0 is consulted.
 */
asmlinkage void do_reserved(struct pt_regs *regs)
{
	rt_set(RT_EXC_CAUSE, read_32bit_cp0_register(CP0_CAUSE));
	rt_set(RT_EXC_EPC, read_32bit_cp0_register(CP0_EPC));
	rt_set(RT_EXC_BADVA, read_32bit_cp0_register(CP0_BADVADDR));
	prom_printf("cp0_cause=%X, cp0_epc=%X, badvaddr=%X\n",
		    read_32bit_cp0_register(CP0_CAUSE),
		    read_32bit_cp0_register(CP0_EPC),
		    read_32bit_cp0_register(CP0_BADVADDR));
	fatal("unhandled exception");
}

/**
 * exception_init - Install exception handlers at KSEG0+0x80
 *
 * Clears BEV in CP0 Status, fills all 32 exception slots with
 * do_reserved(), and copies the exception_matrix dispatcher to the
 * hardware vector address (KSEG0 + 0x80).
 */
extern char exception_matrix[];
void exception_init(void)
{
	unsigned long i;

	clear_cp0_status(ST0_BEV);
	for (i = 0; i <= 31; i++)
		set_except_vector(i, do_reserved);
	/* BEV=0: the general exception vector is KSEG0 + 0x80 (cacheable). */
	memcpy((void *)(KSEG0 + 0x80), &exception_matrix, 0x80);
	flush_cache();
}
