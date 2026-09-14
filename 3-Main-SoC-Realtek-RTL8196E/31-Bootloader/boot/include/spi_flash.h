/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * spi_flash.h - SPI flash: the on-flash layout and the API of flash.c
 *
 * Partitions (see build_fullflash.sh): bootloader at 0, kernel (cs6c header
 * kept) at 0x20000, squashfs rootfs at 0x200000, JFFS2 userdata at 0x400000.
 * The loader only needs to find the kernel; Linux locates the rootfs itself.
 */
#ifndef _SPI_FLASH_H_
#define _SPI_FLASH_H_

/* --- Layout --- */

#define SPI_FLASH_SIZE 0x01000000
#define BOOT_PARTITION_OFFSET 0x00000000
#define BOOT_PARTITION_SIZE 0x00020000
#define ROOTFS_PARTITION_OFFSET 0x00200000
#define ROOTFS_PARTITION_SIZE 0x00200000
#define USERDATA_PARTITION_OFFSET 0x00400000
#define USERDATA_PARTITION_SIZE (SPI_FLASH_SIZE - USERDATA_PARTITION_OFFSET)

/* Kernel partition: 0x20000..0x200000 */
#define KERNEL_PARTITION_OFFSET 0x00020000
#define KERNEL_PARTITION_SIZE 0x001E0000

/* Fixed kernel image slots tried first (flash offsets) */
#define CODE_IMAGE_OFFSET (64 * 1024)   /* 0x10000 */
#define CODE_IMAGE_OFFSET2 (128 * 1024) /* 0x20000 — the shipped layout */
#define CODE_IMAGE_OFFSET3 (192 * 1024) /* 0x30000 */

/* Then every 64 KiB step of this range */
#define CONFIG_LINUX_IMAGE_OFFSET_START 0x00020000
#define CONFIG_LINUX_IMAGE_OFFSET_END 0x01000000
#define CONFIG_LINUX_IMAGE_OFFSET_STEP 0x00010000

/* --- flash.c --- */

/* Probe: JEDEC ID, geometry, memory-mapped read window */
void spi_probe(void);

/* Chip name and raw 3-byte JEDEC ID, for the banner */
extern const char *g_flash_chip_name;
extern unsigned int g_flash_jedec_id;

/* Read `length` bytes from flash offset `src` into RAM at `dst`. Returns 1. */
int flashread(unsigned long dst, unsigned int src, unsigned long length);

/* Erase + program + read-back verify. Returns 1 on success, 0 on failure. */
int spi_flw_image(unsigned int flash_addr_offset, unsigned char *image_addr,
		  unsigned int image_size);

#endif /* _SPI_FLASH_H_ */
