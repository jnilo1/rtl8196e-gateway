// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * checks_test.c - Host-side unit test of boot/include/checks.h
 *
 * The bound checks that protect the boot path (kernel header from flash),
 * the TFTP receiver (packet lengths, RAM window) and the auto-flash path
 * (image header) are pure functions, so they are tested natively here with
 * the Lidl board constants.  The negative cases — a kernel header pointing
 * into the loader, an upload wrapping over DRAM — must never be staged on
 * a board, so this is where they are exercised.
 *
 * Build and run: ./tests/run_host_tests.sh
 */
#include <stdio.h>
#include <stdlib.h>
#include "board.h"
#include "../boot/include/checks.h" /* by path: boot/include must not shadow libc */
#include "../boot/include/stage1_checks.h"

/* Lidl values, mirrored from main.h without pulling the SoC headers. */
#define TOP_KSEG0 (BOARD_DRAM_TOP_KSEG1 - 0x20000000)
#define RESERVED_TOP (TOP_KSEG0 - 0x3000)
#define FLOOR 0x80001000UL
#define LDR_START 0x80400000UL
#define LDR_END 0x80424600UL /* a plausible _end; the real one is in boot.nm */
#define KPART 0x1E0000UL

static int failures;

#define CHECK(cond)                                                            \
	do {                                                                   \
		if (!(cond)) {                                                 \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                            \
		}                                                              \
	} while (0)

static int khdr(unsigned long start, unsigned long len)
{
	return kernel_header_ok(start, len, KPART, FLOOR, RESERVED_TOP,
				LDR_START, LDR_END);
}

static int win(unsigned long addr, unsigned long len)
{
	return ram_window_ok(addr, len, FLOOR, RESERVED_TOP, LDR_START,
			     LDR_END);
}

static int ahdr(unsigned long head_off, unsigned long hdr_len,
		unsigned long upload, unsigned long burn, unsigned long burn_len)
{
	return autoflash_header_ok(head_off, 16, hdr_len, upload, burn,
				   burn_len, 0x1000000, 0x1000);
}

int main(void)
{
	/* --- stage-1.5 LZMA output window (S3) ---------------------------- */
	CHECK(stage1_lzma_output_ok(0x1000));
	CHECK(stage1_lzma_output_ok(BOARD_STAGE2_MAX_SIZE));
	CHECK(!stage1_lzma_output_ok(0));
	CHECK(!stage1_lzma_output_ok(BOARD_STAGE2_MAX_SIZE + 1));
	CHECK(!stage1_lzma_output_ok(0xffffffffUL));

	/* --- kernel header (S5) ------------------------------------------ */
	/* the shipped 6.18 kernel: start 0x80560000, len 0x15F962 */
	CHECK(khdr(0x80560000, 0x15F962));
	/* a kernel filling the whole partition still fits */
	CHECK(khdr(0x80500000, KPART));
	/* rejected: empty, oversized, misaligned */
	CHECK(!khdr(0x80500000, 0));
	CHECK(!khdr(0x80500000, KPART + 1));
	CHECK(!khdr(0x80500000, 0x1001)); /* 16-bit checksum needs pairs */
	CHECK(!khdr(0x80500002, 0x1000));
	/* rejected: one flipped bit lands inside the loader */
	CHECK(!khdr(0x80400000, 0x1000));
	CHECK(!khdr(0x80410000, 0x1000));
	CHECK(!khdr(0x803FF000, 0x2000)); /* starts below, runs into it */
	/* rejected: exception vectors, reserved pages, wrap, other segments */
	CHECK(!khdr(0x80000000, 0x1000));
	CHECK(!khdr(0x80000080, 0x1000));
	CHECK(!khdr(RESERVED_TOP - 0x800, 0x1000));
	CHECK(!khdr(TOP_KSEG0, 0x1000));
	CHECK(!khdr(0xFFFFF000, 0x2000));
	CHECK(!khdr(0x00500000, 0x1000)); /* physical, not KSEG0 */
	CHECK(!khdr(0xA0500000, 0x1000)); /* KSEG1 alias not accepted here */
	/* accepted: right up to the reserved line, right after the loader */
	CHECK(khdr(RESERVED_TOP - 0x1000, 0x1000));
	CHECK(khdr(LDR_END, 0x1000));
	CHECK(khdr(0x80100000, KPART)); /* below the loader, whole partition */

	/* --- RAM window for uploads (S1) --------------------------------- */
	CHECK(win(0x80100000, 0x53D0));	  /* test.bin */
	CHECK(win(0x80500000, 0x1000000)); /* a 16 MiB fullflash */
	CHECK(win(0x80100000, 0));
	CHECK(!win(0x80100000, LDR_START - 0x80100000 + 1)); /* reaches loader */
	CHECK(!win(0x80500000, RESERVED_TOP - 0x80500000 + 1));
	CHECK(!win(0x80500000, 0xFFFFFFFF)); /* wraps */
	CHECK(!win(0x80000000, 0x10));
	CHECK(!win(0x80400000, 0x10));
	CHECK(!win(0x80424000, 0x10));
	CHECK(win(0x80424600, 0x10));
	CHECK(!win(RESERVED_TOP, 0x10));
	CHECK(!win(0x90000000, 0x10));

	/* --- TFTP lengths (S1) ------------------------------------------- */
	CHECK(tftp_udp_len_ok(12, 512));  /* empty last block */
	CHECK(tftp_udp_len_ok(524, 512)); /* full block */
	CHECK(!tftp_udp_len_ok(11, 512)); /* would underflow */
	CHECK(!tftp_udp_len_ok(0, 512));
	CHECK(!tftp_udp_len_ok(525, 512));
	CHECK(!tftp_udp_len_ok(65535, 512));

	CHECK(udp_frame_lens_ok(60, 32, 12));	/* padded ACK-sized frame */
	CHECK(udp_frame_lens_ok(566, 552, 532)); /* a data block */
	CHECK(!udp_frame_lens_ok(60, 1500, 12));	/* IP len beyond the frame */
	CHECK(!udp_frame_lens_ok(566, 552, 600));	/* UDP len beyond IP */
	CHECK(!udp_frame_lens_ok(566, 552, 7));	/* UDP len below header */
	CHECK(!udp_frame_lens_ok(566, 20, 8));	/* IP len below IP+UDP */

	/* --- auto-flash header (S4) -------------------------------------- */
	/* kernel image: 16-byte header + payload, header kept (burn_len = len+16) */
	CHECK(ahdr(0, 0x15F962, 0x15F972, 0x20000, 0x15F972));
	/* boot image: header skipped */
	CHECK(ahdr(0, 0x55D2, 0x55E2, 0, 0x55D2));
	/* rootfs / userdata at their partitions */
	CHECK(ahdr(0, 0x1F0000, 0x1F0010, 0x200000, 0x1F0000));
	CHECK(ahdr(0, 0xC00000, 0xC00010, 0x400000, 0xC00000));
	/* rejected: header claims more than was uploaded */
	CHECK(!ahdr(0, 0x15F962, 0x15F000, 0x20000, 0x15F972));
	CHECK(!ahdr(0, 0xFFFFFFF0, 0x1000, 0x20000, 0x1000)); /* wrap */
	CHECK(!ahdr(0, 0, 0x1000, 0x20000, 0x10));
	CHECK(!ahdr(0, 0x1001, 0x1011, 0x20000, 0x1011));
	/* rejected: past the chip, misaligned destination */
	CHECK(!ahdr(0, 0x1000, 0x1010, 0xFFF800, 0x1000));
	CHECK(!ahdr(0, 0x1000, 0x1010, 0x1000000, 0x1000));
	CHECK(!ahdr(0, 0x1000, 0x1010, 0x20010, 0x1000));
	/* accepted: the very last sector of the chip */
	CHECK(ahdr(0, 0x1000, 0x1010, 0xFFF000, 0x1000));
	/* second image in a concatenation */
	CHECK(ahdr(0x15F972, 0x1F0000, 0x15F972 + 0x1F0010, 0x200000, 0x1F0000));
	CHECK(!ahdr(0x15F972, 0x1F0000, 0x15F972 + 0x1F0000, 0x200000, 0x1F0000));

	/* fixed partition ranges used by the stage-2 auto-flash policy */
	CHECK(flash_partition_ok(0x20000, 0x1e0000, 0x20000, 0x1e0000));
	CHECK(!flash_partition_ok(0x0, 0x1000, 0x20000, 0x1e0000));
	CHECK(!flash_partition_ok(0x1ff000, 0x2000, 0x20000, 0x1e0000));

	/* raw fullflash CRC trailer */
	{
		unsigned char t[16];
		unsigned long crc = 0;
		unsigned int i;

		/* the CRC-32 check value: crc32("123456789") = 0xCBF43926 */
		CHECK(crc32(0, "123456789", 9) == 0xCBF43926U);
		CHECK(crc32(crc32(0, "1234", 4), "56789", 5) == 0xCBF43926U);
		CHECK(crc32(0, "", 0) == 0);

		for (i = 0; i < 16; i++)
			t[i] = 0xff;
		CHECK(fullflash_trailer_parse(t, 0x1000000UL, &crc) ==
		      FULLFLASH_TRAILER_NONE);

		/* "FFCS", 0x01000000, 0xCBF43926, ~0xCBF43926 */
		{
			static const unsigned char v[16] = {
				'F', 'F', 'C', 'S', 0x01, 0x00, 0x00, 0x00,
				0xCB, 0xF4, 0x39, 0x26, 0x34, 0x0B, 0xC6, 0xD9};
			for (i = 0; i < 16; i++)
				t[i] = v[i];
		}
		CHECK(fullflash_trailer_parse(t, 0x1000000UL, &crc) ==
		      FULLFLASH_TRAILER_VALID);
		CHECK(crc == 0xCBF43926UL);
		/* wrong length claimed */
		CHECK(fullflash_trailer_parse(t, 0x800000UL, &crc) ==
		      FULLFLASH_TRAILER_FOREIGN);
		/* one bit of the complement flipped */
		t[15] ^= 1;
		CHECK(fullflash_trailer_parse(t, 0x1000000UL, &crc) ==
		      FULLFLASH_TRAILER_FOREIGN);
		t[15] ^= 1;
		/* wrong magic */
		t[0] = 'X';
		CHECK(fullflash_trailer_parse(t, 0x1000000UL, &crc) ==
		      FULLFLASH_TRAILER_FOREIGN);
		/* a single non-0xFF byte is foreign, not none */
		for (i = 0; i < 16; i++)
			t[i] = 0xff;
		t[7] = 0;
		CHECK(fullflash_trailer_parse(t, 0x1000000UL, &crc) ==
		      FULLFLASH_TRAILER_FOREIGN);
	}

	/* --- auto-flash upload tail: cvimg padding vs foreign bytes -------- */
	{
		unsigned char t[64];
		unsigned long i;

		CHECK(!upload_tail_is_padding(t, 0));
		for (i = 0; i < sizeof(t); i++)
			t[i] = 0x00;
		CHECK(upload_tail_is_padding(t, 1));
		CHECK(upload_tail_is_padding(t, sizeof(t)));
		for (i = 0; i < sizeof(t); i++)
			t[i] = 0xff;
		CHECK(upload_tail_is_padding(t, sizeof(t)));
		/* the two fill values do not mix */
		t[10] = 0x00;
		CHECK(!upload_tail_is_padding(t, sizeof(t)));
		/* one byte of anything else is a foreign header, not padding */
		for (i = 0; i < sizeof(t); i++)
			t[i] = 0x00;
		t[sizeof(t) - 1] = 0x01;
		CHECK(!upload_tail_is_padding(t, sizeof(t)));
		t[sizeof(t) - 1] = 0x00;
		t[0] = 'c';
		CHECK(!upload_tail_is_padding(t, sizeof(t)));
	}

	if (failures) {
		printf("%d check(s) FAILED\n", failures);
		return 1;
	}
	printf("checks_test: all checks passed\n");
	return 0;
}
