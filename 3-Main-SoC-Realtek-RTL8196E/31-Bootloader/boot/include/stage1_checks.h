/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * stage1_checks.h - Pure checks used before stage-1.5 decompresses stage 2
 *
 * This header deliberately has no SoC dependencies.  It is included by the
 * freestanding decompressor and by the native bounds test, so a malformed
 * LZMA size field is never a hardware-only test case.
 */
#ifndef _STAGE1_CHECKS_H_
#define _STAGE1_CHECKS_H_

#include "board.h"

#define STAGE2_LOAD_ADDR 0x80400000UL
#define KSEG1_TO_KSEG0(addr) ((unsigned long)(addr) - 0x20000000UL)

/*
 * The LZMA stream declares its output size.  It is untrusted until this
 * check accepts it: zero is not a runnable stage 2, the configured maximum
 * provides a small blast radius, and the subtraction form cannot wrap.
 */
static inline int stage1_lzma_output_ok(unsigned long out_size)
{
	unsigned long dram_end = KSEG1_TO_KSEG0(BOARD_DRAM_TOP_KSEG1);

	if (out_size == 0 || out_size > BOARD_STAGE2_MAX_SIZE)
		return 0;
	if (STAGE2_LOAD_ADDR >= dram_end)
		return 0;
	return out_size <= dram_end - STAGE2_LOAD_ADDR;
}

#endif /* _STAGE1_CHECKS_H_ */
