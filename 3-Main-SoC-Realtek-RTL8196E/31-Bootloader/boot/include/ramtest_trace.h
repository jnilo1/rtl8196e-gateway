/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ramtest_trace.h - RAM-test build only: watchdog kick from the main loop
 * and a breadcrumb record that survives the watchdog reset.
 *
 * Compiles to nothing in the production build (every macro is a no-op
 * without RAMTEST_TRACE), so the flash image is byte-identical.
 *
 * The record lives in the upper half of the kernel watchdog crash-record
 * page (DRAM top - 0x3000 + 0x800): above RAM_RESERVED_TOP, so no upload
 * or kernel copy can reach it, below the boothold page, and the kernel's
 * panicrec service only looks at the "PANC" magic at offset 0.  Written
 * through KSEG1 so nothing stays in the write-back D-cache.  The magic is
 * published last, after the fields are zeroed.
 *
 * Read it back from the flashed loader after the reset:
 *     DW A1FFD800 10
 */
#ifndef _RAMTEST_TRACE_H_
#define _RAMTEST_TRACE_H_

#ifdef RAMTEST_TRACE

#include "board.h"
#include "boot_soc.h"

#define RT_PAGE (BOARD_DRAM_TOP_KSEG1 - 0x3000 + 0x800)
#define RT_MAGIC 0x52544D42 /* "RTMB" */

enum rt_word {
	RT_MAGIC_W = 0,
	RT_ISR_CNT,	/* eth_interrupt() entries */
	RT_ISR_STATUS,	/* last CPUIISR seen by the ISR */
	RT_RUNOUT_CNT,	/* RUNOUT status seen by swNic_receive() */
	RT_RUNOUT_IMR,	/* CPUIIMR when the RUNOUT mask was enabled */
	RT_BLOCK,	/* TFTP block being handled */
	RT_ADDR,	/* its destination */
	RT_PHASE,	/* see RT_PHASE_* */
	RT_EXC_CAUSE,	/* CP0 Cause of the last reported exception */
	RT_EXC_EPC,
	RT_EXC_BADVA,
	RT_SPURIOUS,	/* copy of g_spurious_irq */
	RT_KICKS,	/* watchdog kicks from the main loop */
	RT_JIFFIES,	/* jiffies at the last kick */
	RT_NWORDS
};

#define RT_PHASE_IDLE 1	 /* eth_poll() entered, ring empty so far */
#define RT_PHASE_FRAME 2 /* a frame handed to kick_tftpd() */
#define RT_PHASE_DATA 3	 /* prepareACK(): block accepted, before memcpy */
#define RT_PHASE_COPIED 4 /* after memcpy */
#define RT_PHASE_ACKED 5  /* after the ACK was queued */

#define RT_W(i) (((volatile unsigned int *)RT_PAGE)[i])
#define rt_set(i, v) (RT_W(i) = (unsigned int)(v))
#define rt_inc(i) (RT_W(i)++)

/*
 * Watchdog: OVSEL 9 as the kernel driver codes it (the period is measured
 * on the bench, not derived).  The counter runs even while halted, so the
 * arm sequence halts and clears first; a kick is the same constant, with
 * WDTCLR set, written without a read-modify-write.
 */
#define RT_WDT_ARM 0x00A40000
#define RT_WDT_HALT 0xA5A40000

static inline void rt_wdt_kick(void)
{
	REG32(WDTCNR_REG) = RT_WDT_ARM;
	rt_inc(RT_KICKS);
}

static inline void rt_init(void)
{
	int i;

	for (i = 0; i < RT_NWORDS; i++)
		rt_set(i, 0);
	rt_set(RT_MAGIC_W, RT_MAGIC);
	REG32(WDTCNR_REG) = RT_WDT_HALT;
	REG32(WDTCNR_REG) = RT_WDT_ARM;
}

#else /* !RAMTEST_TRACE */

#define rt_set(i, v) ((void)0)
#define rt_inc(i) ((void)0)
#define rt_wdt_kick() ((void)0)
#define rt_init() ((void)0)

#endif /* RAMTEST_TRACE */

#endif /* _RAMTEST_TRACE_H_ */
