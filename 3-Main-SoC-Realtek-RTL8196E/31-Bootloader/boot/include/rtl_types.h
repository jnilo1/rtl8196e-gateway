/*
 * Copyright c                  Realtek Semiconductor Corporation, 2002
 * All rights reserved.
 *
 * Abstract : realtek type definition
 *
 * $Author: jasonwang $
 *
 * $Log: rtl_types.h,v $
 * Revision 1.1  2009/11/13 13:22:46  jasonwang
 * Added rtl8196c and 98 bootcode.
 *
 * Revision 1.1.1.1  2007/08/06 10:05:01  root
 * Initial import source to CVS
 *
 * Revision 1.5  2005/09/22 05:22:31  bo_zhao
 * *** empty log message ***
 *
 * Revision 1.1.1.1  2005/09/05 12:38:24  alva
 * initial import for add TFTP server
 *
 * Revision 1.4  2004/08/26 13:53:27  yjlou
 * -: remove all warning messages!
 * +: add compile flags "-Wno-implicit -Werror" in Makefile to treat warning as
 * error!
 *
 * Revision 1.3  2004/05/12 06:35:11  yjlou
 * *: fixed the ASSERT_CSP() and ASSERT_ISR() macro: print #x will cause
 * unpredictable result.
 *
 * Revision 1.2  2004/03/31 01:49:20  yjlou
 * *: all text files are converted to UNIX format.
 *
 * Revision 1.1  2004/03/16 06:36:13  yjlou
 * *** empty log message ***
 *
 * Revision 1.1.1.1  2003/09/25 08:16:56  tony
 *  initial loader tree
 *
 * Revision 1.1.1.1  2003/05/07 08:16:07  danwu
 * no message
 *
 */

#ifndef _RTL_TYPES_H
#define _RTL_TYPES_H

#include <stdint.h>

/* The Realtek names of the fixed-width types, as the switch code spells them. */
typedef uint32_t uint32;
typedef int32_t int32;
typedef uint16_t uint16;
typedef uint8_t uint8;
typedef int8_t int8;

/*
 * Error codes the switch driver and libc return: BSD numbering, as the
 * Realtek switch code has always used it; nothing outside the loader sees them.
 */
#define EEXIST 17     /* Entry exists */
#define EINVAL 22     /* Invalid argument */
#define ERANGE 34     /* Result out of range */
#define ECOLLISION 88 /* Table entry collision */

typedef struct {
	uint16 mac47_32;
	uint16 mac31_16;
	uint16 mac15_0;
	uint16 align;
} macaddr_t;

typedef struct ether_addr_s {
	uint8 octet[6];
} ether_addr_t;


#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#ifndef SUCCESS
#define SUCCESS 0
#endif
#ifndef FAILED
#define FAILED -1
#endif


#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif /* min */

#define ASSERT_CSP(x)                                                          \
	do {                                                                   \
		if (!(x))                                                      \
			fatal("assertion failed: " #x);                        \
	} while (0)

extern void fatal(const char *why) __attribute__((noreturn));

#endif
