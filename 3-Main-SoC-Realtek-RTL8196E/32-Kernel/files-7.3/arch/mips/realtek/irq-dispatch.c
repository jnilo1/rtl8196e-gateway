/*
 * Realtek RTL819x MIPS Interrupt Dispatch
 *
 * Copyright (C) 2025 Jacques Nilo
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */

#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/of_irq.h>

#include <asm/irq_cpu.h>
#include <asm/mipsregs.h>
#include <asm/mach-realtek/imem.h>
#include <linux/rtl819x_intc_stats.h>

/*
 * CANONICAL SOURCE-TO-CPU-LINE ROUTING — must match, in this order:
 *   1. IRR1 programmed by realtek_soc_irq_init() in drivers/irqchip/irq-rtl819x.c
 *   2. the "interrupts"/"interrupt-names" properties of the intc@3000 DT node
 *   3. the names and comments below
 *
 *   | source      | INTC bit | CPU line | role                          |
 *   |-------------|---------:|---------:|-------------------------------|
 *   | TC0         |        8 |      IP7 | clockevent                    |
 *   | switch core |       15 |      IP3 | Ethernet / NAPI               |
 *   | UART1       |       13 |      IP4 | EFR32 radio link (latency)    |
 *   | UART0       |       12 |      IP2 | occasional debug console      |
 *
 * Live silicon reads back IRR1 = 0x30420007, which decodes to exactly that.
 *
 * These constants are deliberately named after the CPU line and NOT after the
 * source. Earlier revisions called IP3 "UART1" and IP4 "SWITCH", the exact
 * inverse of the programmed routing. The numeric do_IRQ() arguments were still
 * correct — IP<n> maps to CPU IRQ <n>, and IP2/IP3/IP4 all land on the same
 * chained handler — so the inversion never misrouted anything. It did mislead
 * a forensic campaign into reading an IP4-only trace as an Ethernet storm when
 * it was ordinary UART1 traffic, and it would become a real bug the day
 * distinct per-parent handlers are installed.
 */
#define REALTEK_CPU_IRQ_IP2         2       /* INTC cascade: UART0 (bit 12) */
#define REALTEK_CPU_IRQ_IP3         3       /* INTC cascade: switch core (bit 15) */
#define REALTEK_CPU_IRQ_IP4         4       /* INTC cascade: UART1 (bit 13) */
#define REALTEK_CPU_IRQ_IP7         7       /* TC0 clockevent, direct */

/* Mask of all known/handled interrupt sources */
#define REALTEK_HANDLED_IRQS (STATUSF_IP7 | STATUSF_IP4 | STATUSF_IP3 | STATUSF_IP2)

/**
 * plat_irq_dispatch - Top-level MIPS interrupt dispatcher
 *
 * Main entry point for hardware interrupts. Services pending CPU lines in
 * the order IP7 > IP4 > IP3 > IP2 (see the routing table above):
 * - IP7: TC0 clockevent (direct, most frequent - checked first)
 * - IP4: UART1, the EFR32 radio link (INTC bit 13, latency-sensitive)
 * - IP3: switch core / Ethernet (INTC bit 15, DMA rings + NAPI)
 * - IP2: INTC cascade for UART0 (bit 12, occasional debug console)
 *
 * Note: Uses independent if statements (not else-if) to handle multiple
 * simultaneous interrupts in a single dispatch call, reducing latency.
 */
asmlinkage __iram void plat_irq_dispatch(void)
{
    unsigned long pending = read_c0_status() & read_c0_cause() & ST0_IM;

    if (likely(pending)) {
        /* TC0 clockevent — the most frequent interrupt */
        if (likely(pending & STATUSF_IP7)) {
            /*
             * The clockevent interrupt count. TC0 never traverses the INTC
             * chained handler, so its GISR bit is not a tick counter; this
             * is (see rtl819x_intc_stats.h). One u32 store on the hottest
             * path, matching the cost already accepted for the INTC taps.
             */
            rtl819x_intc_stats.ip7++;
            do_IRQ(REALTEK_CPU_IRQ_IP7);
        }
        /* UART1 — 16-byte FIFO, ~350 us at 460800 baud, overrun drops a frame */
        if (pending & STATUSF_IP4) {
            do_IRQ(REALTEK_CPU_IRQ_IP4);
        }
        /* Switch/Ethernet — a missed IRQ costs at most a TCP retransmit */
        if (pending & STATUSF_IP3) {
            do_IRQ(REALTEK_CPU_IRQ_IP3);
        }
        /* UART0 cascaded (least frequent) */
        if (pending & STATUSF_IP2) {
            do_IRQ(REALTEK_CPU_IRQ_IP2);
        }

        /* Check for spurious interrupts (should be very rare) */
        if (unlikely(!(pending & REALTEK_HANDLED_IRQS))) {
            spurious_interrupt();
        }
    }
}

/**
 * arch_init_irq - Architecture-specific IRQ initialization
 *
 * This function initializes the interrupt system by invoking the
 * device tree IRQ initialization framework.
 */
void __init arch_init_irq(void)
{
    irqchip_init();
}
