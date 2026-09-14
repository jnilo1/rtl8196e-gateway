/* SPDX-License-Identifier: GPL-2.0 */
/*
 * rtl819x_intc_stats.h - live dispatch statistics of the RTL819x INTC.
 *
 * Cross-subsystem contract between the rtl819x irqchip (producer of the INTC
 * fields), the MIPS architecture dispatcher (producer of @ip7) and the rtl819x
 * watchdog (consumer: 1 Hz flight recorder + panic record v8).
 *
 * Motivation (issue #99): the field soft-lockup is interrupt-storm-shaped,
 * but the storming line is unidentified. Two blind spots in the stock
 * accounting drove this header:
 *   - a chained-handler invocation that finds mask&status == 0 dispatches
 *     nothing and increments NO existing counter;
 *   - kstat per-IRQ counts give volume but not recency; per-line last-seen
 *     jiffies answer "who was active during the final 20 s" in a single
 *     subtraction against the jiffies captured at panic.
 *
 * Producer cost: two u32 stores per chained-handler entry plus two per
 * dispatched bit, all to one cacheline-sized BSS struct — negligible next to
 * the MMIO reads the handler already performs.
 *
 * UP platform, written only from the (sole) CPU's hardirq context, read from
 * panic/hrtimer context — plain u32 loads are torn-free on MIPS32.
 */
#ifndef _LINUX_RTL819X_INTC_STATS_H
#define _LINUX_RTL819X_INTC_STATS_H

#include <linux/types.h>

#define RTL819X_INTC_NR_LINES	32	/* GIMR/GISR are one 32-bit register */

struct rtl819x_intc_stats {
	u32 entries;			/* chained-handler invocations */

	/*
	 * Chained entries that found mask&status == 0. This is an AMBIGUOUS
	 * empty entry, not proof of an INTC storm: the one-snapshot design
	 * lets a single parent invocation service every simultaneously
	 * pending bit, after which an already-latched sibling parent enters
	 * the shared handler with nothing left to do. A source can also
	 * deassert between the parent latching and the GIMR/GISR reads.
	 * Only a SUSTAINED rate, correlated with CPU-parent and source state,
	 * is evidence of a storm.
	 */
	u32 empty;

	/*
	 * CPU IP7 assertions, counted by plat_irq_dispatch(). TC0 is the only
	 * IP7 source on this SoC, so this — not count[8] — is the clockevent
	 * interrupt count. Validate against IRQ 7 in /proc/interrupts.
	 */
	u32 ip7;

	/*
	 * Dispatches per GISR bit, counted inside the chained IP2/IP3/IP4
	 * handler only.
	 *
	 * count[8] (TC0) IS NOT A TICK COUNT. TC0 is routed to CPU IP7 and
	 * requested directly by the timer driver, so it never traverses this
	 * handler. The occasional non-zero value is incidental sampling: the
	 * handler entered for another source and happened to observe GISR
	 * bit 8 set. Use @ip7 for clockevent health.
	 */
	u32 count[RTL819X_INTC_NR_LINES];

	/*
	 * Jiffies at last dispatch, PROVIDED THE CLOCKEVENT IS ALIVE. When
	 * the tick dies, jiffies stop advancing while interrupts on other
	 * lines keep arriving, so these stamps cannot measure recency during
	 * exactly that failure. For post-clockevent forensics use a verified
	 * free-running hardware counter, not a jiffies-derived value.
	 */
	u32 last_seen_j[RTL819X_INTC_NR_LINES];
};

/*
 * Strong definition lives in drivers/irqchip/irq-rtl819x.c, which is
 * unconditionally built in on this platform (it is the system INTC).
 */
extern struct rtl819x_intc_stats rtl819x_intc_stats;

#endif /* _LINUX_RTL819X_INTC_STATS_H */
