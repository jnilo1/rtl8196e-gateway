// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * fatal.c - Last resort: report, then reset through the watchdog
 *
 * RTL8196E stage-2 bootloader
 *
 * Every error path used to be an infinite loop.  A hang in download mode
 * is permanent until someone power-cycles the board, which on a headless
 * remote site means a visit.  fatal() prints why, gives the operator a few
 * seconds to read it, and lets the hardware watchdog reset the SoC.  The
 * reset preserves DRAM, so the boothold hand-off is cleared first: the
 * board must come back through a normal boot, not loop into the failure.
 *
 * Copyright (c) 2024-2026 J. Nilo
 */

#include "boot_soc.h"
#include "boot_irq.h"
#include "main.h"

/* Bounded busy loop: the timer interrupt may be masked or broken here. */
#define FATAL_PAUSE_LOOPS 200000000U /* a few seconds at 400 MHz */

void fatal(const char *why)
{
	volatile unsigned int d;

	cli();
	prom_printf("\n\nFATAL: %s\nResetting...\n", why ? why : "?");

	for (d = 0; d < FATAL_PAUSE_LOOPS; d++)
		;

	BOOTHOLD_RAM[0] = 0;
	BOOTHOLD_IP_MAGIC_RAM[0] = 0;
	BOOTHOLD_IP_RAM[0] = 0;

	REG32(WDTCNR_REG) = 0; /* arm the watchdog: reset follows */
	for (;;)
		;
}
