/*
 * board.h — Sengled Smart Hub E39-G8C ("Sengled G4")
 *
 * One directory per board under 31-Bootloader/boards/<board>/, selected
 * with `BOARD=<board> ./build_bootloader.sh` (default: lidl). Every
 * macro below is mandatory — see boards/README.md for the contract and
 * the validation requirements before flashing a new board.
 */
#ifndef __BOARD_H__
#define __BOARD_H__

/* Human-readable DRAM size, shown in the stage-2 banner ("RAM: 64MB"). */
#define BOARD_DRAM_BANNER "64MB"

/*
 * KSEG1 (uncached) address one past the last DRAM byte.  Drives the
 * boothold flag page (DRAM top - 0x2000; see boot/main.c) — this MUST
 * match the `boothold` reserved-memory node of the board's kernel DTS,
 * or `boothold && reboot` from Linux silently stops working.
 */
#define BOARD_DRAM_TOP_KSEG1 0xA4000000

/* Stage-1.5 may only decompress this much stage-2 code at 0x80400000. */
#define BOARD_STAGE2_MAX_SIZE 0x00100000

/*
 * DDR controller bring-up values, written by btcode/start.S before any
 * DRAM access (nothing overwrites them later — these two macros ARE the
 * DRAM configuration).  Macros are named by REGISTER ADDRESS on purpose:
 * the historical names (DDR_TIMING_VAL at 0x1004, DDR1_32MB_193MHZ at
 * 0x1008) were swapped vs the usual DCR/DTR convention and misled DRAM
 * debugging more than once — go by the address, not by any name.
 */
#define BOARD_DDR_REG_1004 0x54880000	/* -> 0xB8001004 */
#define BOARD_DDR_REG_1008 0x91051D20	/* -> 0xB8001008: 64 MB DDR2 @ 193 MHz */

#endif /* __BOARD_H__ */
