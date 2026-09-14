// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * uart.c - UART driver (console I/O)
 *
 * RTL8196E stage-2 bootloader
 */

#include "boot_soc.h"
#include "uart.h"

/*
 * UART peek buffer: while pollingDownModeKeyword() drains the FIFO
 * looking for ESC, it stashes the first character that isn't ESC here
 * so serial_inc() can return it instead of losing it.  -1 means empty.
 */
int g_uart_peek = -1;

/*
 * Called repeatedly while serial_inc() waits for a character.  Download
 * mode points it at eth_poll() so the TFTP server runs from the main
 * loop between keystrokes; NULL until then.
 */
void (*g_uart_idle)(void) = 0;

/*
 * TX wait bound.  A loop count, not a calibrated delay: the loader keeps
 * going if the transmitter never drains (nothing attached), and the
 * bound only has to exceed the time one character takes at 38400 baud.
 */
#define UART_TX_TIMEOUT 6540

/* UART LSR bits (byte-accessed via rtl_inb) */
#define LSR_THRE 0x60 /* THR empty + transmitter empty */
#define LSR_DR 0x01   /* Data ready */

void serial_outc(char c)
{
	int i = 0;

	while (i < UART_TX_TIMEOUT) {
		if (rtl_inb(UART_LSR) & LSR_THRE)
			break;
		i++;
	}

	rtl_outb(UART_THR, c);

	if (c == '\n')
		rtl_outb(UART_THR, '\r');
}

char serial_inc(void)
{
	int ch;

	/* Return peeked character first */
	if (g_uart_peek >= 0) {
		ch = g_uart_peek;
		g_uart_peek = -1;
		return (char)ch;
	}

	while (!(rtl_inb(UART_LSR) & LSR_DR)) {
		if (g_uart_idle)
			g_uart_idle();
	}

	return (rtl_inb(UART_RBR) & 0xff);
}

void console_init(unsigned long cpu_clock)
{
	unsigned long divisor;
	unsigned long dll, dlm;

	/* 8N1, no DLAB */
	REG32(UART_LCR_REG) = 0x03000000;

	/* Enable and reset FIFOs */
	REG32(UART_FCR_REG) = 0xc7000000;

	/* No interrupts */
	REG32(UART_IER_REG) = 0x00000000;

	/* Compute baud rate divisor: divisor = (clock / 16) / baud - 1 */
	divisor = (cpu_clock / 16) / BAUD_RATE - 1;
	dll = divisor & 0xff;
	dlm = divisor / 0x100;

	/* Set DLAB to access divisor latches */
	REG32(UART_LCR_REG) = 0x83000000;
	REG32(UART_DLL_REG) = dll * 0x1000000;
	REG32(UART_DLM_REG) = dlm * 0x1000000;

	/* Clear DLAB */
	REG32(UART_LCR_REG) = 0x83000000 & 0x7fffffff;
}
