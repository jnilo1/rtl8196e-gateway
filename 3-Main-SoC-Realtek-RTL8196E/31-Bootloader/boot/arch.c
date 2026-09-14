// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * arch.c - CPU and cache initialization for RLX4181
 *
 * RTL8196E stage-2 bootloader
 *
 * Copyright (c) 2009-2020 Realtek Semiconductor Corp.
 * Copyright (c) 2024-2026 J. Nilo
 */

#include "boot_common.h"
#include "boot_soc.h"
#include "main.h"

/**
 * init_arch - Entry point from head.S after BSS clear
 *
 * Disables coprocessors 1-3, enables CU0, then calls start_kernel().
 */
asmlinkage void init_arch(void)
{
	unsigned int s;
	/* Disable coprocessors */
	s = read_32bit_cp0_register(CP0_STATUS);
	s &= ~(ST0_CU1 | ST0_CU2 | ST0_CU3 | ST0_KX | ST0_SX);
	s |= ST0_CU0;
	write_32bit_cp0_register(CP0_STATUS, s);
	s = read_32bit_cp0_register(CP0_STATUS);

	start_kernel();
}

/**
 * setup_arch - Enable CPU interrupt lines
 *
 * Clears BEV (Boot Exception Vectors) and enables hardware interrupt
 * lines IRQ0-IRQ5 in the CP0 Status register.
 */
void setup_arch(void)
{
	unsigned long s;
	s = read_32bit_cp0_register(CP0_STATUS);
	s |= ST0_BEV;
	s ^= ST0_BEV;
	s |= IE_IRQ0 | IE_IRQ1 | IE_IRQ2 | IE_IRQ3 | IE_IRQ4 | IE_IRQ5;
	write_32bit_cp0_register(CP0_STATUS, s);
	return;
}

static void _flush_dcache_(void)
{
	/* CCTL_DCACHE_WBINVAL (0x200): write-back and invalidate D-cache */
	__asm__ volatile("mtc0 $0,$20\n\t"
			 "nop\n\t"
			 "li $8,512\n\t" /* 0x200 = CCTL_DCACHE_WBINVAL */
			 "mtc0 $8,$20\n\t"
			 "nop\n\t"
			 "nop\n\t"
			 "mtc0 $0,$20\n\t"
			 "nop"
			 : /* no output */
			 : /* no input */
			 : "$8", "memory");
}

static void _flush_icache_(void)
{
	_flush_dcache_();

	/* CCTL_ICACHE_INVAL (0x2): invalidate I-cache */
	__asm__ volatile("mtc0 $0,$20\n\t"
			 "nop\n\t"
			 "li $8,2\n\t" /* 0x2 = CCTL_ICACHE_INVAL */
			 "mtc0 $8,$20\n\t"
			 "nop\n\t"
			 "nop\n\t"
			 "mtc0 $0,$20\n\t"
			 "nop"
			 : /* no output */
			 : /* no input */
			 : "$8", "memory");
}

/**
 * flush_cache - Write-back and invalidate all caches
 *
 * Flushes the D-cache (write-back + invalidate) then invalidates
 * the I-cache via the Lexra CCTL coprocessor register ($20).
 */
void flush_cache(void)
{
	_flush_dcache_();
	_flush_icache_();
}

/**
 * invalidate_iram - Invalidate the instruction RAM
 *
 * Writes CCTL_IMEM_OFF (0x20) to CP0 register $20, which invalidates
 * the Lexra on-chip IRAM.  Used before jumping to a new kernel image.
 */
void invalidate_iram(void)
{
	__asm__ volatile("mtc0 $0,$20\n\t"
			 "nop\n\t"
			 "nop\n\t"
			 "li $8,0x00000020\n\t" /* CCTL_IMEM_OFF */
			 "mtc0 $8,$20\n\t"
			 "nop\n\t"
			 "nop"
			 : /* no output */
			 : /* no input */
			 : "$8", "memory");
}
