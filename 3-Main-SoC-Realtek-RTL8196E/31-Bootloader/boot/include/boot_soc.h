/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * boot_soc.h - RTL8196E SoC register map and accessors
 *
 * The system, UART0, interrupt controller, timer and memory controller
 * registers as the loader addresses them, the memory segments, and the C
 * accessors on all of it.  The switch core has its own map,
 * swcore_regs.h.  The register part is
 * plain macros so that inthandler.S can include this file; everything
 * C-only sits under !__ASSEMBLER__.
 *
 * Copyright (c) 2009-2020 Realtek Semiconductor Corp.
 * Copyright (c) 2024-2026 J. Nilo
 */
#ifndef _BOOT_SOC_H_
#define _BOOT_SOC_H_

#include "boot_asm.h"

/* --- Memory segments -------------------------------------------------------- */

#define KSEG0 0x80000000 /* cached */
#define KSEG1 0xa0000000 /* uncached */

/* --- Register map ----------------------------------------------------------- */

/* All peripheral registers sit in one KSEG1 window; UART and interrupt
 * controller registers are given as offsets from SYS_BASE, the others as
 * absolute addresses. */

/* System registers */
#define SYS_BASE 0xb8000000
#define SYS_CLKMANAGE (SYS_BASE + 0x10)

#define PIN_MUX_SEL 0xb8000030
#define PIN_MUX_SEL2 (SYS_BASE + 0x44)

#define BOND_OPTION (SYS_BASE + 0x000C)
#define BOND_ID_MASK (0xF)
#define BOND_8196ES (0xD)
#define HW_STRAP_REG 0xb8000008

/* Memory controller */
#define MCR_REG 0xb8001000
#define MPMR_REG 0xB8001040
#define DDCR_REG 0xb8001050

/* UART0 (offsets from SYS_BASE) */
#define UART_RBR 0x2000
#define UART_THR 0x2000
#define UART_DLL 0x2000
#define UART_IER 0x2004
#define UART_DLM 0x2004
#define UART_IIR 0x2008
#define UART_FCR 0x2008
#define UART_LCR 0x200c
#define UART_MCR 0x2010
#define UART_LSR 0x2014
#define UART_MSR 0x2018
#define UART_SCR 0x201c

/* The same, absolute (uart.c) */
#define UART_DLL_REG (0x2000 + SYS_BASE)
#define UART_IER_REG (0x2004 + SYS_BASE)
#define UART_DLM_REG (0x2004 + SYS_BASE)
#define UART_FCR_REG (0x2008 + SYS_BASE)
#define UART_LCR_REG (0x200c + SYS_BASE)

/* Interrupt controller (offsets from SYS_BASE) */
#define GIMR0 0x3000
#define GISR 0x3004
#define IRR0 0x3008
#define IRR1 0x300c

/* The same registers, absolute */
#define GICR_BASE 0xB8003000
#define GIMR_REG (0x000 + GICR_BASE) /* Global interrupt mask */
#define IRR1_REG (0x00C + GICR_BASE) /* Interrupt routing */

/* Timer/counter and watchdog */
#define TC0DATA_REG (0x100 + GICR_BASE) /* Timer/Counter 0 data */
#define TCCNR_REG (0x110 + GICR_BASE)	/* Timer/Counter control */
#define TCIR_REG (0x114 + GICR_BASE)	/* Timer/Counter interrupt */
#define CDBR_REG (0x118 + GICR_BASE)	/* Clock division base */
#define WDTCNR_REG (0x11C + GICR_BASE)	/* Watchdog timer control */

/* SPI flash, memory-mapped read window (physical) */
#define FLASH_BASE 0x05000000

#ifndef __ASSEMBLER__

/* --- Accessors -------------------------------------------------------------- */

#define REG32(reg) (*(volatile unsigned int *)(reg))
#define WRITE_MEM32(addr, val) (*(volatile unsigned int *)(addr)) = (val)
#define READ_MEM32(addr) (*(volatile unsigned int *)(addr))
#define WRITE_MEM16(addr, val) (*(volatile unsigned short *)(addr)) = (val)
#define READ_MEM16(addr) (*(volatile unsigned short *)(addr))

#define rtl_inb(offset)                                                        \
	(*(volatile unsigned char *)(SYS_BASE + offset))
#define rtl_inw(offset)                                                        \
	(*(volatile unsigned short *)(SYS_BASE + offset))
#define rtl_inl(offset)                                                        \
	(*(volatile unsigned long *)(SYS_BASE + offset))

#define rtl_outb(offset, val)                                                  \
	(*(volatile unsigned char *)(SYS_BASE + offset) = val)
#define rtl_outl(offset, val)                                                  \
	(*(volatile unsigned long *)(SYS_BASE + offset) = val)

#define BIT(x) (1 << (x))

/* Busy-wait: loops iterations of a two-instruction loop. */
static __inline__ void __delay(unsigned long loops)
{
	__asm__ __volatile__(".set\tnoreorder\n"
			     "1:\tbnez\t%0,1b\n\t"
			     "subu\t%0,1\n\t"
			     ".set\treorder"
			     : "=r"(loops)
			     : "0"(loops));
}

#endif /* !__ASSEMBLER__ */

#endif /* _BOOT_SOC_H_ */
