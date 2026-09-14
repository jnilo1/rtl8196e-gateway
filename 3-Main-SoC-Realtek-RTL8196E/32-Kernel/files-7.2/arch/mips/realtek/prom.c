/*
 * Realtek RTL819x Early Boot and Console Initialization
 *
 * This file handles the very early initialization phase of the kernel boot
 * process, specifically setting up the early console (bootconsole) that allows
 * kernel messages to be displayed before the full serial driver is loaded.
 *
 * The RTL819x SoCs use standard 16550A-compatible UARTs, allowing us to use
 * the generic 8250 early printk support provided by the MIPS architecture.
 *
 * Copyright (C) 2025 Jacques Nilo
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */

#include <linux/init.h>
#include <asm/bootinfo.h>
#include <asm/setup.h>
#include <asm/io.h>

/*
 * UART0 Base Address
 *
 * Physical address: 0x18002000
 * Virtual address (KSEG1): 0xB8002000 (uncached)
 *
 * UART0 is used as the primary console for early boot messages and is
 * configured with standard 16550A register layout at this address.
 */
#define REALTEK_UART0_BASE	0xB8002000

/**
 * prom_init() - Initialize the early boot console
 *
 * This function is called very early in the kernel boot sequence, before
 * memory management, device drivers, or most kernel subsystems are initialized.
 * Its sole purpose is to configure a minimal serial console that can display
 * kernel printk() messages during early boot.
 *
 * The function uses the generic MIPS 8250 early printk infrastructure to
 * set up UART0 with the following parameters:
 * - Base address: 0xB8002000 (UART0)
 * - Register shift: 2 (registers are 32-bit aligned, 4 bytes apart)
 * - Divisor: 30000 (for baud rate calculation with 200MHz bus clock)
 *
 * The actual baud rate will be configured by the bootloader before the kernel
 * starts, and this setup just ensures we can use the UART for output.
 *
 * Note: This is enabled by CONFIG_USE_GENERIC_EARLY_PRINTK_8250 in Kconfig
 */
void __init prom_init(void)
{
	/*
	 * Setup early console for printk output
	 *
	 * Parameters:
	 * - base: Physical base address of UART0
	 * - reg_shift: Register alignment (2 = 4-byte aligned registers)
	 * - divisor: Baud rate divisor (30000 for 200MHz clock)
	 */
	setup_8250_early_printk_port((unsigned long)REALTEK_UART0_BASE, 2, 30000);
}

/**
 * prom_free_prom_memory() - Release PROM memory to the kernel
 *
 * On many MIPS systems, the bootloader (PROM/BIOS) reserves some memory
 * during the boot process. This function is called after the kernel has
 * finished using any PROM services to release that memory back to the
 * kernel's memory management system.
 *
 * For RTL819x SoCs, the bootloader (typically U-Boot or proprietary Realtek
 * bootloader) does not reserve any persistent memory regions that need to
 * be freed, so this function is a no-op.
 *
 * This function must be defined as it's part of the MIPS architecture's
 * required interface, even if it does nothing on this platform.
 */
void __init prom_free_prom_memory(void)
{
	/* No PROM memory to free on RTL819x */
}
