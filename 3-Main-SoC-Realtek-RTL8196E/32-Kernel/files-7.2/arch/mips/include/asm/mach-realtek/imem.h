/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_MACH_REALTEK_IMEM_H
#define __ASM_MACH_REALTEK_IMEM_H

/*
 * On-chip I-MEM (instruction SRAM) placement macros for RTL8196E.
 *
 * The RTL8196E has 16 KB of on-chip instruction SRAM accessible via the
 * Lexra COP3 Instruction Window.  Functions annotated with __iram are
 * placed in the .iram linker section, which is copied to on-chip SRAM
 * at boot by _imem_dmem_init().  Subsequent instruction fetches from
 * these functions are served at zero wait-state (1 cycle), bypassing
 * the I-cache entirely.
 *
 * Usage:
 *   static __iram void hot_function(void) { ... }
 *
 * Budget: 16 KB total.  Use only for performance-critical hot-path code
 * (IRQ dispatch, NAPI poll, TX submit, DMA cache ops).
 *
 * D-MEM (8 KB on-chip data SRAM): the hardware also exposes 8 KB of
 * data SRAM accessible via the COP3 Data Window.  No __dram macro is
 * provided here because the v3.4.0 IRAM PoC measured a -4 % UDP TX
 * regression when ring metadata was placed in DMEM (the COP3 DW
 * indirection costs more than the on-chip SRAM saves once data is
 * already cache-resident on this hardware).  The DMEM init code is
 * preserved in arch/mips/realtek/imem.S as a reference for the
 * 4-stage SDRAM->DMEM copy sequence, but the .dram section stays
 * empty in production and the DMEM init path takes its skip_dram
 * branch at boot.
 */

/*
 * Two-level placement control:
 *
 *   CONFIG_RTL8196E_IMEM
 *     Master switch.  When enabled, the linker reserves the .iram
 *     section, the COP3 IW window is programmed at boot, and the
 *     hardware fills the on-chip I-MEM from SDRAM.  When disabled,
 *     everything below collapses to no-op and no IRAM exists at
 *     runtime.
 *
 *   CONFIG_RTL8196E_IMEM_DEFAULT_PLACEMENT
 *     Selective switch.  Controls whether the existing __iram /
 *     __iram_hotpath / __iram_gen / __iram_fwd annotations across the
 *     tree resolve to a real section attribute.  Disabling this turns
 *     off all 24+ default placement sites *without* removing the
 *     annotations, which is exactly what the IRAM PoC needs:
 *     baseline = no placement; phase D/E = only __iram_poc takes
 *     effect.
 *
 *   __iram_poc
 *     Independent of DEFAULT_PLACEMENT.  Active whenever IMEM is on.
 *     Used by the PoC to opt one or two functions into IRAM in
 *     isolation.
 */
#if defined(CONFIG_RTL8196E_IMEM) && defined(CONFIG_RTL8196E_IMEM_POC_IRAM)
#define __iram_poc	__attribute__((section(".iram")))
#else
#define __iram_poc
#endif

#if defined(CONFIG_RTL8196E_IMEM) && defined(CONFIG_RTL8196E_IMEM_DEFAULT_PLACEMENT)
#define __iram		__attribute__((section(".iram")))
#define __iram_gen	__attribute__((section(".iram-gen")))
#define __iram_fwd	__attribute__((section(".iram-fwd")))
#else
#define __iram
#define __iram_gen
#define __iram_fwd
#endif

#endif /* __ASM_MACH_REALTEK_IMEM_H */
