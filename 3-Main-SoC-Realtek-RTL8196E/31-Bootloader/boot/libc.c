// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * libc.c - C library: string, printf, CLI line editor, strtol
 *
 * RTL8196E stage-2 bootloader
 *
 * Copyright (c) 2009-2020 Realtek Semiconductor Corp.
 * Copyright (c) 2024-2026 J. Nilo
 */

#include "boot_common.h"
#include <stdarg.h>

/* No <limits.h> in a freestanding build: the compiler's own bounds. */
#define LONG_MAX __LONG_MAX__
#define LONG_MIN (-LONG_MAX - 1L)
#define ULONG_MAX (2UL * LONG_MAX + 1UL)
#include "boot_soc.h"
#include "boot_net.h"
#include "monitor.h"
#include "uart.h"

/* ===== String functions (io/string.c) ===== */

/**
 * strchr - Find the first occurrence of a character in a string
 * @s: string to search
 * @c: character to find
 *
 * Return: pointer to first occurrence of @c, or NULL
 */
char *strchr(const char *s, int c)
{
	for (; *s != (char)c; ++s)
		if (*s == '\0')
			return NULL;
	return (char *)s;
}
/**
 * strlen - Calculate the length of a string
 * @s: NUL-terminated string
 *
 * Return: number of characters (not including the NUL terminator)
 */
/**
 * strcmp - Compare two strings
 *
 * Return: 0 when equal, the difference of the first differing bytes
 * (as unsigned char) otherwise
 */
int strcmp(const char *cs, const char *ct)
{
	unsigned char c1, c2;

	do {
		c1 = *cs++;
		c2 = *ct++;
		if (c1 != c2)
			return c1 - c2;
	} while (c1);
	return 0;
}

size_t strlen(const char *s)
{
	const char *sc;

	for (sc = s; *sc != '\0'; ++sc)
		/* nothing */;
	return sc - s;
}

void *memset(void *s, int c, size_t count)
{
	char *xs = (char *)s;

	while (count--)
		*xs++ = c;

	return s;
}
void *memcpy(void *dest, const void *src, size_t count)
{
	char *tmp = (char *)dest, *s = (char *)src;

	while (count--)
		*tmp++ = *s++;

	return dest;
}
int memcmp(const void *cs, const void *ct, size_t count)
{
	const unsigned char *su1, *su2;
	signed char res = 0;

	for (su1 = cs, su2 = ct; 0 < count; ++su1, ++su2, count--)
		if ((res = *su1 - *su2) != 0)
			break;
	return res;
}

/* ===== Command line tools (io/ctool.c) ===== */

#define KEYCODE_BS 0x08
#define KEYCODE_TAB 0x09
#define KEYCODE_ESC 0x1B
#define KEYCODE_SP 0x20
#define KEYCODE_CR 0x0D
#define KEYCODE_LF 0x0A
#define KEYCODE_DEL 0x7F

#define TAB_WIDTH 8

#define GetChar() serial_inc()
#define PutChar(x) serial_outc(x)

static char *ArgvArray[MAX_ARGV];

/* Convert lowercase string to uppercase in-place */
char *StrUpr(char *string)
{
	char *p = string;

	while (*p) {
		if (*p >= 'a' && *p <= 'z')
			*p -= ('a' - 'A');
		p++;
	}
	return string;
}

/**
 * GetLine - Read a line of input from the serial console
 * @buffer: output buffer for the entered text
 * @size: maximum number of characters to read
 * @EchoFlag: if non-zero, echo typed characters back to console
 */
void GetLine(char *buffer, const unsigned int size, int EchoFlag)
{
	char *p = buffer;
	unsigned int n = 0;
	int c;
	char ch;
	int i;

	if (!size)
		return;

	while (1) {
		c = GetChar();
		if (c == -1)
			continue;

		ch = (char)c;

		if (ch == KEYCODE_ESC) {
			continue; /* discard ESC from key-repeat after boot */
		} else if (ch == KEYCODE_LF || ch == KEYCODE_CR) {
			break;
		} else if (ch == KEYCODE_BS || ch == KEYCODE_DEL) {
			if (p != buffer) {
				p--;
				n--;
				if (EchoFlag) {
					PutChar(KEYCODE_BS);
					PutChar(' ');
					PutChar(KEYCODE_BS);
				}
			}
		} else if (ch == KEYCODE_TAB) {
			for (i = 0; i < TAB_WIDTH; i++) {
				if (n + 1 >= size)
					break;
				*p++ = ' ';
				n++;
				if (EchoFlag)
					PutChar(' ');
			}
		} else {
			if (n + 1 < size) {
				*p++ = ch;
				n++;
				if (EchoFlag)
					PutChar(ch);
			}
		}
	}

	*p = '\0';
}

/**
 * GetArgc - Count space-separated arguments in a string
 * @string: input string
 *
 * Return: number of arguments (capped at MAX_ARGV - 1)
 */
int GetArgc(const char *string)
{
	int argc = 0;
	char *p = (char *)string;

	while (*p) {
		while (*p == ' ')
			p++;
		if (*p) {
			argc++;
			while (*p && *p != ' ')
				p++;
			continue;
		}
	}
	if (argc >= MAX_ARGV)
		argc = MAX_ARGV - 1;
	return argc;
}

/**
 * GetArgv - Split a string into an argv-style array
 * @string: input string (modified in-place: spaces become NUL)
 *
 * Return: pointer to the static argv array
 */
char **GetArgv(const char *string)
{
	char *p = (char *)string;
	int n = 0;

	memset(ArgvArray, 0, MAX_ARGV * sizeof(char *));
	while (*p && n < MAX_ARGV) {
		while (*p == ' ')
			p++;
		if (!*p)
			break;
		ArgvArray[n++] = p;
		while (*p && *p != ' ')
			p++;
		if (*p)
			*p++ = '\0';
	}
	return (char **)&ArgvArray;
}

/**
 * Hex2Val - Parse a hexadecimal string into an unsigned long
 * @HexStr: input hex string (no "0x" prefix)
 * @PVal: output pointer for the parsed value
 *
 * Return: TRUE on success, FALSE on invalid character or overflow
 */
int Hex2Val(char *HexStr, unsigned long *PVal)
{
	unsigned char *ptr = (unsigned char *)HexStr;
	unsigned long sum = 0, prev = 0;
	unsigned char c, hexval;

	if (!ptr || !*ptr)
		return FALSE;
	if (ptr[0] == '0' && (ptr[1] == 'x' || ptr[1] == 'X'))
		ptr += 2;
	if (!*ptr)
		return FALSE;

	while ((c = *ptr++)) {
		if (c >= '0' && c <= '9')
			hexval = c - '0';
		else if (c >= 'a' && c <= 'f')
			hexval = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F')
			hexval = c - 'A' + 10;
		else
			return FALSE;

		sum = prev * 16 + hexval;
		if (sum < prev)
			return FALSE; /* overflow */
		prev = sum;
	}
	*PVal = prev;
	return TRUE;
}

/* ===== Printf and friends (io/misc.c) ===== */

#ifdef RAMTEST_TRACE
static inline void ramtest_uart_putc(char c)
{
	volatile unsigned int *thr = (volatile unsigned int *)0xB8002000;
	for (volatile int i = 0; i < 0x4000; i++) {
		/* pre-spacing for UART MMIO */
	}
	*thr = ((unsigned int)c) << 24;
	for (volatile int i = 0; i < 0x4000; i++) {
		/* post-spacing for UART MMIO */
	}
	if (c == '\n') {
		for (volatile int i = 0; i < 0x4000; i++) {
			/* spacing before CR */
		}
		*thr = ((unsigned int)'\r') << 24;
		for (volatile int i = 0; i < 0x4000; i++) {
			/* spacing after CR */
		}
	}
}
#define putchar ramtest_uart_putc
#else
#define putchar serial_outc
#endif

/* --- Emit infrastructure --- */

typedef void (*emit_fn)(void *arg, char c);
struct print_ctx {
	emit_fn emit;
	void *arg;
	int count;
};

static void emit_console(void *arg, char c)
{
	(void)arg;
	putchar(c);
}

static void outc(struct print_ctx *ctx, char c)
{
	ctx->emit(ctx->arg, c);
	ctx->count++;
}

/* --- Formatting helpers --- */

static int str_len(const char *s)
{
	int n = 0;
	if (!s)
		return 0;
	while (*s++)
		n++;
	return n;
}

static void outs(struct print_ctx *ctx, const char *s, int width, char pad)
{
	int len;
	int i;

	if (!s)
		s = "(null)";
	len = str_len(s);
	if (width > len) {
		for (i = 0; i < width - len; i++)
			outc(ctx, pad);
	}
	for (; *s; s++)
		outc(ctx, *s);
}

static void outnum(struct print_ctx *ctx, unsigned long val, int base,
		   int width, char pad, int upper, int neg, int alt)
{
	char tmp[32];
	int len = 0;
	int i;
	int prefix = (alt && base == 16) ? 2 : 0;
	int total;
	int padlen;

	do {
		unsigned int d = (unsigned int)(val % (unsigned long)base);
		val /= (unsigned long)base;
		if (d < 10)
			tmp[len++] = (char)('0' + d);
		else
			tmp[len++] = (char)((upper ? 'A' : 'a') + (d - 10));
	} while (val);

	total = len + prefix + (neg ? 1 : 0);
	padlen = (width > total) ? (width - total) : 0;

	if (pad != '0') {
		for (i = 0; i < padlen; i++)
			outc(ctx, pad);
	}
	if (neg)
		outc(ctx, '-');
	if (prefix) {
		outc(ctx, '0');
		outc(ctx, upper ? 'X' : 'x');
	}
	if (pad == '0') {
		for (i = 0; i < padlen; i++)
			outc(ctx, pad);
	}
	while (len--)
		outc(ctx, tmp[len]);
}

/* --- vprintf core --- */

static int vprintf_internal(struct print_ctx *ctx, const char *fmt, va_list ap)
{
	ctx->count = 0;
	for (; *fmt; fmt++) {
		int width = 0;
		int alt = 0;
		char pad = ' ';

		if (*fmt != '%') {
			outc(ctx, *fmt);
			continue;
		}

		fmt++;
		if (*fmt == '%') {
			outc(ctx, '%');
			continue;
		}

		if (*fmt == '#') {
			alt = 1;
			fmt++;
		}
		if (*fmt == '0') {
			pad = '0';
			fmt++;
		}
		while (*fmt >= '0' && *fmt <= '9') {
			width = width * 10 + (*fmt - '0');
			fmt++;
		}

		/* silently consume 'l' modifier; long == int on MIPS32 */
		if (*fmt == 'l')
			fmt++;

		switch (*fmt) {
		case 'c':
			outc(ctx, (char)va_arg(ap, int));
			break;
		case 's':
			outs(ctx, va_arg(ap, const char *), width, pad);
			break;
		case 'd': {
			int v = va_arg(ap, int);
			unsigned long u =
			    (v < 0) ? (unsigned long)(-v) : (unsigned long)v;
			outnum(ctx, u, 10, width, pad, 0, v < 0, 0);
			break;
		}
		case 'u': {
			unsigned int v = va_arg(ap, unsigned int);
			outnum(ctx, v, 10, width, pad, 0, 0, 0);
			break;
		}
		case 'x':
		case 'X': {
			unsigned long v = va_arg(ap, unsigned long);
			outnum(ctx, v, 16, width, pad, (*fmt == 'X'), 0, alt);
			break;
		}
		case 'p': {
			unsigned long v = va_arg(ap, unsigned long);
			outnum(ctx, v, 16, (width ? width : 8), '0', 0, 0, 1);
			break;
		}
		default:
			outc(ctx, *fmt);
			break;
		}
	}
	return ctx->count;
}

/* --- Public API --- */

static int vprintf_console(const char *fmt, va_list ap)
{
	struct print_ctx ctx;

	ctx.emit = emit_console;
	ctx.arg = NULL;
	return vprintf_internal(&ctx, fmt, ap);
}

/**
 * prom_printf - Print a formatted string to the serial console
 * @fmt: printf-style format string
 */
void prom_printf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	(void)vprintf_console(fmt, ap);
	va_end(ap);
}

/**
 * dprintf - Print a formatted string to the console (returns count)
 * @fmt: printf-style format string
 *
 * Return: number of characters written
 */
int dprintf(const char *fmt, ...)
{
	va_list ap;
	int ret;
	va_start(ap, fmt);
	ret = vprintf_console(fmt, ap);
	va_end(ap);
	return ret;
}

/* --- Utilities --- */

static int twiddle_count;

void twiddle(void)
{
	static const char tiddles[] = "-\\|/";
	putchar(tiddles[(twiddle_count++) & 3]);
	putchar('\b');
}

void ddump(unsigned char *pData, int len)
{
	unsigned char *sbuf = pData;
	int length = len;

	int i = 0, j, offset;
	dprintf(
	    " [Addr]   .0 .1 .2 .3 .4 .5 .6 .7 .8 .9 .A .B .C .D .E .F\r\n");

	while (i < length) {

		dprintf("%08X: ", (sbuf + i));

		if (i + 16 < length)
			offset = 16;
		else
			offset = length - i;

		for (j = 0; j < offset; j++)
			dprintf("%02x ", sbuf[i + j]);

		for (j = 0; j < 16 - offset; j++) // a last line
			dprintf("   ");

		dprintf("    "); // between byte and char

		for (j = 0; j < offset; j++) {
			if (' ' <= sbuf[i + j] && sbuf[i + j] <= '~')
				dprintf("%c", sbuf[i + j]);
			else
				dprintf(".");
		}
		dprintf("\n\r");
		i += 16;
	}
}

/* ===== strtoul (io/strtoul.c) ===== */

/**
 * strtoul - Convert a string to an unsigned long integer
 * @nptr: string to convert
 * @endptr: if non-NULL, set to first unconverted character
 * @base: number base (0 for auto-detect, or 8/10/16)
 *
 * Return: parsed value, or ULONG_MAX on overflow
 */
unsigned long int strtoul(const char *nptr, char **endptr, int base)
{
	unsigned long int v = 0;

	while (isspace(*nptr))
		++nptr;
	if (*nptr == '+')
		++nptr;
	if (base == 16 && nptr[0] == '0')
		goto skip0x;
	if (!base) {
		if (*nptr == '0') {
			base = 8;
		skip0x:
			if (nptr[1] == 'x' || nptr[1] == 'X') {
				nptr += 2;
				base = 16;
			}
		} else
			base = 10;
	}
	while (*nptr) {
		register unsigned char c = *nptr;
		c = (c >= 'a'	? c - 'a' + 10
		     : c >= 'A' ? c - 'A' + 10
		     : c <= '9' ? c - '0'
				: 0xff);
		if (c >= base)
			break;
		{
			register unsigned long int w = v * base;
			if (w < v) {
				// errno=ERANGE;
				return ULONG_MAX;
			}
			v = w + c;
		}
		++nptr;
	}
	if (endptr)
		*endptr = (char *)nptr;
	// errno=0;	/* in case v==ULONG_MAX, ugh! */
	return v;
}

/* ===== strtol (io/strtol.c) ===== */

#define ABS_LONG_MIN 2147483648UL
/**
 * strtol - Convert a string to a signed long integer
 * @nptr: string to convert
 * @endptr: if non-NULL, set to first unconverted character
 * @base: number base (0 for auto-detect, or 8/10/16)
 *
 * Return: parsed value, or LONG_MIN/LONG_MAX on overflow
 */
long int strtol(const char *nptr, char **endptr, int base)
{
	int neg = 0;
	unsigned long int v;

	while (isspace(*nptr))
		nptr++;

	if (*nptr == '-') {
		neg = -1;
		++nptr;
	}
	v = strtoul(nptr, endptr, base);
	if (v >= ABS_LONG_MIN) {
		if (v == ABS_LONG_MIN && neg) {
			// errno=0;
			return v;
		}
		// errno=ERANGE;
		return (neg ? LONG_MIN : LONG_MAX);
	}
	return (neg ? -v : v);
}
