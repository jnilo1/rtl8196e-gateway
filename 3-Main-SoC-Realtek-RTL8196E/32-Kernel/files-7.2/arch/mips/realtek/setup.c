/*
 * Realtek RTL819x Platform Initialization
 *
 * This file implements the core platform-specific initialization code for
 * Realtek RTL819x MIPS SoCs (RTL8196E, RTL8197D, etc.). It handles:
 *
 * - Memory subsystem setup and device tree initialization
 * - Power management (system restart, halt, CPU idle)
 * - System controller register mapping
 * - Clock and timer initialization
 * - Platform identification
 *
 * The RTL819x series uses the RLX4181 CPU core (a Lexra-based MIPS-like
 * processor) and provides standard MIPS platform hooks for early boot and
 * hardware initialization.
 *
 * Copyright (C) 2025 Jacques Nilo
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/memblock.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <linux/of_fdt.h>
#include <linux/clk-provider.h>
#include <linux/clocksource.h>

#include <asm/reboot.h>
#include <asm/io.h>
#include <asm/prom.h>
#include <asm/setup.h>
#include <asm/time.h>
#include <asm/bootinfo.h>
#include <asm/addrspace.h>
#include <asm/idle.h>

#include <asm/mach-realtek/realtek_mem.h>

#ifdef CONFIG_RTL8196E_IMEM
extern void _imem_dmem_init(void);
#endif

/* Watchdog Timer Control Register offset in system controller */
#define REALTEK_WATCHDOG_TIMER_REG	0x311C

/* ========================================================================== */
/* Platform Identification */
/* ========================================================================== */

/**
 * get_system_type() - Return the platform name string
 *
 * This function provides a human-readable string identifying the SoC model.
 * The string is displayed in various kernel messages and /proc/cpuinfo.
 *
 * Return: Static string identifying the platform
 */
const char *get_system_type(void)
{
	return "Realtek RTL8196E";
}

/* ========================================================================== */
/* CPU Power Management */
/* ========================================================================== */

/**
 * wait_instruction() - Execute CPU wait/sleep instruction
 *
 * This inline function puts the CPU into a low-power idle state using the
 * RLX-specific 'sleep' instruction. On RLX4181 (based on R3000 ISA), the
 * 'sleep' instruction is used instead of the standard MIPS 'wait' instruction.
 *
 * The CPU will remain in low-power state until an interrupt occurs, at which
 * point it will wake up and continue execution.
 *
 * Note: The .set push/pop directives preserve the assembler state across
 * the inline assembly block.
 */
static inline void wait_instruction(void)
{
	__asm__ __volatile__(
		".set	push\n\t"
		"sleep\n\t"		/* RLX low-power instruction */
		".set	pop\n\t"
	);
}

/**
 * realtek_machine_restart() - Trigger system hardware reset
 * @command: Optional command string (currently ignored)
 *
 * This function performs a full hardware reset of the system by triggering
 * the watchdog timer. The process:
 *
 * 1. Disable all interrupts to prevent any interference
 * 2. Write 0x00 to the watchdog control register, which triggers an
 *    immediate watchdog timeout and system reset
 * 3. Enter infinite sleep loop as a safety measure (should never reach here)
 *
 * This is the standard method for resetting RTL819x SoCs.
 */
static void realtek_machine_restart(char *command)
{
	/* Disable all interrupts */
	local_irq_disable();

	/*
	 * Trigger watchdog reset
	 * Writing 0x00 to the watchdog register causes immediate reset
	 */
	sr_w32(0x00, REALTEK_WATCHDOG_TIMER_REG);

	/* Safety: loop forever in case reset fails (should never happen) */
	for (;;)
		wait_instruction();
}

/**
 * realtek_wait() - CPU idle handler
 *
 * This function is called by the kernel's idle loop when there are no tasks
 * to run. It puts the CPU into a low-power state until the next interrupt
 * or until a task becomes runnable.
 *
 * The function checks if rescheduling is needed (need_resched()) before
 * sleeping to avoid missing a wakeup. After waking, it re-enables interrupts
 * as they may have been disabled by the idle loop.
 *
 * This is installed as the cpu_wait handler for RTL819x platforms.
 */
static void realtek_wait(void)
{
	if (!need_resched())
		wait_instruction();
	local_irq_enable();
}

/**
 * realtek_halt() - System halt handler
 *
 * This function is called when the system is being halted (shutdown without
 * power-off). It disables interrupts and puts the CPU into an infinite sleep
 * loop, effectively stopping all processing.
 *
 * On RTL819x, there is no software-controlled power-off capability, so halt
 * simply stops the CPU. Physical power must be removed to fully power down.
 */
static void realtek_halt(void)
{
	for (;;)
		wait_instruction();
}

/* ========================================================================== */
/* Memory and Device Tree Setup */
/* ========================================================================== */

/**
 * plat_mem_setup() - Early platform memory and handler initialization
 *
 * This function is called very early in the kernel boot process to perform
 * platform-specific setup. It runs before most kernel subsystems are
 * initialized but after basic memory management is available.
 *
 * Key responsibilities:
 * 1. Install platform-specific handlers (restart, halt, idle)
 * 2. Locate and validate the device tree blob (DTB)
 * 3. Initialize the device tree architecture setup
 *
 * The device tree can come from two sources:
 * - fw_passed_dtb: Passed by bootloader in a register/memory location
 * - __dtb_start: Built into the kernel image (CONFIG_BUILTIN_DTB)
 *
 * This function is part of the MIPS platform initialization sequence and
 * is called automatically by the architecture code.
 */
void __init plat_mem_setup(void)
{
	void *dtb = NULL;

#ifdef CONFIG_RTL8196E_IMEM
	/*
	 * Initialise on-chip I-MEM and D-MEM.
	 *
	 * Programs the Lexra COP3 Instruction Window (IW) and Data Window (DW)
	 * registers to pin the .iram/.dram sections in the 16 KB / 8 KB on-chip
	 * SRAMs, giving zero wait-state instruction and data access on the
	 * Ethernet hot path.
	 *
	 * Must be called before any code annotated with __iram_fwd / __iram_gen
	 * is executed, and before cache invalidation in later init stages could
	 * interfere.  plat_mem_setup() is the earliest available hook that has
	 * a working C environment.
	 */
	_imem_dmem_init();
#endif

	/* Install platform power management handlers */
	_machine_restart = realtek_machine_restart;
	_machine_halt = realtek_halt;
	cpu_wait = realtek_wait;

	/*
	 * Locate the device tree blob: built-in DTB compiled into kernel.
	 * (fw_passed_dtb was removed from mainline MIPS; bootloader does
	 * not pass a DTB on this platform.)
	 */
	if (&__dtb_start[0] != &__dtb_end[0])
		dtb = (void *)__dtb_start;

	/* Initialize device tree architecture support */
	__dt_setup_arch(dtb);
}

/* ========================================================================== */
/* System Controller Mapping */
/* ========================================================================== */

/*
 * Global pointer to system controller registers
 *
 * The system controller (sysc) contains critical control registers including:
 * - Chip identification and revision
 * - Clock management and PLL configuration
 * - Bootstrap configuration
 * - Watchdog timer control
 * - PCIe PHY control (if applicable)
 *
 * This pointer is initialized in device_tree_init() and used by various
 * platform code via the sr_r32()/sr_w32() macros defined in realtek_mem.h
 */
void __iomem *_sys_membase;

/**
 * device_tree_init() - Initialize device tree and system controller mapping
 *
 * This function is called during the device initialization phase to:
 * 1. Unflatten the device tree into kernel data structures
 * 2. Locate the system controller node in the device tree
 * 3. Map the system controller registers for access
 * 4. Display bootstrap configuration information
 *
 * The system controller (realtek,rtl819x-sysc) is essential for many platform
 * operations, so we map it early and panic if it cannot be found or mapped.
 *
 * After this function completes, the device tree is fully available to kernel
 * drivers, and the system controller registers can be accessed via sr_r32()
 * and sr_w32() macros.
 */
void __init device_tree_init(void)
{
	struct device_node *np;
	struct resource res;

	/*
	 * Build the device tree
	 * This converts the flattened DTB (prepared in plat_mem_setup) into
	 * the kernel's internal tree structure used by the OF framework
	 */
	unflatten_and_copy_device_tree();

	/* Locate the system controller node */
	np = of_find_compatible_node(NULL, NULL, "realtek,rtl819x-sysc");
	if (!np)
		panic("Failed to find realtek,rtl819x-sysc node");

	/* Get the register address from device tree */
	if (of_address_to_resource(np, 0, &res))
		panic("Failed to get resource for realtek,rtl819x-sysc");

	/* Map the registers into kernel virtual memory (uncached) */
	_sys_membase = ioremap(res.start, resource_size(&res));
	if (!_sys_membase)
		panic("Failed to map memory for rtl819x-sysc");

	/*
	 * Display bootstrap configuration
	 * This reads critical system registers to show hardware configuration:
	 * - Chip ID and revision
	 * - Boot mode and configuration
	 * - Clock settings
	 * - Power-on configuration
	 */
	pr_info("BOOTSTRAP = %x %x %x %x\n",
		sr_r32(0x00),	/* Chip ID */
		sr_r32(0x04),	/* Revision */
		sr_r32(0x08),	/* Bootstrap register */
		sr_r32(0x10));	/* Clock management */
}

/* ========================================================================== */
/* Clock and Timer Initialization */
/* ========================================================================== */

/**
 * plat_time_init() - Initialize platform clocks and timers
 *
 * This function is called during the time subsystem initialization to set up
 * the platform's clock sources and event timers.
 *
 * It performs two key operations:
 *
 * 1. of_clk_init(NULL): Probes and initializes all clock providers defined
 *    in the device tree. This includes fixed clocks, PLLs, and clock gates.
 *
 * 2. timer_probe(): Discovers and initializes timer devices from the device
 *    tree (using TIMER_OF_DECLARE). For RTL819x, this will initialize the
 *    realtek,rtl819x-timer driver in drivers/clocksource/.
 *
 * After this function completes, the kernel has working timekeeping and can
 * schedule timer events.
 */
void __init plat_time_init(void)
{
	/* Initialize all device tree clock providers */
	of_clk_init(NULL);

	/* Probe and initialize timer devices from device tree */
	timer_probe();
}
