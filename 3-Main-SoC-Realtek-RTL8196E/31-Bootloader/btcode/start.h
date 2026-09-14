#ifndef __RTL_START_H__
#define __RTL_START_H__

#include "board.h" /* per-board values: boards/$(BOARD)/board.h via -I */

#define BOOT_ADDR 0x80100000 // compress
// #define	BOOT_ADDR		0x80000000    // no compress

// SoC / DDR register map (RTL8196E)
#define SYS_ID_REG 0xB8000000
#define SYS_ID_RTL8196E 0x8196e000
#define SYS_PATCH_REG 0xB8000008
#define SYS_PATCH_BIT (1 << 19)
#define CLKMGR_REG 0xB8000010

#define STRAP_REG 0xB8000048
#define STRAP_MASK (3 << 22)
#define STRAP_OR (1 << 23)

#define CLK_FREQ_REG 0xB8000088
#define CLK_FREQ_MASK (3 << 29)
#define CLK_FREQ_OR (0 << 29) // 2M

#define OCP_REG 0xB800008c
#define OCP_MASK (0x1f << 2)
#define OCP_OR (0x1f << 2)

#define MPMR_REG 0xB8001040
#define MPMR_DEFAULT 0x3FFFFF80
#define MPMR_PDN 0x7FFFFF80

/* DDR bring-up values come from the board header (BOARD_DDR_REG_1004 /
 * BOARD_DDR_REG_1008, named by register address) — the former in-place
 * names (DDR_TIMING_VAL, DDR1_32MB_193MHZ) were swapped vs the usual
 * DCR/DTR convention and are gone; go by the address. */
#define DDR_TIMING_REG 0xB8001004
#define DDR_CFG_REG 0xB8001008
#define DDCR_REG 0xB8001050
#define DDCR_INIT_VAL 0x50800000

#define CLKMGR_DEFAULT 0x00000b08

// DDR calibration constants
#define DDR_TEST_ADDR 0xA0000000
#define DDR_TEST_PATTERN 0x5a5aa5a5
#define DDR_TEST_MASK 0x00ff00ff
#define DDR_TEST_EXPECT 0x005a00a5
#define DDCR_SW_BASE 0x80000000
#define DDCR_SW_MASK 0xc0000000

//-------------------------------------------------
// Using register: t6, t7           //wei add this code
// t6=value
// t7=address
#define REG32_R(addr, v)                                                       \
	or t7, zero, addr;                                                     \
	lw v, 0(t7);                                                           \
	nop;

// Using register: t6, t7           value support "constant" and "register"
// access, so use "or" to instead "li"
#define REG32_W(addr, v)                                                       \
	or t6, zero, v;                                                        \
	or t7, zero, addr;                                                     \
	sw t6, 0(t7);                                                          \
	nop;

// Using register: t6, t7           //wei add this code
#define REG32_ANDOR(addr, andV, orV)                                           \
	li t7, addr;                                                           \
	lw t6, 0(t7);                                                          \
	and t6, t6, andV;                                                      \
	or t6, t6, orV;                                                        \
	sw t6, 0(t7);                                                          \
	nop;

// Using register: t6, t7           //wei add this code
#define IF_EQ(a, b, lab)                                                       \
	or t6, zero, a;                                                        \
	or t7, zero, b;                                                        \
	beq t6, t7, lab;                                                       \
	nop;

#define IF_NEQ(a, b, lab)                                                      \
	or t6, zero, a;                                                        \
	or t7, zero, b;                                                        \
	bne t6, t7, lab;                                                       \
	nop;

// uart register
#define UART_BASE 0xB8002000
#define UART_RBR (0x00 + UART_BASE)
#define UART_THR (0x00 + UART_BASE)
#define UART_DLL (0x00 + UART_BASE)
#define UART_IER (0x04 + UART_BASE)
#define UART_DLM (0x04 + UART_BASE)
#define UART_IIR (0x08 + UART_BASE)
#define UART_FCR (0x08 + UART_BASE)
#define UART_LCR (0x0c + UART_BASE)
#define UART_MCR (0x10 + UART_BASE)
#define UART_LSR (0x14 + UART_BASE)
#define UART_MSR (0x18 + UART_BASE)
#define UART_SCR (0x1c + UART_BASE)
#define UART_LSR_TX_EMPTY 0x60000000 /* LSR THRE|TEMT, byte lane 3 */

//---------------------------------------
#define SYS_CLK_RATE (200 * 1000000)
// #define SYS_CLK_RATE  	( 33.8688*1000000)      //33.8688MHz
// #define SYS_CLK_RATE	  	(  40*1000000)      //40Hz
// #define SYS_CLK_RATE	  	(  20*1000000)      //20Hz

#define BAUD_RATE (38400)

// Using register: t5, t6, t7     t5=msg(idx)
#define UART_PRINT(msg)                                                        \
	la t5, msg;                                                            \
	1 : lbu t6, 0(t5);                                                     \
	addu t5, 1;                                                            \
	beqz t6, 2f;                                                           \
	nop;                                                                   \
	sll t6, t6, 24;                                                        \
	REG32_W(UART_THR, t6);                                                 \
	j 1b;                                                                  \
	nop;                                                                   \
	2:

// Using register: t5, t6, t7     t5=msg(idx), t7=LSR
// Same as UART_PRINT but waits for the transmitter to be empty before
// each byte, so the string may be longer than what the 16-byte FIFO has
// room for at the time of the call (UART_PRINT relies on the FIFO alone).
#define UART_PRINT_WAIT(msg)                                                   \
	la t5, msg;                                                            \
	1 : lbu t6, 0(t5);                                                     \
	addu t5, 1;                                                            \
	beqz t6, 2f;                                                           \
	nop;                                                                   \
	3 : REG32_R(UART_LSR, t7);                                             \
	and t7, t7, UART_LSR_TX_EMPTY;                                         \
	beqz t7, 3b;                                                           \
	nop;                                                                   \
	sll t6, t6, 24;                                                        \
	REG32_W(UART_THR, t6);                                                 \
	j 1b;                                                                  \
	nop;                                                                   \
	2:

//----------------------------------------------------
#endif
