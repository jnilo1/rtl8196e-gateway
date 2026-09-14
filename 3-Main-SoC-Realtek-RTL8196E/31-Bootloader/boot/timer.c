// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * timer.c - Timer 0 tick (10 ms) and the delays built on it
 *
 * RTL8196E stage-2 bootloader
 *
 * jiffies only advances from the timer interrupt (IRQ 8).  delay_ms()
 * therefore needs interrupts enabled and IRQ 8 unmasked in GIMR; with
 * interrupts masked it spins forever.  Nothing in the loader calls it
 * from interrupt context any more (the TFTP server and the flash writer
 * run in the main loop), and goToDownMode() re-arms the tick before the
 * switch driver first needs a delay.
 *
 * Copyright (c) 2009-2020 Realtek Semiconductor Corp.
 * Copyright (c) 2024-2026 J. Nilo
 */

#include "boot_irq.h"
#include "boot_common.h"
#include "boot_soc.h"
#include "main.h"

#define TC0_IRQ 8 /* GIMR bit of timer/counter 0 */

static void timer_interrupt(int num, void *ptr, struct pt_regs *reg);
static struct irqaction irq_timer = {timer_interrupt, 0, TC0_IRQ, "timer",
				     NULL, NULL};
static volatile unsigned int jiffies = 0;

static void timer_interrupt(int num, void *ptr, struct pt_regs *reg)
{
	REG32(TCIR_REG) = (1 << 31) | (1 << 29); /* TC0IE + TC0IP(W1C) */
	jiffies++;
}

int get_timer_jiffies(void) { return jiffies; }

/**
 * timer_init - Program Timer 0 for a 10 ms periodic interrupt
 * @lexra_clock: bus clock frequency in Hz (the timer input)
 *
 * Stops the timer first: on a RAM test the flash bootloader's tick is
 * still running when this code starts.
 */
void timer_init(unsigned long lexra_clock)
{
#define DIVISOR 0xE
#define DIVF_OFFSET 16
#define TICK_FREQ 100 /* 100 Hz = 10 ms */
	int c;

	REG32(TCCNR_REG) = 0;
	REG32(TCIR_REG) = (1 << 31) | (1 << 29); /* W1C: TC0IE + TC0IP */
	jiffies = 0;

	REG32(CDBR_REG) = (DIVISOR) << DIVF_OFFSET;
	REG32(TC0DATA_REG) = (((lexra_clock / DIVISOR) / TICK_FREQ) + 1) << 4;
	/* Enable timer */
	REG32(TCCNR_REG) = (1 << 31) | (1 << 30);
	/* Wait n cycles for timer to re-latch the new value of TC0DATA. */
	for (c = 0; c < DIVISOR; c++)
		;
	/* Set interrupt routing register */
	REG32(IRR1_REG) = 0x00050004; /* uart: IRQ5, timer0: IRQ4 */
	/* Enable timer interrupt */
	REG32(TCIR_REG) = (1 << 31);

	request_IRQ(TC0_IRQ, &irq_timer, NULL);
}

/**
 * timer_irq_enable - Unmask the tick in GIMR
 *
 * doBooting() masks every interrupt before entering download mode; the
 * switch bring-up that follows relies on delay_ms().
 */
void timer_irq_enable(void)
{
	REG32(GIMR_REG) = REG32(GIMR_REG) | (1 << TC0_IRQ);
}

void delay_ms(unsigned int time_ms)
{
	unsigned int preTime;

	preTime = get_timer_jiffies();
	while (get_timer_jiffies() - preTime < time_ms / 10)
		;
}
