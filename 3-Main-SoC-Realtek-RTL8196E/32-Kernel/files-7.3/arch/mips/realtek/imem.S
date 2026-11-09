/* SPDX-License-Identifier: GPL-2.0 */
/*
 * imem.S — On-chip I-MEM / D-MEM initialisation for RTL8196E (Lexra RLX4181).
 *
 * The RTL8196E SoC contains two tightly-coupled on-chip SRAMs accessed
 * via the Lexra COP3 coprocessor:
 *
 *   I-MEM  16 KB  physical 0x00C00000–0x00C03FFF  (instruction SRAM)
 *   D-MEM   8 KB  physical 0x00C04000–0x00C05FFF  (data SRAM)
 *
 * _imem_dmem_init() programs the COP3 "Instruction Window" (IW) and
 * "Data Window" (DW) registers to point at the .iram / .dram sections,
 * then triggers IRAM Fill so the hardware caches those ranges in on-chip
 * SRAM.  Subsequent instruction / data fetches from those physical
 * addresses are served at zero wait-state instead of going to external
 * SDRAM.
 *
 * Called once during early boot from plat_mem_setup() in setup.c.
 *
 * Production status (May 2026):
 *   - The I-MEM path is in production: ~6 KB of __iram-marked hot path
 *     code is pinned in on-chip SRAM at boot (see imem.h, .iram section
 *     in vmlinux.lds.S).
 *   - The D-MEM path is dormant: the v3.4.0 IRAM PoC measured -4 % UDP
 *     TX when ring metadata was placed in DMEM (the COP3 DW indirection
 *     costs more than on-chip SRAM saves, since the data was already
 *     cache-resident on this hardware).  No source carries __dram or
 *     __dram_poc annotations, so the .dram linker section stays empty,
 *     __dram_start == __dram_end at boot, and the DMEM block below
 *     short-circuits via skip_dram.
 *   - The DMEM init sequence is kept here as the documented reference
 *     for the 4-stage SDRAM->DMEM copy, in case future work needs it.
 *     Re-enabling DMEM requires (1) re-introducing a __dram macro in
 *     imem.h, (2) annotating data with it, and (3) verifying the SDRAM
 *     range immediately after .dram is still kernel-image-resident
 *     (stage (c) below relies on that).  The fix that landed in
 *     abe7f19 ("fix(imem): correct IMEM/DMEM init") corrected two
 *     latent bugs in the original BSP-derived implementation; the
 *     correct sequence below is what survived.
 *
 * COP3 register assignments (RTL8196E):
 *   $0  IW base   (physical address of .iram section, 16 KB aligned)
 *   $1  IW top    (base + 16 KB − 1)
 *   $2  IW base1  (not used: single bank on RTL8196E)
 *   $3  IW top1   (not used)
 *   $4  DW base   (physical address of .dram section, 8 KB aligned)
 *   $5  DW top    (base + 8 KB − 1)
 *   $6  DW base1  (not used)
 *   $7  DW top1   (not used)
 *
 * COP0 $20 (CCTL) bit definitions used here:
 *   bit  1  Invalidate D-cache
 *   bit  4  IRAM Fill  (copy SDRAM range into on-chip SRAM)
 *   bit  5  IRAM Off   (invalidate SRAM)
 *   bit  9  Invalidate I-cache
 *   bit 10  DMEM On    (route DW range to on-chip D-MEM)
 *   bit 11  DMEM Off   (un-route DW range without invalidating D-MEM)
 *
 * Register usage: $8 ($t0), $9 ($t1) — caller-saved, no push/pop needed.
 */

#include <asm/asmmacro.h>

	.set	nomips16		/* this file is MIPS32; called from MIPS32 */
	.text

LEAF(_imem_dmem_init)
	.set	noreorder

	/* ------------------------------------------------------------------
	 * 1. Enable COP3 access in the MIPS Status register (COP0 $12).
	 *    The Lexra COP3 coprocessor implements the SRAM window control.
	 * ------------------------------------------------------------------ */
	mfc0	$8, $12
	nop
	nop
	or	$8, 0x80000000		/* set CU3 bit */
	mtc0	$8, $12
	nop
	nop

	/* ------------------------------------------------------------------
	 * 2. Invalidate the IRAM (0 → 1 transition on the IRAM Off bit).
	 * ------------------------------------------------------------------ */
	mtc0	$0, $20			/* CCTL = 0 */
	nop
	nop
	li	$8, 0x00000020		/* bit 5: IRAM Off */
	mtc0	$8, $20
	nop
	nop

	/* ------------------------------------------------------------------
	 * 3. Invalidate I-cache and D-cache (0 → 1 transition).
	 * ------------------------------------------------------------------ */
	mtc0	$0, $20			/* CCTL = 0 */
	nop
	nop
	li	$8, 0x00000202		/* bit 9: inv I-cache, bit 1: inv D-cache */
	mtc0	$8, $20
	nop
	nop

	/* ------------------------------------------------------------------
	 * 4. Program I-MEM window in COP3.
	 *
	 *    __iram is the virtual address of the .iram section in SDRAM.
	 *    Strip KSEG0/KSEG1 bits → physical address (bits [27:14]).
	 *    The hardware will cache this 16 KB SDRAM range in on-chip SRAM.
	 *
	 *    Skip if __iram == __iram_tail (no .iram annotations active).
	 *    With CONFIG_RTL8196E_IMEM_DEFAULT_PLACEMENT=n and the IRAM
	 *    PoC running DMEM-only, .iram is empty and the linker placed
	 *    .dram immediately after it (same physical address).  Programming
	 *    the IW window here would cause the IW and DW windows to overlap
	 *    on the same physical SDRAM range, which yields undefined
	 *    hardware behavior and bricks the boot.
	 * ------------------------------------------------------------------ */
	la	$8, __iram
	la	$9, __iram_tail
	beq	$8, $9, skip_iram
	nop
	la	$9, 0x0fffc000		/* mask to 16 KB-aligned physical bits */
	and	$8, $8, $9
	mtc3	$8, $0			/* COP3 $0 = IW base */
	nop
	nop
	addiu	$8, $8, 0x3fff		/* base + 16 KB − 1 */
	mtc3	$8, $1			/* COP3 $1 = IW top */
	nop
	nop

	/* ------------------------------------------------------------------
	 * 5. IRAM Fill: hardware caches the IW range from SDRAM into SRAM.
	 *    After this, instruction fetches from __iram..__iram+16KB
	 *    are served at zero wait-state.
	 * ------------------------------------------------------------------ */
	mtc0	$0, $20			/* CCTL = 0 */
	nop
	nop
	li	$8, 0x00000010		/* bit 4: IRAM Fill */
	mtc0	$8, $20
	nop
	nop

skip_iram:

	/* ------------------------------------------------------------------
	 * 6. Program D-MEM window in COP3 — 4-stage BSP sequence.
	 *
	 *    Unlike I-MEM, D-MEM has no hardware "Fill" mechanism.  At
	 *    boot the on-chip 8 KB SRAM is uninitialised; simply pointing
	 *    the DW window at __dram and flipping bit 10 ("DMEM On") would
	 *    leave reads returning garbage and starve the kernel of valid
	 *    .dram content.  The Realtek SDK works around this with a
	 *    four-stage trick (see arch/rlx/soc-rtl8196e/bspinit.h in the
	 *    rtl819x SDK):
	 *
	 *      a) point DW at (__dram + 8 KB) so writes there go to DMEM
	 *         while reads from __dram still go to SDRAM
	 *      b) DMEM On (bit 10)
	 *      c) word-by-word backward copy 8 KB from SDRAM[__dram .. +8K)
	 *         to virt[__dram+8K .. __dram+16K)  (writes routed to DMEM)
	 *      d) DMEM Off (bit 11)
	 *      e) point DW at __dram, DMEM On
	 *
	 *    After (e), accesses to __dram..__dram+8K go to on-chip D-MEM
	 *    which now holds the original .dram payload.
	 *
	 *    Skip the whole block if __dram_start == __dram_end (no .dram
	 *    annotations active).
	 *
	 *    NOTE: this requires that the 8 KB SDRAM range immediately
	 *    after __dram is part of the kernel image and therefore valid
	 *    to access.  The linker places .data right after .dram so this
	 *    holds.  The temporary writes during stage (c) go ONLY to DMEM
	 *    (the DW shadow makes the SDRAM behind it untouched).
	 * ------------------------------------------------------------------ */
#define DMEM0_SIZE 0x2000		/* 8 KB */

	la	$8, __dram_start
	la	$9, __dram_end
	beq	$8, $9, skip_dram
	nop

	/* (a) DW base = (__dram + 8 KB), masked to 8 KB-aligned phys */
	la	$8, __dram_start
	addiu	$8, $8, DMEM0_SIZE
	la	$9, 0x0fffe000
	and	$8, $8, $9
	mtc3	$8, $4			/* DW base */
	nop
	nop
	addiu	$8, $8, (DMEM0_SIZE - 1)
	mtc3	$8, $5			/* DW top  */
	nop
	nop

	/* (b) DMEM On (CCTL bit 10) — front-edge triggered */
	mtc0	$0, $20
	nop
	nop
	li	$8, 0x00000400
	mtc0	$8, $20
	nop
	nop

	/* (c) Backward word copy:
	 *     SDRAM[__dram + 8K - 4]   →  DMEM[__dram + 16K - 4]
	 *     SDRAM[__dram + 8K - 8]   →  DMEM[__dram + 16K - 8]
	 *     ...
	 *     SDRAM[__dram + 0]        →  DMEM[__dram + 8K]
	 *
	 *     $10 = remaining bytes (= 8 KB)
	 *     $8  = dst pointer (in DMEM-mapped virt)
	 *     $9  = src pointer (in SDRAM)
	 *     $12 = scratch word
	 */
	li	$10, DMEM0_SIZE
	la	$8, __dram_start
	addiu	$8, $8, (DMEM0_SIZE + DMEM0_SIZE - 4)	/* dst start */
	la	$9, __dram_start
	addiu	$9, $9, (DMEM0_SIZE - 4)		/* src start */
1:
	lw	$12, 0($9)
	addiu	$9, $9, -4
	addiu	$10, $10, -4
	sw	$12, 0($8)
	bnez	$10, 1b
	addiu	$8, $8, -4		/* delay slot */
	nop

	/* (d) DMEM Off (CCTL bit 11) */
	mtc0	$0, $20
	nop
	nop
	li	$8, 0x00000800
	mtc0	$8, $20
	nop
	nop

	/* (e) DW base = __dram, then DMEM On */
	la	$8, __dram_start
	la	$9, 0x0fffe000
	and	$8, $8, $9
	mtc3	$8, $4
	nop
	nop
	addiu	$8, $8, (DMEM0_SIZE - 1)
	mtc3	$8, $5
	nop
	nop
	mtc0	$0, $20
	nop
	nop
	li	$8, 0x00000400
	mtc0	$8, $20
	nop
	nop

skip_dram:
	/* ------------------------------------------------------------------
	 * 7. Clear CCTL — re-enables normal cache operation.
	 *    The COP3 window registers remain active; the on-chip SRAM now
	 *    serves instruction fetches from the programmed ranges.
	 * ------------------------------------------------------------------ */
	mtc0	$0, $20			/* CCTL = 0 */
	nop
	nop

	.set	reorder
	j	$31			/* return to caller */
END(_imem_dmem_init)
