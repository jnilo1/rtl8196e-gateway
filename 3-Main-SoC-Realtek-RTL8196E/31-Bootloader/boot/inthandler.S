/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * inthandler.S - Interrupt handler trampoline
 *
 * RTL8196E stage-2 bootloader
 *
 * IRQ_finder: saves all registers (SAVE_ALL), reads the GIMR/GISR
 * interrupt status, and calls irq_dispatch() in irq.c.
 *
 * Copyright (c) 2009-2020 Realtek Semiconductor Corp.
 * Copyright (c) 2024-2026 J. Nilo
 */

#include "boot_asm.h"
#include "boot_soc.h"

/*===========================================================================*/
/* IRQ_finder - Interrupt dispatcher                                         */
/*===========================================================================*/
/*
 * This INT dispatch routine is triggered via Exception->CAUSE:EXC-CODE:INT
 */

	.text
	.set	noreorder
	.set	noat
	.align	5

	NESTED(IRQ_finder, PT_SIZE, sp)

	/*---------------------------------------------------------------------*/
	/* Save context and disable interrupts                                */
	/*---------------------------------------------------------------------*/
	SAVE_ALL
	CLI
	.set	at

	/*---------------------------------------------------------------------*/
	/* Read and mask pending interrupts                                   */
	/*---------------------------------------------------------------------*/
	mfc0	t0, CP0_CAUSE		/* Get pending interrupts */
	nop
	nop

	mfc0	t2, CP0_STATUS
	nop
	nop

	and	t0, t2			/* Isolate allowed ones */
	andi	t2, t0, 0xfc00		/* Look at Hardware INT bits 10-15 */

	/*---------------------------------------------------------------------*/
	/* Read GIMR/GISR interrupt controller registers                      */
	/*---------------------------------------------------------------------*/
	la	t0, (GISR  + 0xb8000000)
	la	t1, (GIMR0 + 0xb8000000)
	lw	a0, 0(t0)		/* a0 = ISR: Interrupt status */
	lw	a1, 0(t1)		/* a1 = IMR: Interrupt mask */
	nop
	nop

	/*---------------------------------------------------------------------*/
	/* Isolate allowed interrupts and check for pending IRQs              */
	/*---------------------------------------------------------------------*/
	and	a0, a1			/* Isolate allowed ones */
	bne	t2, zero, handle_it	/* Pending interrupt -> jump to handler */
	nop
	nop

	/*---------------------------------------------------------------------*/
	/* Spurious interrupt: the line dropped between the exception being  */
	/* committed and the read of Cause (a level source acknowledged by   */
	/* the main loop).  Count it and return; nothing needs servicing.    */
	/*---------------------------------------------------------------------*/
	la	t0, g_spurious_irq
	lw	t1, 0(t0)
	nop
	addiu	t1, t1, 1
	sw	t1, 0(t0)
	move	a1, sp
	RESTORE_ALL_AND_RET
	nop
	nop

	/*---------------------------------------------------------------------*/
	/* Handle valid interrupt                                             */
	/*---------------------------------------------------------------------*/
handle_it:
	jal	irq_dispatch		/* Call C interrupt handler */
	nop
	nop

	move	a1, sp
	RESTORE_ALL_AND_RET		/* Restore context and return */
	nop
	nop

	END(IRQ_finder)

	.set	at

	.data
	.globl	g_spurious_irq
	.align	2
g_spurious_irq:
	.word	0
