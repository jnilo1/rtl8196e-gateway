// SPDX-License-Identifier: GPL-2.0-only
/*
 * Realtek RTL8196E Interrupt Controller Driver
 *
 * This driver implements support for the RTL8196E interrupt controller,
 * managing peripheral interrupts (UART, Ethernet, Timers, etc.) and routing
 * them to the appropriate MIPS CPU interrupt lines.
 *
 * Key features:
 * - 32-bit interrupt mask and status registers (GIMR/GISR)
 * - Flexible interrupt routing via IRR registers
 * - Chained interrupt handling for multiple IP lines
 * - Virtual IRQ caching for performance-critical interrupts
 * - Thread-safe mask/unmask operations with raw_spinlock
 *
 * Copyright (C) 2025 Jacques Nilo
 */

#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/irqchip.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/kernel.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/io.h>
#include <linux/rtl819x_intc_stats.h>
#include <asm/mach-realtek/imem.h>

#define DRV_VERSION "1.2"

/*
 * Dispatch statistics consumed by the rtl819x watchdog (1 Hz flight recorder
 * + panic record v8) — see rtl819x_intc_stats.h for the issue #99 rationale
 * and for the exact semantics of every field. Two of them are easy to
 * over-read and are documented there at length: `empty` is an ambiguous empty
 * chained entry rather than proof of a storm, and count[TC0] is incidental
 * sampling rather than a tick count (the clockevent count is `ip7`, produced
 * by plat_irq_dispatch()).
 */
struct rtl819x_intc_stats rtl819x_intc_stats;

/* ========================================================================== */
/* Hardware Definitions */
/* ========================================================================== */

static void __iomem *rtl819x_intc_base;
static DEFINE_RAW_SPINLOCK(intc_lock);  /* Protects GIMR read-modify-write */

#define ic_w32(val, reg)        writel(val, rtl819x_intc_base + reg)
#define ic_r32(reg)             readl(rtl819x_intc_base + reg)

/* Interrupt Controller Registers */
#define REALTEK_IC_REG_MASK         0x00    /* GIMR - Global Interrupt Mask */
#define REALTEK_IC_REG_STATUS       0x04    /* GISR - Global Interrupt Status */
#define REALTEK_IC_REG_IRR0         0x08    /* Interrupt Routing Register 0 */
#define REALTEK_IC_REG_IRR1         0x0C    /* Interrupt Routing Register 1 */
#define REALTEK_IC_REG_IRR2         0x10    /* Interrupt Routing Register 2 */
#define REALTEK_IC_REG_IRR3         0x14    /* Interrupt Routing Register 3 */

/* Hardware Interrupt Bits (GIMR/GISR) */
#define REALTEK_HW_TC0_BIT          8       /* Timer/Counter 0 */
#define REALTEK_HW_TC1_BIT          9       /* Timer/Counter 1 */
#define REALTEK_HW_UART0_BIT        12      /* UART 0 */
#define REALTEK_HW_UART1_BIT        13      /* UART 1 */
#define REALTEK_HW_SW_CORE_BIT      15      /* Switch Core (Ethernet) */

/*
 * MIPS CPU Interrupt Lines (IP0-IP7) — for documentation only.
 * Parent IRQs are now resolved from the DT via irq_of_parse_and_map();
 * see the "interrupts"/"interrupt-names" properties on the intc@3000
 * node. IP7 (timer) is consumed directly by the timer driver via its
 * own DT binding to &cpuintc.
 */

/* IRQ Domain Configuration */
#define REALTEK_INTC_IRQ_COUNT      32
#define REALTEK_INTC_IRQ_BASE       16

/* ========================================================================== */
/* Virtual IRQ Cache (Performance Optimization) */
/* ========================================================================== */

/* Cached virtual IRQs for hot-path optimization in interrupt handler */
static unsigned int uart0_virq;
static unsigned int uart1_virq;
static unsigned int switch_virq;

/* ========================================================================== */
/* Interrupt Routing Configuration */
/* ========================================================================== */

/**
 * realtek_soc_irq_init - Configure interrupt routing registers
 *
 * Sets up IRR registers to route peripheral interrupts to CPU interrupt lines.
 * Configuration: Timer→IP7, UART1→IP4, Switch→IP3, UART0→IP2 (cascaded).
 *
 * Priority rationale (Lidl/Silvercrest Zigbee gateway):
 *   plat_irq_dispatch() services pending IPs in order IP7 > IP4 > IP3 > IP2.
 *   UART1 carries the Zigbee link to the EFR32 radio; the 8250 RX FIFO is
 *   16 bytes (~350 µs at 460800 baud) and an overrun drops a Zigbee frame,
 *   forcing Z2M / ZHA to reconnect — user-visible. Ethernet uses DMA rings
 *   plus NAPI, so a missed switch IRQ at most translates to a TCP retransmit
 *   — invisible. UART1 therefore gets the higher priority IP (IP4), Switch
 *   gets IP3.
 */
static void realtek_soc_irq_init(void)
{
	u32 irr1_val, irr2_val;

	/*
	 * IRR1: Configure routing for Timer0, UARTs, and Switch
	 * Each 4-bit field routes one GIMR bit to a CPU interrupt line (0-7)
	 */
	irr1_val = (0x3 << 28) |  /* Switch Core → IP3 */
			   (0x0 << 24) |  /* Unused */
			   (0x4 << 20) |  /* UART1 → IP4 (highest priority after timer) */
			   (0x2 << 16) |  /* UART0 → IP2 */
			   (0x0 << 12) |  /* OTG → Disabled */
			   (0x0 << 8)  |  /* USB Host → Disabled */
			   (0x0 << 4)  |  /* TC1 → Disabled */
			   (0x7 << 0);    /* TC0 → IP7 */

	ic_w32(irr1_val, REALTEK_IC_REG_IRR1);

	/* IRR2: All peripherals disabled (no PCIe) */
	irr2_val = 0x00000000;
	ic_w32(irr2_val, REALTEK_IC_REG_IRR2);

	/* IRR0 and IRR3: Not used */
	ic_w32(0x00000000, REALTEK_IC_REG_IRR0);
	ic_w32(0x00000000, REALTEK_IC_REG_IRR3);

	pr_debug("RTL8196E INTC: IRR1=0x%08x, IRR2=0x%08x\n", irr1_val, irr2_val);
}

/* ========================================================================== */
/* Interrupt Controller Operations */
/* ========================================================================== */

/**
 * realtek_soc_irq_mask - Disable a hardware interrupt
 * @d: IRQ data containing the hardware IRQ number
 *
 * Thread-safe: Uses raw_spinlock to protect GIMR read-modify-write.
 */
static void realtek_soc_irq_mask(struct irq_data *d)
{
	unsigned long flags;
	u32 mask;

	if (unlikely(d->hwirq >= 32))
		return;

	raw_spin_lock_irqsave(&intc_lock, flags);
	mask = ic_r32(REALTEK_IC_REG_MASK);
	ic_w32(mask & ~BIT(d->hwirq), REALTEK_IC_REG_MASK);
	raw_spin_unlock_irqrestore(&intc_lock, flags);
}

/**
 * realtek_soc_irq_unmask - Enable a hardware interrupt
 * @d: IRQ data containing the hardware IRQ number
 *
 * Thread-safe: Uses raw_spinlock to protect GIMR read-modify-write.
 */
static void realtek_soc_irq_unmask(struct irq_data *d)
{
	unsigned long flags;
	u32 mask;

	if (unlikely(d->hwirq >= 32))
		return;

	raw_spin_lock_irqsave(&intc_lock, flags);
	mask = ic_r32(REALTEK_IC_REG_MASK);
	ic_w32(mask | BIT(d->hwirq), REALTEK_IC_REG_MASK);
	raw_spin_unlock_irqrestore(&intc_lock, flags);
}

/**
 * realtek_soc_irq_ack - Acknowledge a hardware interrupt
 * @d: IRQ data containing the hardware IRQ number
 *
 * Clears pending status in GISR. Write-only operation, no lock needed.
 */
static void realtek_soc_irq_ack(struct irq_data *d)
{
	if (unlikely(d->hwirq >= 32))
		return;

	ic_w32(BIT(d->hwirq), REALTEK_IC_REG_STATUS);
}

/* Interrupt controller chip definition */
static struct irq_chip realtek_soc_irq_chip = {
	.name           = "RTL8196E-INTC",
	.irq_ack        = realtek_soc_irq_ack,
	.irq_mask       = realtek_soc_irq_mask,
	.irq_unmask     = realtek_soc_irq_unmask,
};

/* ========================================================================== */
/* Chained Interrupt Handler */
/* ========================================================================== */

/**
 * realtek_soc_irq_handler - Process interrupts from INTC
 * @desc: IRQ descriptor for the parent interrupt (IP2/IP3/IP4)
 *
 * Handles multiple simultaneous interrupts. Uses cached virtual IRQs
 * for frequently-used interrupts (Switch, UARTs) to avoid lookup overhead.
 */
static __iram void realtek_soc_irq_handler(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);
	struct irq_domain *domain = irq_desc_get_handler_data(desc);
	u32 mask, status, pending;

	chained_irq_enter(chip, desc);

	/* Read interrupt state */
	mask = ic_r32(REALTEK_IC_REG_MASK);
	status = ic_r32(REALTEK_IC_REG_STATUS);
	pending = mask & status;

	/*
	 * issue #99 flight-recorder taps. `empty` records a chained entry that
	 * found nothing pending after masking. That happens during a storm, but
	 * also in normal operation: one parent invocation drains every bit of a
	 * simultaneous snapshot, and the sibling parent latched meanwhile then
	 * enters with an empty one. Only a sustained rate is a storm signal.
	 * The per-bit count/last_seen_j pairs name which line was active in the
	 * final seconds before a panic. Two u32 stores next to MMIO reads.
	 */
	rtl819x_intc_stats.entries++;
	if (unlikely(!pending))
		rtl819x_intc_stats.empty++;

	/* Spurious: no pending bits after masking — skip loop, still call exit */
	while (pending) {
		int bit = __ffs(pending);
		unsigned int virq = 0;

		rtl819x_intc_stats.count[bit]++;
		rtl819x_intc_stats.last_seen_j[bit] = (u32)jiffies;

		/*
		 * GISR ack is handled by realtek_soc_irq_ack() (the .irq_ack
		 * callback wired into handle_level_irq below). Don't W1C the
		 * pending bit here — it would just be a redundant MMIO write
		 * on every IRQ.
		 *
		 * Hot-path optimization: Use cached virtual IRQs for
		 * frequently-used interrupts to avoid irq_find_mapping().
		 */
		switch (bit) {
		case REALTEK_HW_SW_CORE_BIT:
			virq = switch_virq;
			break;
		case REALTEK_HW_UART1_BIT:
			virq = uart1_virq;
			break;
		case REALTEK_HW_UART0_BIT:
			virq = uart0_virq;
			break;
		default:
			virq = irq_find_mapping(domain, bit);
			break;
		}

		/* Dispatch to Linux IRQ handler */
		if (likely(virq))
			generic_handle_irq(virq);
		else
			pr_warn_ratelimited("RTL8196E INTC: No mapping for HW bit %d\n", bit);

		pending &= ~BIT(bit);
	}

	chained_irq_exit(chip, desc);
}

/* ========================================================================== */
/* IRQ Domain Management */
/* ========================================================================== */

/**
 * intc_map - Map hardware IRQ to virtual IRQ number
 * @d: IRQ domain
 * @irq: Virtual IRQ number to assign
 * @hw: Hardware IRQ number (bit in GIMR)
 *
 * Caches virtual IRQs for performance-critical interrupts.
 */
static int intc_map(struct irq_domain *d, unsigned int irq, irq_hw_number_t hw)
{
	/* Cache virtual IRQs for fast lookup in interrupt handler */
	switch (hw) {
	case REALTEK_HW_SW_CORE_BIT:
		switch_virq = irq;
		pr_debug("RTL8196E INTC: Switch (bit %lu) → virq %u\n", hw, irq);
		break;
	case REALTEK_HW_UART0_BIT:
		uart0_virq = irq;
		pr_debug("RTL8196E INTC: UART0 (bit %lu) → virq %u\n", hw, irq);
		break;
	case REALTEK_HW_UART1_BIT:
		uart1_virq = irq;
		pr_debug("RTL8196E INTC: UART1 (bit %lu) → virq %u\n", hw, irq);
		break;
	default:
		pr_debug("RTL8196E INTC: HW bit %lu → virq %u\n", hw, irq);
		break;
	}

	/* Configure as level-triggered interrupt */
	irq_set_chip_and_handler(irq, &realtek_soc_irq_chip, handle_level_irq);

	return 0;
}

static void intc_unmap(struct irq_domain *d, unsigned int irq)
{
	if (irq == switch_virq) switch_virq = 0;
	else if (irq == uart0_virq) uart0_virq = 0;
	else if (irq == uart1_virq) uart1_virq = 0;
}

/* IRQ domain operations */
static const struct irq_domain_ops irq_domain_ops = {
	.xlate = irq_domain_xlate_onecell,
	.map = intc_map,
	.unmap = intc_unmap,
};

/* ========================================================================== */
/* Device Tree Initialization */
/* ========================================================================== */

/**
 * intc_of_init - Initialize interrupt controller from device tree
 * @node: Device tree node for this interrupt controller
 * @parent: Parent device tree node
 *
 * Maps registers, configures routing, enables interrupts, and sets up
 * chained handlers for IP2/IP3/IP4.
 *
 * Return: 0 on success, negative error code on failure
 */
static int __init intc_of_init(struct device_node *node, struct device_node *parent)
{
	struct resource res;
	struct irq_domain *domain;
	int ret;

	/* Get and map memory resource */
	ret = of_address_to_resource(node, 0, &res);
	if (ret) {
		pr_err("RTL8196E INTC: Failed to get memory resource: %d\n", ret);
		return ret;
	}

	rtl819x_intc_base = ioremap(res.start, resource_size(&res));
	if (!rtl819x_intc_base) {
		pr_err("RTL8196E INTC: Failed to map registers at %pa\n", &res.start);
		return -ENOMEM;
	}

	pr_debug("RTL8196E INTC: Registers mapped at %pa (%zu bytes)\n",
			 &res.start, resource_size(&res));

	/* Configure interrupt routing */
	realtek_soc_irq_init();

	/* Create IRQ domain (irq_domain_add_legacy was removed in 6.x,
	 * replaced by irq_domain_create_legacy which takes a fwnode_handle) */
	domain = irq_domain_create_legacy(of_fwnode_handle(node),
									  REALTEK_INTC_IRQ_COUNT,
									  REALTEK_INTC_IRQ_BASE, 0,
									  &irq_domain_ops, NULL);
	if (!domain) {
		pr_err("RTL8196E INTC: Failed to create IRQ domain\n");
		ret = -ENOMEM;
		goto err_iounmap;
	}

	/*
	 * Set up chained interrupt handlers for every parent IRQ described
	 * in the DT (typically IP2 cascade + IP3 Switch + IP4 UART1). The
	 * driver does not assume how many entries there are — it walks them
	 * until irq_of_parse_and_map() returns 0.
	 */
	{
		unsigned int parent_irq;
		int i;

		for (i = 0; (parent_irq = irq_of_parse_and_map(node, i)); i++)
			irq_set_chained_handler_and_data(parent_irq,
											 realtek_soc_irq_handler,
											 domain);

		if (i == 0) {
			pr_err("RTL8196E INTC: No parent IRQ described in DT\n");
			ret = -EINVAL;
			goto err_domain;
		}
	}

	/*
	 * GIMR init: only TC0 is enabled unconditionally.
	 *
	 * UART0/UART1/Switch are routed through this irqdomain and their
	 * GIMR bit is set by realtek_soc_irq_unmask() when the consumer
	 * driver calls request_irq() / enable_irq(). Activating them here
	 * exposes the chained handler to interrupts before any driver is
	 * ready to drain the source.
	 *
	 * TC0 cannot follow that pattern: the timer DT node points at
	 * &cpuintc/<7>, so the timer driver requests CPU IRQ 7 directly
	 * and never goes through this irqdomain's .irq_unmask path. Yet
	 * the only hardware path TC0 -> CPU is via INTC IRR1 + GIMR (no
	 * dedicated bypass — the bootloader, which routes TC0 to IP4 via
	 * IRR1 and arms GIMR bit 8 in monitor.c:163/190 + irq.c:39, is
	 * the reference). Leaving GIMR bit 8 cleared here would keep IP7
	 * permanently silent and hang the kernel at clocksource init.
	 */
	ic_w32(BIT(REALTEK_HW_TC0_BIT), REALTEK_IC_REG_MASK);

	pr_info("irq-rtl819x v" DRV_VERSION " (J. Nilo) - Timer:IP7, UART1:IP4, Switch:IP3, UART0:IP2\n");

	return 0;

err_domain:
	irq_domain_remove(domain);
err_iounmap:
	iounmap(rtl819x_intc_base);
	rtl819x_intc_base = NULL;
	return ret;
}

/* ========================================================================== */
/* Driver Registration */
/* ========================================================================== */

/* Register with kernel irqchip infrastructure */
IRQCHIP_DECLARE(rtl819x_intc, "realtek,rtl819x-intc", intc_of_init);
