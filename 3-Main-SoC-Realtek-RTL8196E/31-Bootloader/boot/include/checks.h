/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * checks.h - Pure bound checks shared by the boot path, the TFTP server and
 * the auto-flash path.
 *
 * Every function here is a pure function of its arguments with no SoC
 * dependency, so the same header compiles natively for the host-side unit
 * test in tests/checks_test.c.  Keep it that way: no register access, no
 * globals, no printing.
 */
#ifndef _CHECKS_H_
#define _CHECKS_H_

/* crc32.c (the fullflash trailer check) */
unsigned int crc32(unsigned int crc, const void *buf, unsigned long len);

/* Two half-open ranges [a, a+alen) and [b, b+blen) overlap. */
static inline int ranges_overlap(unsigned long a, unsigned long alen,
				 unsigned long b, unsigned long blen)
{
	if (alen == 0 || blen == 0)
		return 0;
	return a < b + blen && b < a + alen;
}

/*
 * Is [addr, addr+len) an acceptable RAM window for a load or a copy?
 *
 * @floor:      lowest acceptable address (above the exception vectors)
 * @ceiling:    one past the highest acceptable address (below the reserved pages)
 * @ldr_start:  start of the running loader image
 * @ldr_end:    one past the end of the running loader image (BSS included)
 *
 * The window must not wrap, must lie inside [floor, ceiling) and must not
 * touch the loader.  len == 0 is accepted (nothing is written).
 */
static inline int ram_window_ok(unsigned long addr, unsigned long len,
				unsigned long floor, unsigned long ceiling,
				unsigned long ldr_start, unsigned long ldr_end)
{
	if (addr < floor || addr >= ceiling)
		return 0;
	if (len > ceiling - addr) /* wraps or runs past the ceiling */
		return 0;
	if (ranges_overlap(addr, len, ldr_start, ldr_end - ldr_start))
		return 0;
	return 1;
}

/*
 * Kernel image header read from flash: is copying @len bytes to @start_addr
 * safe?  A corrupted header must be treated as "no image" (the loader then
 * enters download mode, which is the designed recovery), never acted on.
 *
 * @max_len: size of the kernel partition.
 */
static inline int kernel_header_ok(unsigned long start_addr, unsigned long len,
				   unsigned long max_len, unsigned long floor,
				   unsigned long ceiling, unsigned long ldr_start,
				   unsigned long ldr_end)
{
	if (len == 0 || len > max_len || (len & 1))
		return 0;
	if (start_addr & 3) /* the checksum loop reads 16-bit words */
		return 0;
	return ram_window_ok(start_addr, len, floor, ceiling, ldr_start,
			     ldr_end);
}

/*
 * TFTP DATA packet: UDP length field must describe 8 (UDP header) + 2
 * (opcode) + 2 (block) + 0..512 bytes of payload.
 */
#define TFTP_DATA_HDR 12
static inline int tftp_udp_len_ok(unsigned int udp_len, unsigned int blksize)
{
	return udp_len >= TFTP_DATA_HDR && udp_len <= TFTP_DATA_HDR + blksize;
}

/*
 * Lengths carried by a received IPv4/UDP frame must be consistent with what
 * the NIC actually delivered, otherwise the payload parsers read past the
 * receive cluster.
 *
 * @frame_len: bytes delivered by the NIC (Ethernet header included, FCS excluded)
 * @ip_len:    IPv4 total length field
 * @udp_len:   UDP length field
 */
static inline int udp_frame_lens_ok(unsigned int frame_len, unsigned int ip_len,
				    unsigned int udp_len)
{
	if (ip_len < 20 + 8)
		return 0;
	if (frame_len < 14 + ip_len)
		return 0;
	if (udp_len < 8 || ip_len < 20 + udp_len)
		return 0;
	return 1;
}

/*
 * Bytes left after the last image of an auto-flash upload. cvimg -a pads its
 * output to the alignment with 0x00, and an image carved out of erased flash
 * ends in 0xFF: a run of either, up to the end of the upload, is padding and
 * not another header. A run mixing the two values, or holding any other
 * byte, is foreign. An empty tail is not padding either (nothing to skip).
 */
static inline int upload_tail_is_padding(const unsigned char *p,
					 unsigned long n)
{
	unsigned char fill;
	unsigned long i;

	if (n == 0)
		return 0;
	fill = p[0];
	if (fill != 0x00 && fill != 0xff)
		return 0;
	for (i = 1; i < n; i++)
		if (p[i] != fill)
			return 0;
	return 1;
}

/*
 * Auto-flash: an image header found at @head_offset inside an upload of
 * @upload_len bytes claims @hdr_len payload bytes and a flash destination.
 *
 * @hdr_size:    size of the image header (16)
 * @burn_addr:   flash offset the image is written to
 * @burn_len:    bytes written to flash (payload, plus header when kept)
 * @chip_size:   flash size
 * @sector_size: erase granularity — burn_addr must be aligned, or the
 *               read-modify-write of the neighbouring sector could reach
 *               into the bootloader partition
 */
static inline int autoflash_header_ok(unsigned long head_offset,
				      unsigned long hdr_size,
				      unsigned long hdr_len,
				      unsigned long upload_len,
				      unsigned long burn_addr,
				      unsigned long burn_len,
				      unsigned long chip_size,
				      unsigned long sector_size)
{
	if (hdr_len == 0 || (hdr_len & 1) || (burn_len & 1))
		return 0;
	if (hdr_len > upload_len) /* guards the sums below against wrap */
		return 0;
	if (head_offset + hdr_size + hdr_len > upload_len)
		return 0;
	if (burn_addr >= chip_size || burn_len > chip_size - burn_addr)
		return 0;
	if (burn_addr % sector_size)
		return 0;
	return 1;
}

/* Is [burn_addr, burn_addr + burn_len) wholly inside one fixed partition? */
static inline int flash_partition_ok(unsigned long burn_addr,
				     unsigned long burn_len,
				     unsigned long part_addr,
				     unsigned long part_len)
{
	if (burn_len == 0 || burn_addr < part_addr)
		return 0;
	return burn_len <= part_len - (burn_addr - part_addr);
}

/*
 * Raw fullflash CRC trailer: the last 16 bytes of the bootloader partition
 * (0x1FFF0..0x1FFFF), a region the loader never uses and JFFS2 never
 * touches.  build_fullflash.sh and backup_gateway.sh write it, the raw
 * fullflash path checks it when it is there:
 *
 *   "FFCS" | image length (BE) | CRC-32 of the image minus these 16 bytes (BE) | ~CRC (BE)
 *
 * All 0xFF means no trailer and is rejected by the raw-fullflash path.
 * Anything else must be a matching trailer: foreign bytes are refused,
 * otherwise a damaged trailer would silently switch the integrity check off.
 */
#define FULLFLASH_TRAILER_OFFSET 0x1FFF0UL
#define FULLFLASH_TRAILER_LEN 16UL
#define FULLFLASH_TRAILER_MAGIC 0x46464353UL /* "FFCS" */

enum fullflash_trailer {
	FULLFLASH_TRAILER_NONE,    /* 16 x 0xFF */
	FULLFLASH_TRAILER_VALID,   /* magic, length and complement agree */
	FULLFLASH_TRAILER_FOREIGN, /* something else */
};

static inline unsigned long checks_be32(const unsigned char *p)
{
	return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
	       ((unsigned long)p[2] << 8) | p[3];
}

/*
 * fullflash_trailer_parse - classify the 16 bytes at FULLFLASH_TRAILER_OFFSET
 * @t: the 16 bytes
 * @image_len: the length the trailer must claim (the flash size)
 * @crc: on VALID, the CRC-32 the image must have
 */
static inline enum fullflash_trailer
fullflash_trailer_parse(const unsigned char *t, unsigned long image_len,
			unsigned long *crc)
{
	unsigned long i, c, nc;

	for (i = 0; i < FULLFLASH_TRAILER_LEN; i++)
		if (t[i] != 0xff)
			break;
	if (i == FULLFLASH_TRAILER_LEN)
		return FULLFLASH_TRAILER_NONE;

	c = checks_be32(t + 8);
	nc = checks_be32(t + 12);
	if (checks_be32(t) != FULLFLASH_TRAILER_MAGIC ||
	    checks_be32(t + 4) != image_len || (c ^ nc) != 0xffffffffUL)
		return FULLFLASH_TRAILER_FOREIGN;
	*crc = c;
	return FULLFLASH_TRAILER_VALID;
}

#endif /* _CHECKS_H_ */
