/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * stdlib.h - What libc.c and calloc.c provide
 *
 * The loader is freestanding: these are the only string, memory, number,
 * console and heap routines there are.  memcpy & co are ours because of
 * -ffreestanding; the list matches nm --defined-only on libc.o.
 */
#ifndef _STDLIB_H
#define _STDLIB_H

#include <stddef.h>
#include "rtl_types.h"

/* --- Memory and strings (libc.c) ------------------------------------------ */

void *memcpy(void *dest, const void *src, size_t count);
void *memset(void *s, int c, size_t count);
int memcmp(const void *cs, const void *ct, size_t count);
size_t strlen(const char *s);
char *strchr(const char *s, int c);

int strcmp(const char *cs, const char *ct);

/* --- Characters --- */

static inline int isspace(int ch)
{
	return (unsigned int)(ch - 9) < 5u || ch == ' ';
}

/* --- Numbers (libc.c) ----------------------------------------------------- */

unsigned long strtoul(const char *nptr, char **endptr, int base);
long strtol(const char *nptr, char **endptr, int base);

/* --- Console (libc.c) ----------------------------------------------------- */

void prom_printf(const char *fmt, ...);
int dprintf(const char *fmt, ...);
#define printf dprintf
void twiddle(void);
void ddump(unsigned char *pData, int len);

/* --- Command line (libc.c, for monitor.c) --------------------------------- */

void GetLine(char *buffer, const unsigned int size, int EchoFlag);
int GetArgc(const char *string);
char **GetArgv(const char *string);
char *StrUpr(char *string);
int Hex2Val(char *HexStr, unsigned long *PVal);

/* --- Heap (calloc.c) ------------------------------------------------------ */

void *malloc(uint32 nbytes);
void free(void *ap);

#endif /* _STDLIB_H */
