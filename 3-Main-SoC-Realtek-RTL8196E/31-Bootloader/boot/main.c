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
#include "boot_irq.h"
#include "main.h"
#include "checks.h"
#include "uart.h"
#include "ramtest_trace.h"

/* The only stack (head.S points sp at its top) and the malloc arena. */
unsigned char init_task_union[SYS_STACK_SIZE];
unsigned long kernelsp;
char dl_heap[_SYSTEM_HEAP_SIZE];

unsigned long glexra_clock = 200 * 1000 * 1000;

/* Set once the download-mode key has been seen during the image scan. */
static unsigned int gCHKKEY_HIT = 0;

/* Flash-mapped address of the header of the image being booted. */
static unsigned long return_addr;

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
 *   A page near the top of DRAM is well above the kernel image (loaded
 *   at phys 0x00500000) and above any plausible early-boot scratch use
 *   of low memory.  100% reliable in testing.
 *
 * Why not the very top page (0x01FFF000)?  It produced false HOLD
 * detections on cold boots.  The cause was never identified — the
 * loader itself never touches that page (stage-1 runs without a stack
 * and stage-2's stack sits inside its own BSS), so the writer is either
 * the kernel or the stock loader.  The page below it was found clean by
 * experiment and is what every consumer (DTS, boothold, this file)
 * agrees on; do not move it without re-running that experiment.
 *
 * KSEG1 is used (not KSEG0) so that both the read and the clear
 * bypass the cache and go directly to DRAM.  Without this, the
 * clear (write 0) stays in the write-back cache and is lost on
 * power cycle — causing a false boot-hold on every cold boot.
 *
 * Optional TFTP-server-IP hand-off: `boothold <ip>` writes a marker word
 * and the packed IPv4 just below the HOLD magic in the same page (see
 * 34-Userdata/boothold/src/boothold.c).  It is honoured only when HOLD
 * itself is valid — i.e. a deliberate warm reboot from a running Linux.
 * On a cold boot the page holds garbage; the marker will not match and
 * tftpd_entry() keeps the compiled default (192.168.1.6).
 */

extern unsigned long g_tftp_server_ip;
extern const char *g_flash_chip_name;
extern unsigned int g_flash_jedec_id;

static int check_image(IMG_HEADER_Tp pHeader);
static void doBooting(int flag, unsigned long addr, IMG_HEADER_Tp pheader);
static void showBoardInfo(void);
static void setClkInitConsole(void);
static void initHeap(void);
static void initInterrupt(void);

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

	setClkInitConsole();
	initHeap();
	initInterrupt();
	timer_init(glexra_clock);
	spi_probe();
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
	ret = check_image(&header);

	invalidate_iram();
	doBooting(ret, return_addr, &header);
}

/**
 * showBoardInfo - Print hardware identification banner
 *
 * The CPU frequency is the platform constant (400 MHz core, 200 MHz bus
 * driving UART and timer).  The former per-boot calibration loop only fed
 * this line and cost ~0.2 s on every boot.
 */
static void showBoardInfo(void)
{
	prom_printf("Realtek RTL8196E  CPU: 400MHz  RAM: " BOARD_DRAM_BANNER
		    "  Flash: %s (JEDEC %06x)\n",
		    g_flash_chip_name, g_flash_jedec_id);
	prom_printf("Bootloader: %s - %s - J. Nilo\n", B_VERSION, BOOT_CODE_TIME);
}

/**
 * check_system_image - Validate a firmware image at a flash address
 * @addr: flash-mapped address of the image header
 * @pHeader: output buffer for the parsed image header
 *
 * Reads the image header from flash, checks signature (cs/cr), checks
 * that the load window the header asks for is sane, copies the image
 * body to RAM at pHeader->startAddr, and verifies the 16-bit checksum.
 * Periodically polls for the user's ESC while summing.
 *
 * Return: 0 if not found, 1 if Linux image, 2 if Linux+rootfs image
 */
static int check_system_image(unsigned long addr, IMG_HEADER_Tp pHeader)
{
	int ret = 0;
	unsigned long i;
	unsigned short sum = 0, *word_ptr;
	unsigned char *img;
#ifdef RAMTEST_TRACE
	int t0, t1;
#endif

	if (gCHKKEY_HIT == 1)
		return 0;

	word_ptr = (unsigned short *)pHeader;
	for (i = 0; i < sizeof(IMG_HEADER_T); i += 2, word_ptr++)
		*word_ptr = rtl_inw(addr + i);

	if (!memcmp(pHeader->signature, FW_SIGNATURE, SIG_LEN))
		ret = 1;
	else if (!memcmp(pHeader->signature, FW_SIGNATURE_WITH_ROOT, SIG_LEN))
		ret = 2;
	if (!ret)
		return 0;

	/*
	 * The header comes straight from flash.  One flipped bit in
	 * startAddr would make the copy below land on this loader, on the
	 * exception vectors or on the reserved pages, and the board would
	 * hang before the ESC check ever ran.  A header that does not
	 * describe a plausible window is treated as "no image": the loader
	 * then falls into download mode, which is the designed recovery.
	 */
	if (!kernel_header_ok(pHeader->startAddr, pHeader->len,
			      KERNEL_PARTITION_SIZE, RAM_LOAD_FLOOR,
			      RAM_RESERVED_TOP, (unsigned long)_ftext,
			      (unsigned long)_end)) {
		prom_printf("image header at %X rejected: start=%X len=%X\n",
			    addr - FLASH_BASE, pHeader->startAddr,
			    pHeader->len);
		return 0;
	}

	img = (unsigned char *)pHeader->startAddr;
#ifdef RAMTEST_TRACE
	t0 = get_timer_jiffies();
#endif
	if (!flashread((unsigned long)img,
		       (unsigned int)(addr - FLASH_BASE + sizeof(IMG_HEADER_T)),
		       pHeader->len)) {
		prom_printf("image read at %X failed\n", addr - FLASH_BASE);
		return 0;
	}
#ifdef RAMTEST_TRACE
	t1 = get_timer_jiffies();
	dprintf("\n---RAMTEST kernel copy: %d bytes in %d ms\n",
		(int)pHeader->len, (t1 - t0) * 10);
#endif

	for (i = 0; i < pHeader->len; i += 2) {
		if ((i % CHKKEY_POLL_BYTES) == 0 && user_interrupt() == 1)
			return 0;
		sum += *(unsigned short *)(img + i);
	}
	if (sum)
		ret = 0;

	return ret;
}

/**
 * check_image - Scan flash for a valid firmware image
 * @pHeader: output buffer for the image header
 *
 * Tries the three fixed kernel slots, then every 64 KiB step of the
 * configured scan range.  The rootfs is not scanned: Linux locates it.
 *
 * Return: 0 if no image found, 1 if kernel found, 2 if kernel+rootfs
 */
static int check_image(IMG_HEADER_Tp pHeader)
{
	static const unsigned long slots[] = {CODE_IMAGE_OFFSET,
					      CODE_IMAGE_OFFSET2,
					      CODE_IMAGE_OFFSET3};
	unsigned long i, off;
	int ret = 0;

	for (i = 0; i < sizeof(slots) / sizeof(slots[0]) && !ret; i++) {
		return_addr = (unsigned long)FLASH_BASE + slots[i];
		ret = check_system_image(return_addr, pHeader);
	}

	off = CONFIG_LINUX_IMAGE_OFFSET_START;
	while (off <= CONFIG_LINUX_IMAGE_OFFSET_END && !ret) {
		if (off != CODE_IMAGE_OFFSET && off != CODE_IMAGE_OFFSET2 &&
		    off != CODE_IMAGE_OFFSET3) {
			return_addr = (unsigned long)FLASH_BASE + off;
			ret = check_system_image(return_addr, pHeader);
		}
		off += CONFIG_LINUX_IMAGE_OFFSET_STEP;
	}
	return ret;
}

/*
 * pollingDownModeKeyword - drain the UART FIFO looking for the key
 *
 * Examining only the first character would let one stray byte sit in
 * front of the user's key and hide it for the rest of the boot: the
 * stashed character is not consumed until the monitor runs, so every
 * later poll would return immediately.  A line transient at reset is
 * enough to cause that.  The loop always terminates — draining a byte
 * costs a couple of register reads, orders of magnitude less than the
 * time the next one takes to arrive on the wire.
 */
static int pollingDownModeKeyword(int key)
{
	int ch;

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
 *
 * Immediate poll, no wait: there is no timed window before or after the
 * image scan.  The key is sampled while the image is checksummed and once
 * more just before the jump.
 *
 * Return: 1 if ESC pressed, 0 otherwise
 */
int user_interrupt(void)
{
	return pollingDownModeKeyword(ESC);
}

/**
 * goToDownMode - Enter TFTP download and monitor console mode
 *
 * Re-arms the timer tick (doBooting masked every interrupt), brings up
 * the Ethernet switch, starts the TFTP server and enters the interactive
 * monitor.  Received frames are handled from the main loop: the console
 * reader calls eth_poll() while it waits for a character, so the network
 * is serviced between keystrokes rather than from the interrupt handler.
 */
void goToDownMode(void)
{
	timer_irq_enable();
	sti();

	if (eth_startup(0)) {
		prom_printf("---Ethernet recovery unavailable; serial monitor only\n");
		monitor();
		return;
	}

	dprintf("\n---Ethernet init Okay!\n");

	tftpd_entry();
	rt_init(); /* RAM-test build: breadcrumbs + watchdog armed */
	g_uart_idle = eth_poll;

	monitor();
}

/**
 * goToLocalStartMode - Hand control to the kernel image found in flash
 * @addr: flash-mapped address of the image header
 * @pheader: parsed image header
 *
 * Returns only if the user pressed ESC in the meantime.
 */
static void goToLocalStartMode(unsigned long addr, IMG_HEADER_Tp pheader)
    __attribute__((unused)); /* compiled out of the RAM-test build's flow */
static void goToLocalStartMode(unsigned long addr, IMG_HEADER_Tp pheader)
{
	unsigned short *word_ptr;
	void (*jump)(void);
	unsigned long i;

	word_ptr = (unsigned short *)pheader;
	for (i = 0; i < sizeof(IMG_HEADER_T); i += 2, word_ptr++)
		*word_ptr = rtl_inw(addr + i);

	if (user_interrupt()) /* user escaped while the image was copied */
		return;

	REG32(GIMR_REG) = 0; /* mask all interrupts */
	jump = (void *)(pheader->startAddr);
	cli();
	/*
	 * Quiesce the Ethernet switch before handing off to the kernel: a
	 * live switch can DMA inbound frames into DRAM during early boot
	 * and corrupt it (the intermittent post-flash boot loop).  The same
	 * sequence guards the `J` command and the post-flash reboot.
	 */
	swCore_quiesce();
	flush_cache();
	jump();
}

/**
 * setClkInitConsole - Configure memory controller and UART console
 *
 * Enables the MCR prefetch bit and initializes the serial console
 * at the configured baud rate.
 */
static void setClkInitConsole(void)
{
	REG32(MCR_REG) = REG32(MCR_REG) | (1 << 27); /* new prefetch */
	console_init(glexra_clock);
}

/**
 * initHeap - Initialize the bootloader heap allocator
 *
 * Sets up the malloc/free arena using the dl_heap BSS region.
 */
static void initHeap(void)
{
	unsigned int heap_addr = ((unsigned int)dl_heap & (~7)) + 8;
	unsigned int heap_end = heap_addr + sizeof(dl_heap) - 8;

	i_alloc((void *)heap_addr, (void *)heap_end);
	flush_cache();
}

/**
 * initInterrupt - Set up the interrupt subsystem
 *
 * Masks all hardware interrupts, configures CP0 exception vectors,
 * installs the IRQ dispatcher, and enables interrupts.
 */
static void initInterrupt(void)
{
	rtl_outl(GIMR0, 0x00); /* mask all interrupts */
	setup_arch();	       /* clear BEV, enable the IRQ lines */
	exception_init();      /* copy the dispatcher to 0x80000080 */
	init_IRQ();	       /* route exception 0 to IRQ_finder */
	sti();
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
static void doBooting(int flag, unsigned long addr, IMG_HEADER_Tp pheader)
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
	int key_at_decision = user_interrupt();

	dprintf("\n---RAMTEST key check: during scan=%d, at decision=%d\n",
		key_during_scan, key_at_decision);
#endif
	if (flag) {
#ifdef RAMTEST_TRACE
		dprintf("\n---RAMTEST mode: skipping kernel boot\n");
#else
		if (!user_interrupt())
			goToLocalStartMode(addr, pheader);
		/* goToLocalStartMode() returns only when the user escaped
		 * during the copy: fall into download mode. */
#endif
		dprintf("\n---Escape booting by user\n");
	}
	REG32(GIMR_REG) = 0x0;
	goToDownMode();
}
