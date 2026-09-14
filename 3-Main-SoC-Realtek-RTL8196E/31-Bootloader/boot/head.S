/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * head.S - MIPS entry point, cache/IRAM init, exception dispatch
 *
 * RTL8196E stage-2 bootloader
 *
 * Initializes CP0 status/cause, flushes and refills caches and IRAM,
 * clears BSS, sets up the stack, then jumps to init_arch().  Also
 * contains the write-buffer flush routine, and the exception_matrix
 * dispatcher that reads CP0_CAUSE and dispatches to the registered
 * exception handler.
 *
 * Copyright (C) 1994, 1995 Waldorf Electronics
 * Copyright (C) 1995-1999 Ralf Baechle
 * Copyright (C) 1996 Paul M. Antoine
 * Copyright (C) 1999 Silicon Graphics, Inc.
 * Copyright (c) 2009-2020 Realtek Semiconductor Corp.
 * Copyright (c) 2024-2026 J. Nilo
 */

#include "boot_asm.h"

/*===========================================================================*/
/* Startup code section                                                      */
/*===========================================================================*/
__INIT
	.text

	/* Jump to initialization routine */
	b	check_self_exist
	/* b	after_move */

/*---------------------------------------------------------------------------*/
/* Exception handler reservation                                             */
/*---------------------------------------------------------------------------*/
	/*
	 * Reserved space for exception handlers.
	 * Necessary for machines which link their kernels at KSEG0.
	 * FIXME: Use the initcode feature to get rid of unused handler
	 * variants.
	 */
	.fill	0x400

/*===========================================================================*/
/* start - Main entry point                                                 */
/*===========================================================================*/
	NESTED(start, 16, sp)
	.set	noreorder

	/* Kernel profiling symbols */
	EXPORT(stext)
	EXPORT(_stext)

/*---------------------------------------------------------------------------*/
check_self_exist:
	/* #include "weiboot.s" */
	/* #include "uart.s" */

after_move:

	/*---------------------------------------------------------------------*/
	/* Initial status and cause registers                                 */
	/*---------------------------------------------------------------------*/
	/* Initial code from 8672 ADSL project */
	
	/* Initialize status register */
	mtc0	zero, CP0_STATUS		/* Load status register */
	mfc0	t1, CP0_STATUS
	nop
	srl	t1, t1, 0x2
	sll	t1, t1, 0x2			/* Clear KUc and IEc */
	mtc0	t1, CP0_STATUS			/* Set status register */

	/* Clear cause register */
	mfc0	t1, CP0_CAUSE
	mtc0	zero, CP0_CAUSE			/* Clear cause register */

	/*---------------------------------------------------------------------*/
	/* Cache and write buffer initialization                              */
	/*---------------------------------------------------------------------*/
	jal	_rom_flush_cache
	nop

	/* Flush the write buffer */
	jal	_rom_wbflush
	nop

	/*---------------------------------------------------------------------*/
	/* Stack pointer initialization                                       */
	/*---------------------------------------------------------------------*/
	/*
	 * RAMTEST_TRACE: build-time flag for the RAM test image
	 * (scripts/build_ramtest.sh).  When set, three things change:
	 *
	 * 1. Stack init is deferred until after BSS clear (below), so
	 *    the read-after-write BSS loop can run without a valid sp.
	 * 2. BSS clear uses a read-back loop (sw then lw) to verify
	 *    each write reaches RAM correctly.
	 * 3. After BSS clear, a kseg1-to-kseg0 trampoline switches
	 *    execution to cached memory before entering init_arch.
	 */
#ifndef RAMTEST_TRACE
	/* Setup stack pointer */
	la	$28, init_task_union
	addiu	t0, $28, SYS_STACK_SIZE - 32
	subu	sp, t0, 4 * SZREG
	sw	t0, kernelsp
#endif

	/*---------------------------------------------------------------------*/
	/* Initialize and start COP3 (CP3 - Coprocessor 3)                   */
	/*---------------------------------------------------------------------*/
	mfc0	$4, CP0_STATUS
	nop
	or	$4, 0x80000000
	mtc0	$4, CP0_STATUS
	nop
	nop

	/*---------------------------------------------------------------------*/
	/* Clear BSS section                                                  */
	/*---------------------------------------------------------------------*/
	/*
	 * The firmware/bootloader passes argc/argp/envp
	 * to us as arguments.  But clear bss first because
	 * the romvec and other important info is stored there
	 * by prom_init().
	 */

#ifdef RAMTEST_TRACE
	/* RAMTEST: write-then-read-back BSS clear to detect RAM faults */
	la	t0, _edata
	la	t1, (_end - 4)
1:
	sw	zero, (t0)
	lw	t9, (t0)
	nop
	nop
	beq	t0, t1, 2f
	nop
	addiu	t0, 4
	b	1b
	nop
2:
#else
	/* Standard BSS clear */
	la	t0, _edata			/* la t0, _fbss */
	sw	zero, (t0)
	la	t1, (_end - 4)
1:
	addiu	t0, 4
	bne	t0, t1, 1b
	sw	zero, (t0)
#endif

#ifdef RAMTEST_TRACE
	/*---------------------------------------------------------------------*/
	/* RAMTEST: deferred stack init + kseg1-to-kseg0 transition           */
	/*---------------------------------------------------------------------*/
	la	$28, init_task_union
	addiu	t0, $28, SYS_STACK_SIZE - 32
	subu	sp, t0, 4 * SZREG
	sw	t0, kernelsp

	/*--- Transition to kseg0 (cached) ---
	 * btcode jumps to DECOMP_ADDR|0xA0000000 in ramtest mode,
	 * so everything runs from kseg1 (uncached) until we switch.
	 * la loads the link-time (kseg0) address via lui+ori.
	 */
	la	t0, 1f
	jr	t0
	nop
1:
#endif

	/*---------------------------------------------------------------------*/
	/* Jump to C entry point                                              */
	/*---------------------------------------------------------------------*/
	jal	init_arch
	nop
	nop

	END(start)

/*===========================================================================*/
/* _rom_flush_cache - Flush and refill caches and IRAM                      */
/*===========================================================================*/
	.text
	LEAF(_rom_flush_cache)
	.set	noreorder

	/*---------------------------------------------------------------------*/
	/* Transition to kseg1 from undetermined kernel segment               */
	/*---------------------------------------------------------------------*/
	la	$9, 2f
	or	$9, 0xa0000000
	jr	$9
	.set	noreorder

2:
	/*---------------------------------------------------------------------*/
	/* Initialize and start COP3                                          */
	/*---------------------------------------------------------------------*/
	mfc0	$8, $12			/* CP0_STATUS */
	nop
	nop
	or	$8, 0x80000000
	mtc0	$8, $12
	nop
	nop

	/*---------------------------------------------------------------------*/
	/* Invalidate IRAM with a 0->1 transition                             */
	/*---------------------------------------------------------------------*/
	mtc0	$0, $20			/* CCTL - Cache Control Register */
	nop
	nop
	li	$8, CCTL_IMEM_OFF		/* invalidate IRAM */
	mtc0	$8, $20
	nop
	nop

	/*---------------------------------------------------------------------*/
	/* Invalidate I-cache and D-cache with a 0->1 transition              */
	/*---------------------------------------------------------------------*/
	mtc0	$0, $20			/* CCTL */
	nop
	nop
	li	$8, CCTL_DCACHE_WBINVAL | CCTL_ICACHE_INVAL
	mtc0	$8, $20
	nop
	nop

	/* DDR calibration requires IRAM */
	/*---------------------------------------------------------------------*/
	/* Load IRAM base and top                                             */
	/*---------------------------------------------------------------------*/
	la	$8, __iram
	la	$9, 0x0ffffc00
	and	$8, $8, $9
	mtc3	$8, CP3_IWBASE		/* IW base */
	nop
	nop
	addiu	$8, $8, 0xfff
	mtc3	$8, CP3_IWTOP		/* IW top */
	nop
	nop

	/*---------------------------------------------------------------------*/
	/* Refill IRAM with a 0->1 transition                                 */
	/*---------------------------------------------------------------------*/
	mtc0	$0, $20			/* CCTL */
	nop
	nop
	li	$8, CCTL_IMEM_FILL		/* refill IRAM from flash */
	mtc0	$8, $20
	nop
	nop

	/*---------------------------------------------------------------------*/
	/* Enable I-cache and D-cache (clear CCTL)                           */
	/*---------------------------------------------------------------------*/
	mtc0	$0, $20			/* CCTL = 0: caches active */
	nop
	nop

	.set	reorder
	j	$31
	nop

	END(_rom_flush_cache)

/*===========================================================================*/
/* _rom_wbflush - Write buffer flush routine                                */
/*===========================================================================*/
	LEAF(_rom_wbflush)

_rom_wbflush:
	li	$8, 0xa0000000		/* Load uncached address */
	lw	$8, 0($8)		/* Read to force write buffer flush */
	j	$31			/* Return to caller */
	nop

	END(_rom_wbflush)

/*===========================================================================*/
/* exception_matrix - Exception vector dispatcher                           */
/*===========================================================================*/
	/*
	 * General exception vector.
	 * TLB refill, EXL == 0, R[23]00 version
	 *
	 * This is the direction of exception toward their handlers.
	 * Cyrus Tsai
	 */

	.set	at
	.set	reorder

	NESTED(exception_matrix, 0, sp)

	mfc0	k1, CP0_CAUSE		/* Read cause register */
	nop
	nop
	la	k0, exception_handlers	/* Load handler table base */
	andi	k1, k1, 0x7c		/* Mask cause bits */
	addu	k0, k0, k1		/* Index into table */
	lw	k0, (k0)		/* Load handler address */
	jr	k0			/* Jump to handler */
	nop
	nop

	END(exception_matrix)

/*
 * This buffer is reserved for the use of the cache error handler.
 */

__FINIT
