// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * main.c - Boot logic: image validation, kernel loading, boot flow
 *
 * RTL8196E stage-2 bootloader
 *
 * Copyright (c) 2009-2020 Realtek Semiconductor Corp.
 * Copyright (c) 2024-2026 J. Nilo
 */

#include "board.h"
#include "boot_common.h"
#include "boot_soc.h"
#include "spi_common.h"
#include "cache.h"
#include "main.h"
#include "uart.h"

unsigned char *p_kernel_img;

unsigned long glexra_clock = 200 * 1000 * 1000;

unsigned int gCHKKEY_HIT = 0;

/*
 * Boot-hold: Linux can request the bootloader to stop at the <RealTek>
 * prompt by writing a magic word to a fixed RAM address before
 * triggering a watchdog reset.  DRAM contents survive the reset on the
 * RTL8196E (verified experimentally).  A full power cycle clears DRAM
 * and restores normal boot.
 * The flag is one-shot: the bootloader clears it before entering
 * download mode.
 *
 * The flag words live at the top of a 4 KB page placed at DRAM top
 * minus 0x2000, derived from the board's BOARD_DRAM_TOP_KSEG1 (on the
 * Lidl board: page 0x01FFE000–0x01FFEFFF, HOLD at KSEG1 0xA1FFEFFC).
 * The same page is declared as reserved-memory with no-map in the
 * board's kernel DTS, where the `boothold` tool and the watchdog panic
 * record resolve it at runtime — the bootloader is the only
 * compile-time consumer, so a board with a different DRAM size only
 * needs its board.h and its DTS to agree.  The kernel page allocator
 * skips this page — no KSEG0/KSEG1 coherency conflict.
 *
 * Why HIGH in DRAM and not at 0x003FFFFC like in v2.x?
 *
 *   On Linux 5.10 (v2.x), HOLD at 0x003FFFFC was reliable.  On Linux
 *   6.18 (v3.0.0+), the bootloader started reading garbage / zeros
 *   from that address ~13-27% of the time — symptoms consistent with
 *   the kernel scribbling low DRAM during early init or shutdown,
 *   before the reserved-memory no-map declaration takes effect.
 *   Moving HOLD just below the btcode stack (which lives at the very
 *   top of DRAM) puts it well above the kernel image (loaded at
 *   phys 0x00500000) and above any plausible early-boot scratch use
 *   of low memory.  100% reliable in testing.
 *
 * KSEG1 is used (not KSEG0) so that both the read and the clear
 * bypass the cache and go directly to DRAM.  Without this, the
 * clear (write 0) stays in the write-back cache and is lost on
 * power cycle — causing a false boot-hold on every cold boot.
 *
 * Top of DRAM (0x81FFFFFC) is NOT safe: btcode stack starts there.
 * 0x01FFEFFC is one 4KB page below the stack, well clear of stack
 * usage on this minimal bootloader.
 */
#define BOOTHOLD_MAGIC  0x484F4C44  /* "HOLD" */
#define BOOTHOLD_PAGE   (BOARD_DRAM_TOP_KSEG1 - 0x2000)
#define BOOTHOLD_RAM    ((volatile unsigned long *)(BOOTHOLD_PAGE + 0xFFC))

/*
 * Optional TFTP-server-IP handoff: `boothold <ip>` writes a marker word and
 * the packed IPv4 just below the HOLD magic in the same reserved DRAM page
 * (see 34-Userdata/boothold/src/boothold.c).  It is honoured only when HOLD
 * itself is valid — i.e. a deliberate warm reboot from a running Linux.  On a
 * cold boot the page holds garbage; the marker will not match and tftpd_entry()
 * keeps the compiled default (192.168.1.6).
 */
#define BOOTHOLD_IP_MAGIC_RAM ((volatile unsigned long *)(BOOTHOLD_PAGE + 0xFF8))
#define BOOTHOLD_IP_RAM       ((volatile unsigned long *)(BOOTHOLD_PAGE + 0xFF4))
#define BOOTHOLD_IP_MAGIC     0x49505634  /* "IPV4" */

extern unsigned long g_tftp_server_ip;

void goToDownMode(void);

/**
 * start_kernel - Main bootloader entry point (called from init_arch)
 *
 * Initializes console, heap, interrupts, and SPI flash, then
 * searches for a valid firmware image.  If found, boots the
 * kernel; otherwise enters download mode.
 */
void start_kernel(void)
{
	int ret;

	IMG_HEADER_T header;
	SETTING_HEADER_T setting_header;
	//-------------------------------------------------------
	setClkInitConsole();

	initHeap();

	initInterrupt();

	initFlash();

	showBoardInfo();

	if (BOOTHOLD_RAM[0] == BOOTHOLD_MAGIC) {
		/* Apply the optional server-IP override before clearing. */
		if (BOOTHOLD_IP_MAGIC_RAM[0] == BOOTHOLD_IP_MAGIC)
			g_tftp_server_ip = BOOTHOLD_IP_RAM[0];
		/* One-shot: wipe the whole handoff so a later download-mode
		 * entry (e.g. after a failed flash) cannot reuse stale data. */
		BOOTHOLD_RAM[0] = 0;
		BOOTHOLD_IP_MAGIC_RAM[0] = 0;
		BOOTHOLD_IP_RAM[0] = 0;
		prom_printf("---Boot hold requested\n");
		goToDownMode();
		return;
	}

	return_addr = 0;
	ret = check_image(&header, &setting_header);

	invalidate_iram();
	doBooting(ret, return_addr, &header);
}

/**
 * showBoardInfo - Print hardware identification banner
 *
 * Displays CPU speed, RAM size, and flash chip name on the console.
 */
void showBoardInfo(void)
{
	int cpu_speed;

	cpu_speed = check_cpu_speed();

	prom_printf("Realtek RTL8196E  CPU: %dMHz  RAM: " BOARD_DRAM_BANNER
		    "  Flash: %s\n",
		    cpu_speed, g_flash_chip_name);
	prom_printf("Bootloader: %s - %s - J. Nilo\n", B_VERSION, BOOT_CODE_TIME);
}

/**
 * check_system_image - Validate a firmware image at a flash address
 * @addr: flash-mapped address of the image header
 * @pHeader: output buffer for the parsed image header
 * @setting_header: output buffer for settings header
 *
 * Reads the image header from flash, checks signature (cs/cr),
 * copies the image body to RAM at pHeader->startAddr, and verifies
 * the 16-bit checksum.  Periodically polls for user ESC interrupt.
 *
 * Return: 0 if not found, 1 if Linux image, 2 if Linux+rootfs image
 */
int check_system_image(unsigned long addr, IMG_HEADER_Tp pHeader,
		       SETTING_HEADER_Tp setting_header)
{
	// Read header, heck signature and checksum
	int i, ret = 0;
	unsigned short sum = 0, *word_ptr;
	char image_sig[4] = {0};
	char image_sig_root[4] = {0};
	if (gCHKKEY_HIT == 1)
		return 0;
	/*check firmware image.*/
	word_ptr = (unsigned short *)pHeader;
	for (i = 0; i < sizeof(IMG_HEADER_T); i += 2, word_ptr++)
		*word_ptr = rtl_inw(addr + i);

	memcpy(image_sig, FW_SIGNATURE, SIG_LEN);
	memcpy(image_sig_root, FW_SIGNATURE_WITH_ROOT, SIG_LEN);

	if (!memcmp(pHeader->signature, image_sig, SIG_LEN))
		ret = 1;
	else if (!memcmp(pHeader->signature, image_sig_root, SIG_LEN))
		ret = 2;
	if (ret) {

		p_kernel_img = (unsigned char *)pHeader->startAddr;
		flashread(
		    (unsigned long)p_kernel_img,
		    (unsigned int)(addr - FLASH_BASE + sizeof(IMG_HEADER_T)),
		    pHeader->len);

		for (i = 0; i < pHeader->len; i += 2) {
			if ((i % CHKKEY_POLL_BYTES) == 0 &&
			    user_interrupt(0) == 1)
				return 0;
			sum += *(unsigned short *)(p_kernel_img + i);
		}
		if (sum) {
			ret = 0;
		}
	}

	return (ret);
}

/**
 * check_rootfs_image - Validate a SquashFS root filesystem image
 * @addr: flash-mapped address of the rootfs
 *
 * Checks for sqsh/hsqs signature, reads the filesystem length from
 * the superblock, and verifies the 16-bit checksum.
 *
 * Return: 1 if valid, 0 otherwise
 */
int check_rootfs_image(unsigned long addr)
{
	// Read header, heck signature and checksum
	int i;
	unsigned short sum = 0, *word_ptr;
	unsigned long length = 0;
	unsigned char tmpbuf[16];
#define SIZE_OF_SQFS_SUPER_BLOCK 640
#define SIZE_OF_CHECKSUM 2
#define OFFSET_OF_LEN 2

	if (gCHKKEY_HIT == 1)
		return 0;

	word_ptr = (unsigned short *)tmpbuf;
	for (i = 0; i < 16; i += 2, word_ptr++)
		*word_ptr = rtl_inw(addr + i);

	if (memcmp(tmpbuf, SQSH_SIGNATURE, SIG_LEN) &&
	    memcmp(tmpbuf, SQSH_SIGNATURE_LE, SIG_LEN)) {
		prom_printf("no rootfs signature at %X!\n", addr - FLASH_BASE);
		return 0;
	}

	length = *(((unsigned long *)tmpbuf) + OFFSET_OF_LEN) +
		 SIZE_OF_SQFS_SUPER_BLOCK + SIZE_OF_CHECKSUM;

	for (i = 0; i < length; i += 2) {
		if ((i % CHKKEY_POLL_BYTES) == 0 && user_interrupt(0) == 1)
			return 0;
		sum += rtl_inw(addr + i);
	}

	if (sum) {
		prom_printf("rootfs checksum error at %X!\n",
			    addr - FLASH_BASE);
		return 0;
	}
	return 1;
}

static int check_image_header(IMG_HEADER_Tp pHeader,
			      SETTING_HEADER_Tp psetting_header,
			      unsigned long bank_offset)
{
	int i, ret = 0;
	// flash mapping
	return_addr =
	    (unsigned long)FLASH_BASE + CODE_IMAGE_OFFSET + bank_offset;
	/* quiet: suppress verbose header scan output */
	ret = check_system_image((unsigned long)FLASH_BASE + CODE_IMAGE_OFFSET +
				     bank_offset,
				 pHeader, psetting_header);

	if (ret == 0) {
		return_addr = (unsigned long)FLASH_BASE + CODE_IMAGE_OFFSET2 +
			      bank_offset;
		ret = check_system_image((unsigned long)FLASH_BASE +
					     CODE_IMAGE_OFFSET2 + bank_offset,
					 pHeader, psetting_header);
	}
	if (ret == 0) {
		return_addr = (unsigned long)FLASH_BASE + CODE_IMAGE_OFFSET3 +
			      bank_offset;
		ret = check_system_image((unsigned long)FLASH_BASE +
					     CODE_IMAGE_OFFSET3 + bank_offset,
					 pHeader, psetting_header);
	}

	i = CONFIG_LINUX_IMAGE_OFFSET_START;
	while (i <= CONFIG_LINUX_IMAGE_OFFSET_END && (0 == ret)) {
		return_addr = (unsigned long)FLASH_BASE + i + bank_offset;
		if (CODE_IMAGE_OFFSET == i || CODE_IMAGE_OFFSET2 == i ||
		    CODE_IMAGE_OFFSET3 == i) {
			i += CONFIG_LINUX_IMAGE_OFFSET_STEP;
			continue;
		}
		ret = check_system_image((unsigned long)FLASH_BASE + i +
					     bank_offset,
					 pHeader, psetting_header);
		i += CONFIG_LINUX_IMAGE_OFFSET_STEP;
	}

#if !SKIP_ROOTFS_SCAN
	if (ret == 2) {
		ret = check_rootfs_image((unsigned long)FLASH_BASE +
					 ROOT_FS_OFFSET + bank_offset);
		if (ret == 0)
			ret = check_rootfs_image(
			    (unsigned long)FLASH_BASE + ROOT_FS_OFFSET +
			    ROOT_FS_OFFSET_OP1 + bank_offset);
		if (ret == 0)
			ret = check_rootfs_image(
			    (unsigned long)FLASH_BASE + ROOT_FS_OFFSET +
			    ROOT_FS_OFFSET_OP1 + ROOT_FS_OFFSET_OP2 +
			    bank_offset);

		i = CONFIG_ROOT_IMAGE_OFFSET_START;
		while ((i <= CONFIG_ROOT_IMAGE_OFFSET_END) && (0 == ret)) {
			if (ROOT_FS_OFFSET == i ||
			    (ROOT_FS_OFFSET + ROOT_FS_OFFSET_OP1) == i ||
			    (ROOT_FS_OFFSET + ROOT_FS_OFFSET_OP1 +
			     ROOT_FS_OFFSET_OP2) == i) {
				i += CONFIG_ROOT_IMAGE_OFFSET_STEP;
				continue;
			}
			ret = check_rootfs_image((unsigned long)FLASH_BASE + i +
						 bank_offset);
			i += CONFIG_ROOT_IMAGE_OFFSET_STEP;
		}
	}
#endif
	return ret;
}

/**
 * check_image - Scan flash for a valid firmware image
 * @pHeader: output buffer for the image header
 * @psetting_header: output buffer for settings header
 *
 * Searches known flash offsets and a configurable scan range for
 * a valid Linux kernel image and optional root filesystem.
 *
 * Return: 0 if no image found, 1 if kernel found, 2 if kernel+rootfs
 */
int check_image(IMG_HEADER_Tp pHeader, SETTING_HEADER_Tp psetting_header)
{
	int ret = 0;
	// only one bank

	ret = check_image_header(pHeader, psetting_header, 0);

	return ret;
}

// monitor user interrupt
int pollingDownModeKeyword(int key)
{
	int ch;

	/*
	 * Drain whatever the FIFO holds, looking for the key.  Examining only
	 * the first character would let one stray byte sit in front of the
	 * user's key and hide it for the rest of the boot: the stashed
	 * character is not consumed until the monitor runs, so every later
	 * poll would return immediately.  A line transient at reset is enough
	 * to cause that.  The loop always terminates — draining a byte costs
	 * a couple of register reads, orders of magnitude less than the time
	 * the next one takes to arrive on the wire.
	 */
	while (uart_data_ready()) {
		ch = uart_getc_nowait();
		if (ch == key) {
			gCHKKEY_HIT = 1;
			return 1;
		}
		/* Keep the first non-matching character so serial_inc() can
		 * still return it; later ones are noise ahead of the prompt. */
		if (g_uart_peek < 0)
			g_uart_peek = ch;
	}
	return 0;
}

/**
 * user_interrupt - Check if the user pressed ESC to abort booting
 * @time: timeout (unused, immediate poll)
 *
 * Return: 1 if ESC pressed, 0 otherwise
 */
int user_interrupt(unsigned long time)
{
	return pollingDownModeKeyword(ESC);
}

/**
 * goToDownMode - Enter TFTP download and monitor console mode
 *
 * Initializes the Ethernet interface, starts the TFTP server,
 * then enters the interactive monitor command loop.
 */
void goToDownMode()
{

	eth_startup(0);

	dprintf("\n---Ethernet init Okay!\n");
	sti();

	tftpd_entry();

	monitor();
	return;
}

/* swCore.c — full switch-core reset (active_swcore toggle), used to flush any
 * in-flight CPU-port DMA before the kernel handoff. */
extern void FullAndSemiReset(void);

void goToLocalStartMode(unsigned long addr, IMG_HEADER_Tp pheader)
{
	unsigned short *word_ptr;
	void (*jump)(void);
	int i;

	word_ptr = (unsigned short *)pheader;
	for (i = 0; i < sizeof(IMG_HEADER_T); i += 2, word_ptr++)
		*word_ptr = rtl_inw(addr + i);

	if (!user_interrupt(0)) // See if user escape during copy image
	{
		outl(0, GIMR0); // mask all interrupt

		jump = (void *)(pheader->startAddr);

		cli();
		/*
		 * Quiesce the Ethernet switch before handing off to the kernel.
		 * The kernel re-inits the MAC, but until it does, a live switch can
		 * DMA inbound frames into DRAM during early boot and corrupt it —
		 * the intermittent post-flash boot loop. Turning the PHY interface
		 * off stops new ingress but NOT an already-armed DMA, so on the
		 * auto-boot path (taken on every boot) first stop the CPU-port DMA
		 * engine and hard-reset the switch core (the same active_swcore reset
		 * swCore_init() runs on every boot, which aborts any in-flight
		 * transfer), THEN hold the PHY off (the reset re-defaults PCRP, so
		 * PHY-off must come after it). Counterpart of the same quiesce in the
		 * `J` command (monitor.c) and autoreboot() (tftpd.c).
		 */
		WRITE_MEM32(CPUICR, 0); /* stop CPU-port RX/TX DMA */
		FullAndSemiReset();	/* hard-reset switch core — flush in-flight DMA */
		WRITE_MEM32(PCRP0, (READ_MEM32(PCRP0) & (~EnablePHYIf)));
		WRITE_MEM32(PCRP1, (READ_MEM32(PCRP1) & (~EnablePHYIf)));
		WRITE_MEM32(PCRP2, (READ_MEM32(PCRP2) & (~EnablePHYIf)));
		WRITE_MEM32(PCRP3, (READ_MEM32(PCRP3) & (~EnablePHYIf)));
		WRITE_MEM32(PCRP4, (READ_MEM32(PCRP4) & (~EnablePHYIf)));
		flush_cache();
		jump(); // jump to start
		return;
	}
	return;
}

/**
 * setClkInitConsole - Configure memory controller and UART console
 *
 * Enables the MCR prefetch bit and initializes the serial console
 * at the configured baud rate.
 */
void setClkInitConsole(void)
{
	REG32(MCR_REG) = REG32(MCR_REG) | (1 << 27); // new prefetch

	console_init(glexra_clock);
}

/**
 * initHeap - Initialize the bootloader heap allocator
 *
 * Sets up the malloc/free arena using the dl_heap BSS region.
 */
void initHeap(void)
{
	/* Initialize malloc mechanism */
	unsigned int heap_addr = ((unsigned int)dl_heap & (~7)) + 8;
	unsigned int heap_end = heap_addr + sizeof(dl_heap) - 8;
	i_alloc((void *)heap_addr, (void *)heap_end);
	cli();
	flush_cache(); // david
}

/**
 * initInterrupt - Set up the interrupt subsystem
 *
 * Masks all hardware interrupts, configures CP0 exception vectors,
 * installs the IRQ dispatcher, and enables interrupts.
 */
void initInterrupt(void)
{
	rtl_outl(GIMR0, 0x00); /*mask all interrupt*/
	setup_arch();	       /*setup the BEV0,and IRQ */
	exception_init();      /*Copy handler to 0x80000080*/
	init_IRQ();	       /*Allocate IRQfinder to Exception 0*/
	sti();
}

/**
 * initFlash - Probe and initialize the SPI flash
 */
void initFlash(void)
{
	spi_probe(); // JSW : SPI flash init
}

/**
 * doBooting - Execute boot decision based on image check result
 * @flag: result from check_image (0 = no image found)
 * @addr: flash address of the validated image
 * @pheader: parsed image header
 *
 * If a valid image was found, checks for user interrupt (ESC),
 * then either boots the kernel or enters download mode.
 */
void doBooting(int flag, unsigned long addr, IMG_HEADER_Tp pheader)
{
#ifdef RAMTEST_TRACE
	/*
	 * RAM-test build: the kernel jump below is compiled out, so both
	 * outcomes end in download mode and the key decision would otherwise
	 * be invisible.  Report it instead of acting on it.  The two reads are
	 * sequenced deliberately: user_interrupt() sets gCHKKEY_HIT itself, so
	 * evaluating both inside one call would not say which one fired.
	 */
	int key_during_scan = gCHKKEY_HIT;
	int key_at_decision = user_interrupt(0);

	dprintf("\n---RAMTEST key check: during scan=%d, at decision=%d\n",
		key_during_scan, key_at_decision);
#endif
	if (flag) {
#ifdef RAMTEST_TRACE
		dprintf("\n---RAMTEST mode: skipping kernel boot\n");
#else
		switch (user_interrupt(WAIT_TIME_USER_INTERRUPT)) {
		case LOCALSTART_MODE:
		default:
			goToLocalStartMode(addr, pheader);
		case DOWN_MODE:
#endif
			dprintf("\n---Escape booting by user\n");
			REG32(GIMR_REG) = 0x0;

			goToDownMode();
#ifndef RAMTEST_TRACE
			break;
		} /*switch case */
#endif
	} /*if image correct*/
	else {
		REG32(GIMR_REG) = 0x0;
		goToDownMode();
	}
	return;
}
