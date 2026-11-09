// SPDX-License-Identifier: GPL-2.0-only
/*
 * Realtek RTL819x timer / clocksource driver.
 *
 * TC1 is the continuous clocksource. TC0 is the one-shot clockevent.
 * The default protected path uses COUNTER mode and verifies every start.
 * With CONFIG_RTL819X_TC0_DNT, TC0 enters TIMER mode once and every deadline
 * updates DATA0 while EN stays high; there is no runtime mode switch.
 *
 * Field failures had EN and DATA0 correctly visible while COUNT0 was zero.
 * That is consistent with, but does not prove, a missed slow-domain capture
 * of the TC0_EN re-arm edge. DNT avoids that edge and remains opt-in.
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/clockchips.h>
#include <linux/clocksource.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/sched_clock.h>

#include "timer-of.h"

#define DRV_VERSION "1.3"

#define REALTEK_TC_REG_DATA0		0x00
#define REALTEK_TC_REG_DATA1		0x04
#define REALTEK_TC_REG_COUNT0		0x08
#define REALTEK_TC_REG_COUNT1		0x0c
#define REALTEK_TC_REG_CTRL		0x10
#define REALTEK_TC_REG_IR		0x14
#define REALTEK_TC_REG_CLOCK_DIV	0x18
#define REALTEK_TC_CTRL_TC0_EN		BIT(31)
#define REALTEK_TC_CTRL_TC0_TIMER_MODE	BIT(30)
#define REALTEK_TC_CTRL_TC1_EN		BIT(29)
#define REALTEK_TC_CTRL_TC1_TIMER_MODE	BIT(28)
#define REALTEK_TC_IR_TC0_EN		BIT(31)
#define REALTEK_TC_IR_TC1_EN		BIT(30)
#define REALTEK_TC_IR_TC0_PENDING	BIT(29)
#define REALTEK_TC_IR_TC1_PENDING	BIT(28)
#define REALTEK_TIMER_RESOLUTION	28
#define RTLADJ_TICK(x)			((x) >> 4)
#define TC_ENCODE(delta)		((u32)(delta) << 4)
#define TC_MIN_DELTA			8
#define TC_MAX_DELTA			((1U << REALTEK_TIMER_RESOLUTION) - 1)

#define PROTECTED_OBSERVE_EDGES	6
#define PROTECTED_SPIN_CAP		20000
#define PROTECTED_ARM_TRIES		4

static int rtl819x_set_state_shutdown(struct clock_event_device *ced);
static int rtl819x_set_state_oneshot(struct clock_event_device *ced);
static int rtl819x_timer_set_next_event(unsigned long delta,
					struct clock_event_device *ced);
static irqreturn_t rtl819x_timer_interrupt(int irq, void *dev_id);

static struct timer_of to = {
	.flags = TIMER_OF_BASE | TIMER_OF_CLOCK | TIMER_OF_IRQ,
	.of_base = { .name = "rtl819x-timer" },
	.of_clk = { .name = "refclk" },
	.of_irq = { .handler = rtl819x_timer_interrupt, .flags = IRQF_TIMER },
	.clkevt = {
		.name = "rtl819x-timer", .rating = 100,
		.features = CLOCK_EVT_FEAT_ONESHOT,
		.set_next_event = rtl819x_timer_set_next_event,
		.set_state_oneshot = rtl819x_set_state_oneshot,
		.set_state_shutdown = rtl819x_set_state_shutdown,
	},
};

#define tc_w32(val, reg)	writel((val), timer_of_base(&to) + (reg))
#define tc_r32(reg)		readl(timer_of_base(&to) + (reg))

static u64 rtl819x_tc1_count_read(struct clocksource *cs)
{
	return RTLADJ_TICK(tc_r32(REALTEK_TC_REG_COUNT1));
}

static u64 notrace rtl819x_read_sched_clock(void)
{
	return RTLADJ_TICK(tc_r32(REALTEK_TC_REG_COUNT1));
}

static struct clocksource rtl819x_clocksource = {
	.name = "RTL819X counter", .read = rtl819x_tc1_count_read,
	.flags = CLOCK_SOURCE_IS_CONTINUOUS,
};

/* Stop inherited firmware state and force TC0 into protected COUNTER mode. */
static void __init rtl819x_timer_quiesce(void)
{
	u32 val = tc_r32(REALTEK_TC_REG_CTRL);

	val &= ~(REALTEK_TC_CTRL_TC0_EN | REALTEK_TC_CTRL_TC1_EN |
		 REALTEK_TC_CTRL_TC0_TIMER_MODE);
	tc_w32(val, REALTEK_TC_REG_CTRL);
	val = tc_r32(REALTEK_TC_REG_IR);
	val &= ~(REALTEK_TC_IR_TC0_EN | REALTEK_TC_IR_TC1_EN);
	val |= REALTEK_TC_IR_TC0_PENDING | REALTEK_TC_IR_TC1_PENDING;
	tc_w32(val, REALTEK_TC_REG_IR);
}

static int __init rtl819x_clocksource_init(unsigned long freq)
{
	u32 val;
	int ret;

	tc_w32(0xfffffff0, REALTEK_TC_REG_DATA1);
	val = tc_r32(REALTEK_TC_REG_CTRL);
	val |= REALTEK_TC_CTRL_TC1_EN | REALTEK_TC_CTRL_TC1_TIMER_MODE;
	tc_w32(val, REALTEK_TC_REG_CTRL);
	val = tc_r32(REALTEK_TC_REG_IR);
	val |= REALTEK_TC_IR_TC1_PENDING;
	val &= ~REALTEK_TC_IR_TC1_EN;
	tc_w32(val, REALTEK_TC_REG_IR);
	rtl819x_clocksource.rating = 200;
	rtl819x_clocksource.mask = CLOCKSOURCE_MASK(REALTEK_TIMER_RESOLUTION);
	ret = clocksource_register_hz(&rtl819x_clocksource, freq);
	if (ret)
		return ret;
	sched_clock_register(rtl819x_read_sched_clock,
				     REALTEK_TIMER_RESOLUTION, freq);
	return 0;
}

#ifndef CONFIG_RTL819X_TC0_DNT
/* CTRL readback only proves the bus register. COUNT0 proves TC0 started. */
static bool rtl819x_tc0_started(void)
{
	u32 start = tc_r32(REALTEK_TC_REG_COUNT1), now;
	unsigned int spins;

	for (spins = 0; spins < PROTECTED_SPIN_CAP; spins++) {
		if (tc_r32(REALTEK_TC_REG_COUNT0))
			return true;
		now = tc_r32(REALTEK_TC_REG_COUNT1);
		if (((now - start) >> 4) >= PROTECTED_OBSERVE_EDGES)
			return false;
		cpu_relax();
	}
	return false;
}

/* Default path: after a real timeout, issue a fresh EN edge up to four times. */
static void rtl819x_protected_arm(unsigned long delta)
{
	u32 ctrl;
	int try;

	for (try = 0; try < PROTECTED_ARM_TRIES; try++) {
		ctrl = tc_r32(REALTEK_TC_REG_CTRL);
		ctrl &= ~REALTEK_TC_CTRL_TC0_EN;
		tc_w32(ctrl, REALTEK_TC_REG_CTRL);
		tc_w32(TC_ENCODE(delta), REALTEK_TC_REG_DATA0);
		ctrl |= REALTEK_TC_CTRL_TC0_EN;
		tc_w32(ctrl, REALTEK_TC_REG_CTRL);
		if (rtl819x_tc0_started())
			return;
	}
	pr_emerg_ratelimited("rtl819x-timer: TC0 failed to start after %u attempts\n",
			    PROTECTED_ARM_TRIES);
}
#endif

#ifdef CONFIG_RTL819X_TC0_DNT
static bool rtl819x_dnt_active;

/* A zero written to a W1C pending bit preserves a pending min-delta expiry. */
static void rtl819x_dnt_mask_and_ack(void)
{
	u32 ir = tc_r32(REALTEK_TC_REG_IR);

	ir &= ~(REALTEK_TC_IR_TC0_EN | REALTEK_TC_IR_TC1_PENDING);
	ir |= REALTEK_TC_IR_TC0_PENDING;
	tc_w32(ir, REALTEK_TC_REG_IR);
}

/* T1: the only TC0 CTRL write after boot in the DNT lifecycle. */
static void rtl819x_dnt_enter(void)
{
	u32 ctrl;

	rtl819x_dnt_mask_and_ack();
	ctrl = tc_r32(REALTEK_TC_REG_CTRL);
	ctrl |= REALTEK_TC_CTRL_TC0_TIMER_MODE | REALTEK_TC_CTRL_TC0_EN;
	tc_w32(ctrl, REALTEK_TC_REG_CTRL);
	rtl819x_dnt_active = true;
}

/* DATA0 first, then unmask; do not clear a pending event which raced the arm. */
static void rtl819x_dnt_arm(unsigned long delta)
{
	u32 ir;

	tc_w32(TC_ENCODE(delta), REALTEK_TC_REG_DATA0);
	ir = tc_r32(REALTEK_TC_REG_IR);
	ir |= REALTEK_TC_IR_TC0_EN;
	ir &= ~(REALTEK_TC_IR_TC0_PENDING | REALTEK_TC_IR_TC1_PENDING);
	tc_w32(ir, REALTEK_TC_REG_IR);
}
#endif

static int rtl819x_set_state_shutdown(struct clock_event_device *ced)
{
	u32 val;

#ifdef CONFIG_RTL819X_TC0_DNT
	if (rtl819x_dnt_active) {
		/* NO_HZ: leave TIMER|EN intact; only suppress and clear TC0 IRQ. */
		rtl819x_dnt_mask_and_ack();
		return 0;
	}
#endif
	val = tc_r32(REALTEK_TC_REG_CTRL);
	val &= ~REALTEK_TC_CTRL_TC0_EN;
	tc_w32(val, REALTEK_TC_REG_CTRL);
	val = tc_r32(REALTEK_TC_REG_IR);
	val &= ~REALTEK_TC_IR_TC0_EN;
	val |= REALTEK_TC_IR_TC0_PENDING;
	tc_w32(val, REALTEK_TC_REG_IR);
	return 0;
}

static int rtl819x_set_state_oneshot(struct clock_event_device *ced)
{
	u32 val;

#ifdef CONFIG_RTL819X_TC0_DNT
	if (rtl819x_dnt_active) {
		/* Core state callbacks must not reintroduce an EN transition. */
		rtl819x_dnt_mask_and_ack();
		return 0;
	}
#endif
	val = tc_r32(REALTEK_TC_REG_CTRL);
	val &= ~(REALTEK_TC_CTRL_TC0_EN | REALTEK_TC_CTRL_TC0_TIMER_MODE);
	tc_w32(val, REALTEK_TC_REG_CTRL);
	val = tc_r32(REALTEK_TC_REG_IR);
	val |= REALTEK_TC_IR_TC0_EN | REALTEK_TC_IR_TC0_PENDING;
	tc_w32(val, REALTEK_TC_REG_IR);
	return 0;
}

static int rtl819x_timer_set_next_event(unsigned long delta,
					struct clock_event_device *ced)
{
#ifdef CONFIG_RTL819X_TC0_DNT
	if (unlikely(!rtl819x_dnt_active))
		rtl819x_dnt_enter();
	rtl819x_dnt_arm(delta);
#else
	rtl819x_protected_arm(delta);
#endif
	return 0;
}

static irqreturn_t rtl819x_timer_interrupt(int irq, void *dev_id)
{
	struct clock_event_device *ced = dev_id;
	u32 ir = tc_r32(REALTEK_TC_REG_IR);

	if (!(ir & REALTEK_TC_IR_TC0_PENDING))
		return IRQ_NONE;
#ifdef CONFIG_RTL819X_TC0_DNT
	if (rtl819x_dnt_active) {
		/* Mask/W1C before dispatch; DNT ISR never writes CTRL or DATA0. */
		ir &= ~(REALTEK_TC_IR_TC0_EN | REALTEK_TC_IR_TC1_PENDING);
		ir |= REALTEK_TC_IR_TC0_PENDING;
		tc_w32(ir, REALTEK_TC_REG_IR);
	} else
#endif
	{
		ir &= ~REALTEK_TC_IR_TC1_PENDING;
		ir |= REALTEK_TC_IR_TC0_PENDING;
		tc_w32(ir, REALTEK_TC_REG_IR);
	}
	if (likely(ced && ced->event_handler))
		ced->event_handler(ced);
	return IRQ_HANDLED;
}

static int __init rtl819x_timer_init(struct device_node *np)
{
	struct clk *busclk;
	unsigned long bus_rate, timer_rate;
	u32 div_fac, div_readback;
	int ret;

	ret = timer_of_init(np, &to);
	if (ret)
		panic("Failed to init timer_of for %pOF: %d", np, ret);
	to.clkevt.cpumask = cpumask_of(0);
	timer_rate = timer_of_rate(&to);
	rtl819x_timer_quiesce();
	busclk = of_clk_get_by_name(np, "busclk");
	if (IS_ERR(busclk))
		panic("Failed to get timer busclk for %pOF: %ld", np, PTR_ERR(busclk));
	ret = clk_prepare_enable(busclk);
	if (ret)
		panic("Failed to enable timer busclk for %pOF: %d", np, ret);
	bus_rate = clk_get_rate(busclk);
	if (!bus_rate || timer_rate > bus_rate || bus_rate % timer_rate)
		panic("Invalid timer clock rates: bus=%lu timer=%lu", bus_rate, timer_rate);
	div_fac = bus_rate / timer_rate;
	if (!div_fac || div_fac > 0xffff)
		panic("Invalid timer divider: %u", div_fac);
	tc_w32(div_fac << 16, REALTEK_TC_REG_CLOCK_DIV);
	div_readback = tc_r32(REALTEK_TC_REG_CLOCK_DIV);
	if ((div_readback >> 16) != div_fac)
		panic("Timer divider readback failed: expected %u, got %#x", div_fac,
		      div_readback);
	ret = rtl819x_clocksource_init(timer_rate);
	if (ret)
		panic("Failed to register timer clocksource: %d", ret);
	clockevents_config_and_register(&to.clkevt, timer_rate, TC_MIN_DELTA,
					TC_MAX_DELTA);
	pr_info("timer-rtl819x v" DRV_VERSION " (J. Nilo) IRQ:%d CLK:%lu Hz, %s\n",
		timer_of_irq(&to), timer_rate,
#ifdef CONFIG_RTL819X_TC0_DNT
		"DNT enabled (protected until first clockevent)");
#else
		"protected verified arm");
#endif
	return 0;
}

TIMER_OF_DECLARE(rtl819x_timer, "realtek,rtl819x-timer", rtl819x_timer_init);
