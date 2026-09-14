// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * crc32.c - CRC-32 (ISO 3309 / zlib polynomial 0xEDB88320)
 *
 * RTL8196E stage-2 bootloader
 *
 * Used to check the trailer that build_fullflash.sh appends to a raw
 * 16 MiB image (see checks.h, fullflash_trailer_parse).  The 1 KiB table
 * is built on first use so the image carries no data for it.  Plain C
 * with no SoC dependency: the host unit test compiles this file as is.
 *
 * Copyright (c) 2026 J. Nilo
 */


static unsigned int crc_table[256];
static int crc_table_ready;

static void crc32_init_table(void)
{
	unsigned int n, k, c;

	for (n = 0; n < 256; n++) {
		c = n;
		for (k = 0; k < 8; k++)
			c = (c & 1) ? 0xEDB88320U ^ (c >> 1) : c >> 1;
		crc_table[n] = c;
	}
	crc_table_ready = 1;
}

/*
 * crc32 - running CRC-32 of a buffer
 * @crc: 0 for the first call, the previous result to continue
 * @buf: bytes
 * @len: byte count
 *
 * Same convention as zlib's crc32(): crc32(crc32(0, a, la), b, lb) is the
 * CRC of a followed by b.
 */
unsigned int crc32(unsigned int crc, const void *buf, unsigned long len)
{
	const unsigned char *p = buf;

	if (!crc_table_ready)
		crc32_init_table();
	crc = ~crc;
	while (len--)
		crc = crc_table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
	return ~crc;
}
