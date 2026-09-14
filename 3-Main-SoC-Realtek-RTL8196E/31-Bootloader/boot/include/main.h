/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * main.h - Boot flow constants and the cross-file API of main.c
 */
#ifndef _MAIN_H_
#define _MAIN_H_

#include "board.h"
#include "boot_asm.h"
#include "boot_common.h"
#include "spi_flash.h"
#include "ver.h"

#define ESC '\033' /* the download-mode key */

/* --- Realtek image header (cvimg) and the signatures the loader accepts -- */

/* Firmware signatures (override at build time if needed) */
#ifndef FW_SIGNATURE
#define FW_SIGNATURE ((char *)"cs6c")		/* kernel */
#endif
#ifndef FW_SIGNATURE_WITH_ROOT
#define FW_SIGNATURE_WITH_ROOT ((char *)"cr6c") /* kernel + rootfs */
#endif
#ifndef ROOT_SIGNATURE
#define ROOT_SIGNATURE ((char *)"r6cr")
#endif

#define SQSH_SIGNATURE ((char *)"sqsh")

#define BOOT_SIGNATURE ((char *)"boot")
#define ALL1_SIGNATURE ((char *)"ALL1")
#define ALL2_SIGNATURE ((char *)"ALL2")

#define SIG_LEN 4

/* Firmware image header, 16 bytes, produced by cvimg */
typedef struct _header_ {
	unsigned char signature[SIG_LEN];
	unsigned long startAddr;
	unsigned long burnAddr;
	unsigned long len;
} IMG_HEADER_T, *IMG_HEADER_Tp;

typedef struct _signature__ {
	unsigned char *signature;
	unsigned char *comment;
	int sig_len;
	int skip;
	int maxSize;
	int reboot;
} SIGN_T;

/* --- RAM map (KSEG0) ------------------------------------------------------ */

/* KSEG0 (cached) address one past the last DRAM byte, from the board's KSEG1 value. */
#define BOARD_DRAM_TOP_KSEG0 (BOARD_DRAM_TOP_KSEG1 - 0x20000000)

/*
 * Two 4 KiB pages at the top of DRAM are never written by the loader:
 * the boothold hand-off page (top - 0x2000) and the kernel watchdog crash
 * record (top - 0x3000, reserved by the DTS).  Uploads and kernel copies
 * must stay below this line.
 */
#define RAM_RESERVED_TOP (BOARD_DRAM_TOP_KSEG0 - 0x3000)

/* First KSEG0 page holds the exception vectors (0x80000080); never a load target. */
#define RAM_LOAD_FLOOR 0x80001000

/* --- Boot-hold hand-off page (see the comment block in main.c) ------------ */

#define BOOTHOLD_MAGIC 0x484F4C44 /* "HOLD" */
#define BOOTHOLD_PAGE (BOARD_DRAM_TOP_KSEG1 - 0x2000)
#define BOOTHOLD_RAM ((volatile unsigned long *)(BOOTHOLD_PAGE + 0xFFC))
#define BOOTHOLD_IP_MAGIC_RAM ((volatile unsigned long *)(BOOTHOLD_PAGE + 0xFF8))
#define BOOTHOLD_IP_RAM ((volatile unsigned long *)(BOOTHOLD_PAGE + 0xFF4))
#define BOOTHOLD_IP_MAGIC 0x49505634 /* "IPV4" */

/* --- Stack and heap (defined in main.c, referenced by head.S) ------------- */

/* SYS_STACK_SIZE: boot_asm.h (head.S needs it too) */
extern unsigned char init_task_union[SYS_STACK_SIZE];
extern unsigned long kernelsp;

#define _SYSTEM_HEAP_SIZE (1024 * 64)
extern char dl_heap[_SYSTEM_HEAP_SIZE];

/*
 * Poll the console for the download-mode key once per this many bytes of
 * image scanned.  A power of two, so the test compiles to a mask.
 */
#define CHKKEY_POLL_BYTES 0x10000 /* 64 KiB */

/* Linker symbols bounding the running stage-2 image. */
extern char _ftext[];
extern char _end[];

/* --- main.c --------------------------------------------------------------- */

void start_kernel(void);
int user_interrupt(void);
void goToDownMode(void);

/*
 * fatal - report an unrecoverable condition and reset the board
 * @why: short message printed on the console
 *
 * Prints, waits a few seconds so the message can be read, clears the
 * boothold hand-off (so the reset does not loop back into download mode
 * with stale data) and triggers the hardware watchdog.  Never returns.
 */
void fatal(const char *why) __attribute__((noreturn));

/* --- other compilation units used by main.c ------------------------------- */

/* arch.c */
void flush_cache(void);
void invalidate_iram(void);

/* irq.c */
void exception_init(void);
void init_IRQ(void);
void setup_arch(void);

/* timer.c */
void timer_init(unsigned long lexra_clock);
void timer_irq_enable(void);
int get_timer_jiffies(void);
void delay_ms(unsigned int time_ms);

/* calloc.c */
void i_alloc(void *_heapstart, void *_heapend);

/* monitor.c */
void monitor(void);

/* flash.c */
void spi_probe(void);
int flashread(unsigned long dst, unsigned int src, unsigned long length);

/* net/eth.c, net/tftpd.c */
int eth_startup(int etherport);
void eth_poll(void);
void tftpd_entry(void);

/* swCore.c */
void swCore_quiesce(void);

#endif /* _MAIN_H_ */
