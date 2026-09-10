#include "boot_common.h"
#include "boot_soc.h"
#include "rtk.h"
#include "flash_layout.h"
#include "ver.h"
#include "etherboot.h"
#include "cache.h"

#ifndef BOOT_CODE_TIME
#define BOOT_CODE_TIME "unknown"
#endif

//------------------------------------------------------------------------------------------
//
#define __KERNEL_SYSCALLS__

#define SYS_STACK_SIZE (4096 * 2)
unsigned char init_task_union[SYS_STACK_SIZE];

/* Setting image header */
typedef struct _setting_header_ {
	unsigned char Tag[2];
	unsigned char Version[2];
	unsigned short len;
} SETTING_HEADER_T, *SETTING_HEADER_Tp;

//------------------------------------------------------------------------------------------
/*
 * Poll the console for the download-mode key once per this many bytes of
 * image scanned.  A power of two, so the test compiles to a mask.
 */
#define CHKKEY_POLL_BYTES 0x10000 /* 64 KiB */
unsigned long return_addr;
#define WAIT_TIME_USER_INTERRUPT (3 * CPU_CLOCK)

#define LOCALSTART_MODE 0
#define DOWN_MODE 1

/*Cyrus Tsai*/
unsigned long kernelsp;
#define _SYSTEM_HEAP_SIZE 1024 * 64 // wei add
char dl_heap[_SYSTEM_HEAP_SIZE];    // wei add

// check
#define HS_IMAGE_OFFSET (24 * 1024) // 0x6000
#define DS_IMAGE_OFFSET (25 * 1024) // 0x6400
#define CS_IMAGE_OFFSET (32 * 1024) // 0x8000

/*in irq.c*/
extern void __init exception_init(void);
extern void init_IRQ(void);
extern void eth_startup(int etherport);

/*in tftpd.c*/
extern void tftpd_entry(void);

/*in monitor.c*/
extern int check_cpu_speed(void);
extern volatile int get_timer_jiffies(void);
extern int SettingCPUClk(int clk_sel, int clk_div, int sync_oc);

/*in flash.c*/
extern void spi_probe();

/*in main.c (was utility.c)*/
extern unsigned long glexra_clock;

int user_interrupt(unsigned long time);

int check_system_image(unsigned long addr, IMG_HEADER_Tp pHeader,
		       SETTING_HEADER_Tp setting_header);
int check_rootfs_image(unsigned long addr);

void showBoardInfo(void);
void setClkInitConsole(void);
void initHeap(void);
void initInterrupt(void);
void initFlash(void);
int check_image(IMG_HEADER_Tp pHeader, SETTING_HEADER_Tp psetting_header);
void doBooting(int flag, unsigned long addr, IMG_HEADER_Tp pheader);
int flashread(unsigned long dst, unsigned int src, unsigned long length);
void eth_startup(int etherport);
void tftpd_entry(void);
void monitor(void);
void i_alloc(void *_heapstart, void *_heapend);
void setup_arch(void);
void exception_init(void);
void init_IRQ(void);
void spi_probe(void);

#define SYS_BASE 0xb8000000
#define SYS_HW_STRAP (SYS_BASE + 0x08)
