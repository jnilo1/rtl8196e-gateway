// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * flash.c - SPI flash driver (probe, read, write, erase, verify)
 *
 * RTL8196E stage-2 bootloader
 *
 * Single SPI NOR chip on CS0.  The public API is at the bottom of the file
 * (spi_probe, flashread, spi_flw_image); everything above it is the
 * Realtek controller sequence, kept as it was validated on the hardware.
 *
 * Writes are verified: every sector or block programmed is read back and
 * compared with the source before the next one is touched, so a failed
 * program cycle is reported instead of being followed by a reboot into
 * nothing.  Whole 64 KiB blocks are erased with one block-erase command
 * (datasheet typical 300 ms) instead of sixteen sector erases (16 × 50 ms).
 *
 * Copyright (c) 2009-2020 Realtek Semiconductor Corp.
 * Copyright (c) 2024-2026 J. Nilo
 */

#include "boot_common.h"
#include "ramtest_trace.h"
#include "boot_soc.h"
#include "spi_flash.h"
#include <rtl_types.h>

/* --- Geometry of the one supported part and the controller sequence layer.
 * Nothing outside this file uses them (a static linkage would change the
 * inlining, hence the image). --- */

#define SPI_BLOCK_SIZE 0x10000 /* 64 KiB, erase command D8 */
#define SPI_SECTOR_SIZE 0x1000 /* 4 KiB, erase command 20 */
#define SPI_PAGE_SIZE 0x100    /* 256 B, program command 02 */

struct spi_flash_type {
	unsigned int chip_id;
	unsigned char mfr_id;
	unsigned char dev_id;
	unsigned char capacity_id;
	unsigned char device_size; /* 2^N bytes */
	unsigned int chip_size;
	unsigned int block_size;
	unsigned int sector_size;
	unsigned int page_size;
	unsigned int chip_clk;
	const char *chip_name;
};

/* Controller sequence layer */
void SFCSR_CS_L(unsigned char ucLen, unsigned char ucIOWidth);
void SFCSR_CS_H(unsigned char ucLen, unsigned char ucIOWidth);
void SeqCmd_Order(unsigned char ucIOWidth, unsigned int uiCmd);
void SeqCmd_Write(unsigned char ucIOWidth, unsigned int uiCmd,
		  unsigned int uiValue, unsigned char ucValueLen);
unsigned int SeqCmd_Read(unsigned char ucIOWidth, unsigned int uiCmd,
			 unsigned char ucRDLen);
void ComSrlCmd_InputCommand(unsigned int uiAddr, unsigned int uiCmd,
			    unsigned char ucIsFast, unsigned char ucIOWidth,
			    unsigned char ucDummyCount);
unsigned int ComSrlCmd_ComRead(unsigned int uiAddr, unsigned int uiLen,
			       unsigned char *pucBuffer, unsigned int uiCmd,
			       unsigned char ucIsFast, unsigned char ucIOWidth,
			       unsigned char ucDummyCount);
unsigned int ComSrlCmd_ComWrite(unsigned int uiAddr, unsigned int uiLen,
				unsigned char *pucBuffer, unsigned int uiCmd,
				unsigned char ucIsFast, unsigned char ucIOWidth,
				unsigned char ucDummyCount);


#define NDEBUG(args...) printf(args)
#define KDEBUG(args...)
#define LDEBUG(args...)

const char *g_flash_chip_name = "UNKNOWN";
unsigned int g_flash_jedec_id = 0;
static int g_flash_writable = 0;
static int g_spi_fault = 0;

static unsigned int g_flash_write_total = 0;
static unsigned int g_flash_write_done = 0;
static int g_flash_write_last_pct = -1;

/* One sector for read-modify-write, one for read-back: BSS, not stack. */
static unsigned char sector_buf[SPI_SECTOR_SIZE];
static unsigned char verify_buf[SPI_SECTOR_SIZE];

static void flash_write_progress_add(unsigned int bytes);

#ifdef RAMTEST_TRACE
/*
 * Fault injection for the RAM-test build only: set to non-zero from the
 * console (EW <address of g_flash_verify_poison> 1, address in boot.nm)
 * and the next read-back verify sees one corrupted byte, which must be
 * reported as a failed write.  Self-clearing.
 */
unsigned int g_flash_verify_poison = 0;
#endif

// Reset flash-write progress counter
static void flash_write_progress_reset(unsigned int total)
{
	g_flash_write_total = total;
	g_flash_write_done = 0;
	g_flash_write_last_pct = -1;
	if (total == 0) {
		return;
	}
	flash_write_progress_add(0);
}

// Update flash-write progress by bytes written
static void flash_write_progress_add(unsigned int bytes)
{
	unsigned int pct;

	rt_wdt_kick(); /* RAM-test build: a flash write outlasts the watchdog */
	if (g_flash_write_total == 0) {
		return;
	}
	if (g_flash_write_done + bytes >= g_flash_write_total) {
		g_flash_write_done = g_flash_write_total;
	} else {
		g_flash_write_done += bytes;
	}
	pct = (g_flash_write_done * 100U) / g_flash_write_total;
	if ((int)pct != g_flash_write_last_pct) {
		g_flash_write_last_pct = (int)pct;
		NDEBUG("\rFlashing: %d%%", (int)pct);
	}
}

#define SIZEN_16M 0x18 /* JEDEC capacity code: 2^24 bytes */
#define SIZE_256B 0x100
#define SIZE_004K 0x1000
#define SIZE_064K 0x10000

/* SPI Flash Configuration Register(SFCR) (0xb800-1200) */
#define SFCR 0xb8001200 /*SPI Flash Configuration Register*/
#define SFCR_SPI_CLK_DIV(val) ((val) << 29)
#define SFCR_RBO(val) ((val) << 28)
#define SFCR_WBO(val) ((val) << 27)
#define SFCR_SPI_TCS(val) ((val) << 22) /* 8196C and later: 5 bits */

/* SPI Flash Configuration Register(SFCR2) (0xb800-1204) */
#define SFCR2 0xb8001204
#define SFCR2_SFCMD(val) ((val) << 24)	/*8 bit, 1111_1111 */
#define SFCR2_SFSIZE(val) ((val) << 21) /*3 bit, 111 */
#define SFCR2_RD_OPT(val) ((val) << 20)
#define SFCR2_CMD_IO(val) ((val) << 18)	     /*2 bit, 11 */
#define SFCR2_ADDR_IO(val) ((val) << 16)     /*2 bit, 11 */
#define SFCR2_DUMMY_CYCLE(val) ((val) << 13) /*3 bit, 111 */
#define SFCR2_DATA_IO(val) ((val) << 11)     /*2 bit, 11 */
#define SFCR2_HOLD_TILL_SFDR2(val) ((val) << 10)

/* SPI Flash Control and Status Register(SFCSR)(0xb800-1208) */
#define SFCSR 0xb8001208
#define SFCSR_SPI_CSB0(val) ((val) << 31)
#define SFCSR_SPI_CSB1(val) ((val) << 30)
#define SFCSR_LEN(val) ((val) << 28) /*2 bits*/
#define SFCSR_SPI_RDY(val) ((val) << 27)
#define SFCSR_IO_WIDTH(val) ((val) << 25) /*2 bits*/
#define SFCSR_CHIP_SEL(val) ((val) << 24)
#define SFCSR_CMD_BYTE(val) ((val) << 16) /*8 bit, 1111_1111 */

#define SFCSR_SPI_CSB(val) ((val) << 30)

/* SPI Flash Data Register(SFDR)(0xb800-120c) */
#define SFDR 0xb800120c

#define SPICMD_WREN (0x06 << 24)     /* set the write enable latch */
#define SPICMD_WRDI (0x04 << 24)     /* reset the write enable latch */
#define SPICMD_RDID (0x9f << 24)     /* JEDEC ID: manufacturer + 2 device bytes */
#define SPICMD_RDSR (0x05 << 24)     /* read status register */
#define SPICMD_FASTREAD (0x0b << 24) /* 0b a1 a2 a3 dd, n bytes until CS# high */
#define SPICMD_SE (0x20 << 24)	     /* 4 KiB sector erase */
#define SPICMD_BE (0xd8 << 24)	     /* 64 KiB block erase */
#define SPICMD_PP (0x02 << 24)	     /* page program */
#define SPI_STATUS_WIP 0x00	     /* write in process bit */

#define SPI_REG_READ(reg) *((volatile unsigned int *)(reg))

/*
 * Controller transactions normally finish in a handful of bus cycles.
 * The NOR operation itself has a separate, longer budget below.  A stuck
 * controller is latched as a fault so that no later flash write is attempted.
 */
#define SPI_CONTROLLER_POLL_LIMIT 1000000U
#define SPI_FLASH_WIP_POLL_LIMIT 10000000U

static void spi_set_fault(const char *where)
{
	if (!g_spi_fault)
		NDEBUG("SPI timeout: %s\n", where);
	g_spi_fault = 1;
	g_flash_writable = 0;
}

static int spi_controller_ready(void)
{
	unsigned int count;

	for (count = 0; count < SPI_CONTROLLER_POLL_LIMIT; count++)
		if (SPI_REG_READ(SFCSR) & SFCSR_SPI_RDY(1))
			return 1;
	spi_set_fault("controller busy");
	return 0;
}

static void spi_reg_load(unsigned int reg, unsigned int val)
{
	if (!spi_controller_ready())
		return;
	*((volatile unsigned int *)reg) = val;
}

#define SPI_REG_LOAD(reg, val) spi_reg_load((reg), (val))

#define IOWIDTH_SINGLE 0x00
#define DATA_LENTH1 0x00
#define ISFAST_NO 0x00
#define ISFAST_YES 0x01
#define ISFAST_ALL 0x02
#define DUMMYCOUNT_0 0x00
#define DUMMYCOUNT_1 0x01

/* The one chip, on CS0. */
#define CHIP 0

struct spi_flash_type spi_flash_info;

/****************************** Common function ******************************/
// DRAM/bus clock in MHz from the strap register, indexed by bits 12:10.
// Table values as shipped in the Realtek SDK; the SPI divider is derived
// from it.  Only the 193 MHz entry has been exercised on our boards.
static unsigned int CheckDramFreq(void)
{
	unsigned short usFreqBit;
	static const unsigned short usFreqVal[] = {156, 193, 181, 231,
						   212, 125, 237, 168};
	usFreqBit = (0x00001C00 & (*(volatile unsigned int *)0xb8000008)) >> 10;
	return usFreqVal[usFreqBit];
}
// Configure SPI clock divider in SFCR register
static void setFSCR(unsigned int uiClkMhz, unsigned int uiRBO,
		    unsigned int uiWBO, unsigned int uiTCS)
{
	unsigned int ui, uiClk;
	uiClk = CheckDramFreq();
	ui = uiClk / uiClkMhz;
	if ((uiClk % uiClkMhz) > 0) {
		ui = ui + 1;
	}
	if ((ui % 2) > 0) {
		ui = ui + 1;
	}
	spi_flash_info.chip_clk = uiClk / ui;
	SPI_REG_LOAD(SFCR, SFCR_SPI_CLK_DIV((ui - 2) / 2) | SFCR_RBO(uiRBO) |
			       SFCR_WBO(uiWBO) | SFCR_SPI_TCS(uiTCS));
}

// Poll status register WIP bit until flash is ready
static int spiFlashReady(void)
{
	unsigned int uiCount, ui;

	for (uiCount = 0; uiCount < SPI_FLASH_WIP_POLL_LIMIT; uiCount++) {
		ui = SeqCmd_Read(IOWIDTH_SINGLE, SPICMD_RDSR, 1);
		if (g_spi_fault)
			return 0;
		if ((ui & (1 << SPI_STATUS_WIP)) == 0) {
			return 1;
		}
	}
	spi_set_fault("flash WIP");
	return 0;
}

// Toggle CS to reset SPI flash state machine
static void rstSPIFlash(void)
{
	SFCSR_CS_L(0, IOWIDTH_SINGLE);
	SFCSR_CS_H(0, IOWIDTH_SINGLE);
	SFCSR_CS_L(0, IOWIDTH_SINGLE);
	SFCSR_CS_H(0, IOWIDTH_SINGLE);
}

// Assert chip-select (CS low) with given length and IO width
void SFCSR_CS_L(unsigned char ucLen, unsigned char ucIOWidth)
{
	if (!spi_controller_ready())
		return;
	*((volatile unsigned int *)(SFCSR)) =
	    SFCSR_SPI_CSB(1 + CHIP) | SFCSR_LEN(ucLen) | SFCSR_SPI_RDY(1) |
	    SFCSR_IO_WIDTH(ucIOWidth) | SFCSR_CHIP_SEL(0) | SFCSR_CMD_BYTE(5);
}

// Deassert chip-select (CS high)
void SFCSR_CS_H(unsigned char ucLen, unsigned char ucIOWidth)
{
	if (ucLen == 0)
		ucLen = 1;
	if (!spi_controller_ready())
		return;
	*((volatile unsigned int *)(SFCSR)) =
	    SFCSR_SPI_CSB(3) | SFCSR_LEN(ucLen) | SFCSR_SPI_RDY(1) |
	    SFCSR_IO_WIDTH(ucIOWidth) | SFCSR_CHIP_SEL(0) | SFCSR_CMD_BYTE(5);
}

// Read JEDEC ID (Command 9F) — returns 3-byte manufacturer+device ID
static unsigned int ComSrlCmd_RDID(unsigned int uiLen)
{
	unsigned int ui;
	SPI_REG_LOAD(SFCR, (SFCR_SPI_CLK_DIV(7) | SFCR_RBO(1) | SFCR_WBO(1) |
			    SFCR_SPI_TCS(31))); // SFCR default setting
	rstSPIFlash();
	SFCSR_CS_L(0, IOWIDTH_SINGLE);
	SPI_REG_LOAD(SFDR, SPICMD_RDID);
	SFCSR_CS_L((uiLen - 1), IOWIDTH_SINGLE);
	ui = SPI_REG_READ(SFDR);
	SFCSR_CS_H(0, IOWIDTH_SINGLE);
	return g_spi_fault ? 0 : ui;
}

// Send a single-byte SPI command (no data phase)
void SeqCmd_Order(unsigned char ucIOWidth, unsigned int uiCmd)
{
	SFCSR_CS_L(ucIOWidth, IOWIDTH_SINGLE);
	SPI_REG_LOAD(SFDR, uiCmd);
	SFCSR_CS_H(ucIOWidth, IOWIDTH_SINGLE);
}

// Send a SPI command followed by a data write
void SeqCmd_Write(unsigned char ucIOWidth, unsigned int uiCmd,
		  unsigned int uiValue, unsigned char ucValueLen)
{
	SFCSR_CS_L(DATA_LENTH1, ucIOWidth);
	SPI_REG_LOAD(SFDR, uiCmd);
	SFCSR_CS_L(ucValueLen - 1, ucIOWidth);
	SPI_REG_LOAD(SFDR, (uiValue << ((4 - ucValueLen) * 8)));
	SFCSR_CS_H(DATA_LENTH1, IOWIDTH_SINGLE);
}

// Send a SPI command and read back data
unsigned int SeqCmd_Read(unsigned char ucIOWidth, unsigned int uiCmd,
			 unsigned char ucRDLen)
{
	unsigned int ui;
	SFCSR_CS_L(DATA_LENTH1, ucIOWidth);
	SPI_REG_LOAD(SFDR, uiCmd);
	SFCSR_CS_L(ucRDLen - 1, ucIOWidth);
	ui = SPI_REG_READ(SFDR);
	SFCSR_CS_H(DATA_LENTH1, ucIOWidth);
	ui = ui >> ((4 - ucRDLen) * 8);
	return g_spi_fault ? 0 : ui;
}

// Sector Erase (Command 20) — erase one 4 KB sector
static unsigned int ComSrlCmd_SE(unsigned int uiAddr)
{
	SeqCmd_Order(IOWIDTH_SINGLE, SPICMD_WREN);
	SeqCmd_Write(IOWIDTH_SINGLE, SPICMD_SE, uiAddr, 3);
	return spiFlashReady();
}

// Block Erase (Command D8) — erase one 64 KB block
static unsigned int ComSrlCmd_BE(unsigned int uiAddr)
{
	SeqCmd_Order(IOWIDTH_SINGLE, SPICMD_WREN);
	SeqCmd_Write(IOWIDTH_SINGLE, SPICMD_BE, uiAddr, 3);
	return spiFlashReady();
}

// Send SPI command + 3-byte address + dummy cycles
void ComSrlCmd_InputCommand(unsigned int uiAddr, unsigned int uiCmd,
			    unsigned char ucIsFast, unsigned char ucIOWidth,
			    unsigned char ucDummyCount)
{
	int i;

	// input command
	if (ucIsFast == ISFAST_ALL) {
		SFCSR_CS_L(0, ucIOWidth);
	} else {
		SFCSR_CS_L(0, IOWIDTH_SINGLE);
	}
	SPI_REG_LOAD(SFDR, uiCmd); // Read Command

	// input 3 bytes address
	if (ucIsFast == ISFAST_NO) {
		SFCSR_CS_L(0, IOWIDTH_SINGLE);
	} else {
		SFCSR_CS_L(0, ucIOWidth);
	}
	SPI_REG_LOAD(SFDR, (uiAddr << 8));
	SPI_REG_LOAD(SFDR, (uiAddr << 16));
	SPI_REG_LOAD(SFDR, (uiAddr << 24));

	// input dummy cycle
	for (i = 0; i < ucDummyCount; i++) {
		SPI_REG_LOAD(SFDR, 0);
	}

	SFCSR_CS_L(3, ucIOWidth);
}

/*
 * spi_enable_mmap_read - program SFCR2 so the flash appears at FLASH_BASE
 *
 * The memory-mapped window (0xBD000000, single-IO fast read with one
 * dummy byte, the same command the PIO read path uses) is what
 * check_system_image() reads image headers through.  Called once from
 * spi_probe(); it used to be a side effect of the first PIO read.
 */
static void spi_enable_mmap_read(void)
{
	unsigned int ui, uiDy;
	unsigned int uiCmd = SPICMD_FASTREAD >> 24;
	unsigned char ucIOWidth = IOWIDTH_SINGLE;
	unsigned char ucDummyCount = DUMMYCOUNT_1;

	ui = SFCR2_SFCMD(uiCmd) | SFCR2_SFSIZE(spi_flash_info.device_size - 17) |
	     SFCR2_RD_OPT(0) | SFCR2_HOLD_TILL_SFDR2(0);
	/* ISFAST_YES: command single-IO, address and data at ucIOWidth */
	ui = ui | SFCR2_CMD_IO(IOWIDTH_SINGLE) | SFCR2_ADDR_IO(ucIOWidth) |
	     SFCR2_DATA_IO(ucIOWidth);
	uiDy = ucIOWidth * 2;
	if (uiDy == 0) {
		uiDy = 1;
	}
	/* ucDummyCount is a byte count: ucDummyCount*8 / (uiDy*2) cycles */
	ui = ui | SFCR2_DUMMY_CYCLE((ucDummyCount * 4 / uiDy));
	SPI_REG_LOAD(SFCR2, ui);
}

// Generic SPI flash read — command + address + dummy + data
unsigned int ComSrlCmd_ComRead(unsigned int uiAddr, unsigned int uiLen,
			       unsigned char *pucBuffer, unsigned int uiCmd,
			       unsigned char ucIsFast, unsigned char ucIOWidth,
			       unsigned char ucDummyCount)
{
	unsigned int ui, uiCount, i;
	unsigned char *puc = pucBuffer;

	ComSrlCmd_InputCommand(uiAddr, uiCmd, ucIsFast, ucIOWidth, ucDummyCount);
	if (g_spi_fault)
		return 0;

	uiCount = uiLen / 4;
	for (i = 0; i < uiCount; i++) // Read 4 bytes every time.
	{
		ui = SPI_REG_READ(SFDR);
		memcpy(puc, &ui, 4);
		puc += 4;
	}

	i = uiLen % 4;
	if (i > 0) {
		ui = SPI_REG_READ(SFDR); // another bytes.
		memcpy(puc, &ui, i);
		puc += i;
	}
	SFCSR_CS_H(0, IOWIDTH_SINGLE);
	return g_spi_fault ? 0 : uiLen;
}

// Generic SPI flash write — WREN + command + address + data
unsigned int ComSrlCmd_ComWrite(unsigned int uiAddr, unsigned int uiLen,
				unsigned char *pucBuffer, unsigned int uiCmd,
				unsigned char ucIsFast, unsigned char ucIOWidth,
				unsigned char ucDummyCount)
{
	unsigned int ui, uiCount, i;
	unsigned char *puc = pucBuffer;

	SeqCmd_Order(IOWIDTH_SINGLE, SPICMD_WREN);

	ComSrlCmd_InputCommand(uiAddr, uiCmd, ucIsFast, ucIOWidth, ucDummyCount);
	if (g_spi_fault)
		return 0;

	uiCount = uiLen / 4;
	for (i = 0; i < uiCount; i++) {
		memcpy(&ui, puc, 4);
		puc += 4;
		SPI_REG_LOAD(SFDR, ui);
	}

	i = uiLen % 4;
	if (i > 0) {
		memcpy(&ui, puc, i);
		puc += i;
		SFCSR_CS_L(i - 1, ucIOWidth);
		SPI_REG_LOAD(SFDR, ui);
	}
	SFCSR_CS_H(0, IOWIDTH_SINGLE);
	if (!spiFlashReady())
		return 0;
	return uiLen;
}

// Fast Read (Command 0B) — single-IO with 1 dummy byte
static unsigned int mxic_cmd_read_s1(unsigned int uiAddr, unsigned int uiLen,
				     unsigned char *pucBuffer)
{
	return ComSrlCmd_ComRead(uiAddr, uiLen, pucBuffer, SPICMD_FASTREAD,
				 ISFAST_YES, IOWIDTH_SINGLE, DUMMYCOUNT_1);
}
// Page Program (PP) Sequence (Command 02) — single-IO
static unsigned int mxic_cmd_write_s1(unsigned int uiAddr, unsigned int uiLen,
				      unsigned char *pucBuffer)
{
	return ComSrlCmd_ComWrite(uiAddr, uiLen, pucBuffer, SPICMD_PP,
				  ISFAST_NO, IOWIDTH_SINGLE, DUMMYCOUNT_0);
}

/****************************** Write path ******************************/

/*
 * Read @len bytes back from @addr and compare with @src.  Reports the
 * first mismatching byte.  Return: 1 if identical, 0 otherwise.
 */
static int flash_verify(unsigned int addr, const unsigned char *src,
			unsigned int len)
{
	unsigned int n, i;

	while (len) {
		n = len > SPI_SECTOR_SIZE ? SPI_SECTOR_SIZE : len;
		if (mxic_cmd_read_s1(addr, n, verify_buf) != n)
			return 0;
#ifdef RAMTEST_TRACE
		if (g_flash_verify_poison) {
			g_flash_verify_poison = 0;
			verify_buf[0] ^= 0xFF;
		}
#endif
		for (i = 0; i < n; i++) {
			if (verify_buf[i] != src[i]) {
				NDEBUG("\nFlash verify FAILED at 0x%x: wrote "
				       "%02x, read %02x\n",
				       addr + i, src[i], verify_buf[i]);
				return 0;
			}
		}
		addr += n;
		src += n;
		len -= n;
	}
	return 1;
}

/* Program @npages consecutive pages from @src starting at @addr (already erased). */
static int program_pages(unsigned int addr, const unsigned char *src,
			 unsigned int npages)
{
	unsigned int i;

	for (i = 0; i < npages; i++) {
		if (mxic_cmd_write_s1(addr, SPI_PAGE_SIZE,
				      (unsigned char *)src) != SPI_PAGE_SIZE)
			return 0;
		addr += SPI_PAGE_SIZE;
		src += SPI_PAGE_SIZE;
	}
	return 1;
}

/* Erase, program and verify one whole 4 KiB sector. */
static int write_sector(unsigned int addr, const unsigned char *src)
{
	if (!ComSrlCmd_SE(addr))
		return 0;
	if (!program_pages(addr, src, SPI_SECTOR_SIZE / SPI_PAGE_SIZE))
		return 0;
	return flash_verify(addr, src, SPI_SECTOR_SIZE);
}

/* Erase, program and verify one whole 64 KiB block (block-aligned addr). */
static int write_block(unsigned int addr, const unsigned char *src)
{
	if (!ComSrlCmd_BE(addr))
		return 0;
	if (!program_pages(addr, src, SPI_BLOCK_SIZE / SPI_PAGE_SIZE))
		return 0;
	return flash_verify(addr, src, SPI_BLOCK_SIZE);
}

/*
 * Write @len bytes (less than a sector, not crossing a sector boundary)
 * at @addr through a read-modify-write of the containing sector.
 */
static int write_partial_sector(unsigned int addr, const unsigned char *src,
				unsigned int len)
{
	unsigned int off = addr % SPI_SECTOR_SIZE;
	unsigned int base = addr - off;

	if (mxic_cmd_read_s1(base, SPI_SECTOR_SIZE, sector_buf) !=
	    SPI_SECTOR_SIZE)
		return 0;
	memcpy(sector_buf + off, src, len);
	return write_sector(base, sector_buf);
}

/*
 * Write an arbitrary range: partial head sector, whole sectors (grouped
 * into 64 KiB block erases whenever the address is block-aligned and at
 * least a block remains), partial tail sector.  Every unit is verified
 * right after it is programmed.  Return: 1 on success, 0 on the first
 * verify failure.
 */
static int flash_write_range(unsigned int addr, const unsigned char *src,
			     unsigned int len)
{
	unsigned int n;

	flash_write_progress_reset(len);

	/* head: up to the next sector boundary */
	if (addr % SPI_SECTOR_SIZE) {
		n = SPI_SECTOR_SIZE - (addr % SPI_SECTOR_SIZE);
		if (n > len)
			n = len;
		if (!write_partial_sector(addr, src, n))
			return 0;
		flash_write_progress_add(n);
		addr += n;
		src += n;
		len -= n;
	}

	/* body: whole sectors, by block when possible */
	while (len >= SPI_SECTOR_SIZE) {
		if ((addr % SPI_BLOCK_SIZE) == 0 && len >= SPI_BLOCK_SIZE) {
			if (!write_block(addr, src))
				return 0;
			n = SPI_BLOCK_SIZE;
		} else {
			if (!write_sector(addr, src))
				return 0;
			n = SPI_SECTOR_SIZE;
		}
		flash_write_progress_add(n);
		addr += n;
		src += n;
		len -= n;
	}

	/* tail */
	if (len) {
		if (!write_partial_sector(addr, src, len))
			return 0;
		flash_write_progress_add(len);
	}

	SeqCmd_Order(IOWIDTH_SINGLE, SPICMD_WRDI);
	return !g_spi_fault;
}

/****************************** Public API ******************************/

/**
 * spi_probe - Identify the SPI flash and configure the controller
 *
 * Reads the JEDEC ID and applies the 16 MiB / 64 KiB block / 4 KiB
 * sector / 256 B page geometry.  Writes are only allowed when the
 * capacity byte confirms a 16 MiB part: programming a chip of another
 * size with this geometry would corrupt it silently, whereas reads with
 * the wrong geometry are merely wrong.  Enables the memory-mapped read
 * window used by the boot-time image scan.
 */
void spi_probe(void)
{
	unsigned int id;
	static char name_other[] = "SPI-NOR";

	g_spi_fault = 0;
	id = ComSrlCmd_RDID(4);
	id = ComSrlCmd_RDID(4);
	id = (id >> 8) & 0xFFFFFF; /* manufacturer, type, capacity */
	g_flash_jedec_id = id;

	spi_flash_info.chip_id = id;
	spi_flash_info.mfr_id = (id >> 16) & 0xff;
	spi_flash_info.dev_id = (id >> 8) & 0xff;
	spi_flash_info.capacity_id = id & 0xff;
	spi_flash_info.device_size = SIZEN_16M; /* 2^N bytes */
	spi_flash_info.chip_size = 1U << SIZEN_16M;
	spi_flash_info.block_size = SIZE_064K;
	spi_flash_info.sector_size = SIZE_004K;
	spi_flash_info.page_size = SIZE_256B;
	spi_flash_info.chip_name =
	    (spi_flash_info.mfr_id == 0xC8) ? "GD25Q128" : name_other;
	g_flash_chip_name = spi_flash_info.chip_name;
	g_flash_writable = !g_spi_fault &&
			   (spi_flash_info.capacity_id == SIZEN_16M);

	/* 84 MHz SPI clock target, byte order both ways, 31 idle clocks on CS */
	setFSCR(84, 1, 1, 31);
	spi_enable_mmap_read();
}

/**
 * flashread - Read data from SPI flash into RAM
 * @dst: destination RAM address
 * @src: source flash offset (relative to flash base)
 * @length: number of bytes to read
 *
 * Return: 1 on success
 */
int flashread(unsigned long dst, unsigned int src, unsigned long length)
{
	return mxic_cmd_read_s1(src, length, (unsigned char *)dst) == length;
}

/**
 * spi_flw_image - Write an image to flash and verify it
 * @flash_addr_offset: destination offset in flash
 * @image_addr: source data in RAM
 * @image_size: number of bytes to write
 *
 * Erases the target range (by 64 KiB block where aligned, by 4 KiB sector
 * elsewhere), programs it page by page and reads every unit back.
 * Refused when the probe did not identify a 16 MiB chip.
 *
 * Return: 1 on success, 0 on failure (nothing written, or a verify
 * mismatch — the range is then in an undefined state and must be
 * written again)
 */
int spi_flw_image(unsigned int flash_addr_offset, unsigned char *image_addr,
		  unsigned int image_size)
{
	if (!g_flash_writable) {
		NDEBUG("Flash write refused: JEDEC %06x is not a 16 MiB part\n",
		       g_flash_jedec_id);
		return 0;
	}
	if (image_size == 0)
		return 0;
	if (flash_addr_offset >= spi_flash_info.chip_size ||
	    image_size > spi_flash_info.chip_size - flash_addr_offset) {
		NDEBUG("Flash write refused: 0x%x+0x%x exceeds the chip\n",
		       flash_addr_offset, image_size);
		return 0;
	}
	rstSPIFlash();
	return flash_write_range(flash_addr_offset, image_addr, image_size);
}
