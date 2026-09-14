// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * monitor.c - Debug console commands
 *
 * RTL8196E stage-2 bootloader
 *
 * Copyright (c) 2009-2020 Realtek Semiconductor Corp.
 * Copyright (c) 2024-2026 J. Nilo
 */

#include "boot_common.h"
#include "boot_soc.h"
#include "boot_irq.h"
#include "monitor.h"
#include "boot_net.h"
#include "spi_flash.h"
#include "uart.h"
#include "main.h"
#include "checks.h"
#include <rtl_types.h>
#include "swcore.h"

#define MAIN_PROMPT "<RealTek>"

static int require_args(int argc, int min, const char *usage)
{
	if (argc < min) {
		if (usage && *usage)
			printf("Usage: %s\n", usage);
		else
			printf("Usage: <command> <args>\n");
		return 0;
	}
	return 1;
}

static int parse_hex_arg(const char *arg, unsigned long *out,
			 const char *label)
{
	const char *p = arg;

	if (!p || !*p) {
		printf("Invalid hex value.\n");
		return 0;
	}
	if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
		p += 2;
	if (!Hex2Val((char *)p, out)) {
		if (label && *label)
			printf("Invalid hex %s.\n", label);
		else
			printf("Invalid hex value.\n");
		return 0;
	}
	return 1;
}

static int YesOrNo(void);
static int CmdHelp(int argc, char *argv[]);
static int CmdDumpWord(int argc, char *argv[]);
static int CmdDumpByte(int argc, char *argv[]);
static int CmdWriteWord(int argc, char *argv[]);
static int CmdWriteByte(int argc, char *argv[]);
static int CmdCmp(int argc, char *argv[]);
static int CmdIp(int argc, char *argv[]);
static int CmdAuto(int argc, char *argv[]);
static int CmdLoad(int argc, char *argv[]);
static int CmdCfn(int argc, char *argv[]);
static int CmdSFlw(int argc, char *argv[]);
static int CmdFlr(int argc, char *argv[]);
static int TestCmd_MDIOR(int argc, char *argv[]);
static int TestCmd_MDIOW(int argc, char *argv[]);
static int CmdPHYregR(int argc, char *argv[]);
static int CmdPHYregW(int argc, char *argv[]);

static const COMMAND_TABLE MainCmdTable[] = {
    {"HELP", 0, CmdHelp, "HELP: Print this help message"},
    {"?", 0, CmdHelp,
     "HELP (?)				    : Print this help message"},
    {"DB", 2, CmdDumpByte, "DB <Address> <Len>"},
    {"DW", 2, CmdDumpWord, "DW <Address> <Len>"},
    {"EB", 2, CmdWriteByte, "EB <Address> <Value1> <Value2>..."},
    {"EW", 2, CmdWriteWord, "EW <Address> <Value1> <Value2>..."},
    {"CMP", 3, CmdCmp, "CMP: CMP <dst><src><length>"},
    {"IPCONFIG", 2, CmdIp, "IPCONFIG:<TargetAddress>"},
    {"AUTOBURN", 1, CmdAuto, "AUTOBURN: 0/1"},
    {"LOADADDR", 1, CmdLoad, "LOADADDR: <Load Address>"},
    {"J", 1, CmdCfn, "J: Jump to <TargetAddress>"},
    {"FLR", 3, CmdFlr, "FLR: FLR <dst><src><length>"},
    {"FLW", 3, CmdSFlw,
     "FLW <dst_ROM_offset> <src_RAM_addr> <length_Byte>: Write "
     "offset-data to SPI from RAM"},
    {"MDIOR", 0, TestCmd_MDIOR, "MDIOR:  MDIOR <phyid> <reg>"},
    {"MDIOW", 0, TestCmd_MDIOW, "MDIOW:  MDIOW <phyid> <reg> <data>"},
    {"PHYR", 2, CmdPHYregR, "PHYR: PHYR <PHYID><reg>"},
    {"PHYW", 3, CmdPHYregW, "PHYW: PHYW <PHYID><reg><data>"},
};

#define NUM_COMMANDS (sizeof(MainCmdTable) / sizeof(COMMAND_TABLE))

/**
 * monitor - Interactive command-line monitor loop
 *
 * Drains stale UART input, then loops: prints the prompt, reads a
 * command line, parses it, and dispatches to the matching handler
 * in MainCmdTable.  The network is serviced while GetLine() waits.
 */
void monitor(void)
{
	char buffer[MAX_MONITOR_BUFFER + 1];
	int argc;
	unsigned int i;
	char **argv;

	/*
	 * Drain stale bytes from the UART RX FIFO before entering the
	 * command loop.  When the user holds ESC to enter download mode,
	 * keyboard repeat fills the FIFO with 0x1b bytes that would
	 * otherwise be consumed as input by GetLine().  GetLine() also
	 * ignores ESC characters for late arrivals during key repeat.
	 */
	g_uart_peek = -1;
	while (uart_data_ready())
		(void)uart_getc_nowait();

	while (1) {
		printf("%s", MAIN_PROMPT);
		memset(buffer, 0, MAX_MONITOR_BUFFER);
		GetLine(buffer, MAX_MONITOR_BUFFER, 1);
		printf("\n");

		argc = GetArgc((const char *)buffer);
		if (argc < 1)
			continue;

		argv = GetArgv((const char *)buffer);
		StrUpr(argv[0]);

		for (i = 0; i < NUM_COMMANDS; i++) {
			if (!strcmp(argv[0], MainCmdTable[i].cmd)) {
				(void)MainCmdTable[i].func(argc - 1, argv + 1);
				break;
			}
		}
		if (i == NUM_COMMANDS)
			printf("Unknown command !\r\n");
	}
}

/*
 * J <address> — jump.  BFC00000 means a watchdog reset.  Either way the
 * switch is quiesced first with the same sequence the kernel hand-off
 * uses: this is the path RAM-loaded loaders and manual kernel boots take.
 */
static int CmdCfn(int argc, char *argv[])
{
	unsigned long Address;
	void (*jump)(void);
	if (argc < 1) {
		printf("Usage: J <TargetAddress>\n");
		return FALSE;
	}
	if (!parse_hex_arg(argv[0], &Address, "Address")) {
		printf("Usage: J <TargetAddress>\n");
		return FALSE;
	}

	dprintf("---Jump to address=%X\n", Address);
	jump = (void *)(Address);
	REG32(GIMR_REG) = 0; /* mask all interrupts */
	cli();
	swCore_quiesce();
	flush_cache();
	if (Address == 0xBFC00000) {
		REG32(WDTCNR_REG) = 0; /* arm the watchdog: reset follows */
		for (;;)
			;
	}
	jump();
	return TRUE;
}

/* IPCONFIG [A.B.C.D] — show or set the TFTP server address */
static int CmdIp(int argc, char *argv[])
{
	unsigned char *ptr;
	unsigned int i;
	int ip[4];
	unsigned char ip_u8[4];

	if (argc == 0) {
		unsigned char cur[4];
		tftp_get_server_ip(cur);
		printf(" Target Address=%d.%d.%d.%d\n",
		       cur[0], cur[1], cur[2], cur[3]);
		return 0;
	}

	ptr = (unsigned char *)argv[0];

	for (i = 0; i < 4; i++) {
		ip[i] = strtol((const char *)ptr, (char **)NULL, 10);
		if (ip[i] < 0 || ip[i] > 255) {
			printf("Invalid IP format.\n");
			printf("Usage: IPCONFIG <A.B.C.D>\n");
			return 0;
		}
		if (i < 3) {
			ptr = (unsigned char *)strchr((const char *)ptr, '.');
			if (!ptr) {
				printf("Invalid IP format.\n");
				printf("Usage: IPCONFIG <A.B.C.D>\n");
				return 0;
			}
			ptr++;
		}
	}
	ip_u8[0] = (unsigned char)ip[0];
	ip_u8[1] = (unsigned char)ip[1];
	ip_u8[2] = (unsigned char)ip[2];
	ip_u8[3] = (unsigned char)ip[3];
	tftp_set_server_ip(ip_u8);
	/* Keep the shared server-IP global in sync (the boothold DRAM handoff
	 * sets the same variable — see boot/main.c, boot/net/tftpd.c). */
	g_tftp_server_ip = ((unsigned long)ip[0] << 24) |
			   ((unsigned long)ip[1] << 16) |
			   ((unsigned long)ip[2] << 8) | (unsigned long)ip[3];
	/*replace the MAC address middle 4 bytes.*/
	eth0_mac[1] = ip[0];
	eth0_mac[2] = ip[1];
	eth0_mac[3] = ip[2];
	eth0_mac[4] = ip[3];
	tftp_set_server_mac((const unsigned char *)eth0_mac);
	prom_printf("Now your Target IP is %d.%d.%d.%d\n", ip[0], ip[1], ip[2],
		    ip[3]);
	return 0;
}

static int CmdDumpWord(int argc, char *argv[])
{
	unsigned long src;
	unsigned int len, i;

	if (!require_args(argc, 1, "DW <Address> <Len>"))
		return 0;

	if (!parse_hex_arg(argv[0], &src, "Address"))
		return 0;
	if (src < 0x80000000)
		src |= 0x80000000;

	if (!argv[1])
		len = 1;
	else
		len = strtoul((const char *)(argv[1]), (char **)NULL, 10);
	while ((src) & 0x03)
		src++;

	for (i = 0; i < len; i += 4, src += 16) {
		dprintf("%08X:	%08X	%08X	%08X	%08X\n", src,
			*(unsigned long *)(src), *(unsigned long *)(src + 4),
			*(unsigned long *)(src + 8),
			*(unsigned long *)(src + 12));
	}
	return 0;
}

static int CmdDumpByte(int argc, char *argv[])
{
	unsigned long src;
	unsigned int len;

	if (!require_args(argc, 1, "DB <Address> <Len>"))
		return 0;

	if (!parse_hex_arg(argv[0], &src, "Address"))
		return 0;
	if (src < 0x80000000)
		src |= 0x80000000;
	if (!argv[1])
		len = 16;
	else
		len = strtoul((const char *)(argv[1]), (char **)NULL, 10);

	ddump((unsigned char *)src, len);
	return 0;
}

static int CmdWriteWord(int argc, char *argv[])
{
	unsigned long src;
	unsigned long value;
	int i;

	if (!require_args(argc, 2, "EW <Address> <Value1> <Value2>..."))
		return 0;

	if (!parse_hex_arg(argv[0], &src, "Address"))
		return 0;
	while ((src) & 0x03)
		src++;

	for (i = 0; i < argc - 1; i++, src += 4) {
		if (!parse_hex_arg(argv[i + 1], &value, "Value"))
			return 0;
		*(volatile unsigned int *)(src) = (unsigned int)value;
	}
	return 0;
}

static int CmdWriteByte(int argc, char *argv[])
{
	unsigned long src;
	unsigned long value;
	int i;

	if (!require_args(argc, 2, "EB <Address> <Value1> <Value2>..."))
		return 0;

	if (!parse_hex_arg(argv[0], &src, "Address"))
		return 0;

	for (i = 0; i < argc - 1; i++, src++) {
		if (!parse_hex_arg(argv[i + 1], &value, "Value"))
			return 0;
		*(volatile unsigned char *)(src) = (unsigned char)value;
	}
	return 0;
}

static int CmdCmp(int argc, char *argv[])
{
	unsigned long i;
	unsigned long dst, src;
	unsigned long dst_value, src_value;
	unsigned long length;
	unsigned long error;

	if (!require_args(argc, 3, "CMP <dst> <src> <length>"))
		return 1;
	if (!parse_hex_arg(argv[0], &dst, "Dst"))
		return 1;
	if (!parse_hex_arg(argv[1], &src, "Src"))
		return 1;
	if (!parse_hex_arg(argv[2], &length, "Length"))
		return 1;
	error = 0;
	for (i = 0; i < length; i += 4) {
		dst_value = *(volatile unsigned int *)(dst + i);
		src_value = *(volatile unsigned int *)(src + i);
		if (dst_value != src_value) {
			printf("%dth data(%x %x) error\n", i, dst_value,
			       src_value);
			error = 1;
		}
	}
	if (!error)
		printf("No error found\n");
	return 0;
}

static int CmdAuto(int argc, char *argv[])
{
	if (argc < 1) {
		printf("AutoBurning=%d\n", autoBurn);
		return 0;
	}

	if (argv[0][0] == '0' && argv[0][1] == '\0')
		autoBurn = 0;
	else if (argv[0][0] == '1' && argv[0][1] == '\0')
		autoBurn = 1;
	else {
		printf("AutoBurning=%d\n", autoBurn);
		printf("Usage: AUTOBURN 0|1\n");
		return 0;
	}
	printf("AutoBurning=%d\n", autoBurn);
	return 0;
}

/*
 * LOADADDR [addr] — where TFTP uploads land.  Refused below the first
 * page (exception vectors), inside the running loader, or in the two
 * reserved pages at the top of DRAM; a KSEG1 alias is accepted and
 * normalised to KSEG0.
 */
static int CmdLoad(int argc, char *argv[])
{
	unsigned long addr;

	if (argc < 1) {
		printf("TFTP Load Addr: 0x%x\n", image_address);
		return 0;
	}

	if (!parse_hex_arg(argv[0], &addr, "Address")) {
		printf("Usage: LOADADDR <HexAddress>\n");
		return 0;
	}
	if (addr >= 0xA0000000 && addr < 0xC0000000)
		addr -= 0x20000000;
	if (!ram_window_ok(addr, 4, RAM_LOAD_FLOOR, RAM_RESERVED_TOP,
			   (unsigned long)_ftext, (unsigned long)_end)) {
		printf("Refused: 0x%x is outside free RAM (%x-%x, loader at "
		       "%x-%x)\n",
		       addr, RAM_LOAD_FLOOR, RAM_RESERVED_TOP,
		       (unsigned long)_ftext, (unsigned long)_end);
		return 0;
	}
	image_address = addr;
	printf("Set TFTP Load Addr 0x%x\n", image_address);
	return 0;
}

/*
--------------------------------------------------------------------------
Flash Utility
--------------------------------------------------------------------------
*/
static int CmdFlr(int argc, char *argv[])
{
	unsigned long dst, src;
	unsigned long length;

	if (!require_args(argc, 3, "FLR <dst> <src> <length>"))
		return 0;

	if (!parse_hex_arg(argv[0], &dst, "Dst"))
		return 0;
	if (!parse_hex_arg(argv[1], &src, "Src"))
		return 0;
	if (!parse_hex_arg(argv[2], &length, "Length"))
		return 0;

	if (!ram_window_ok(dst, length, RAM_LOAD_FLOOR, RAM_RESERVED_TOP,
			   (unsigned long)_ftext, (unsigned long)_end)) {
		printf("Refused: %x+%x is outside free RAM\n", dst, length);
		return 0;
	}
	if (src >= SPI_FLASH_SIZE || length > SPI_FLASH_SIZE - src) {
		printf("Refused: flash range %x+%x is outside the chip\n", src,
		       length);
		return 0;
	}

	printf("Flash read from %X to %X with %X bytes	?\n", src, dst, length);
	printf("(Y)es , (N)o ? --> ");

	if (YesOrNo()) {
		if (flashread(dst, src, length)) {
			printf("Flash Read Succeeded!\n");
			file_length_to_server = length;
			image_address = dst;
		} else
			printf("Flash Read Failed!\n");
	} else
		printf("Abort!\n");
	return 0;
}

static int CmdHelp(int argc, char *argv[])
{
	unsigned int i;

	printf("----------------- COMMAND MODE HELP ------------------\n");
	for (i = 0; i < NUM_COMMANDS; i++) {
		if (MainCmdTable[i].msg) {
			printf("%s\n", MainCmdTable[i].msg);
		}
	}

	return TRUE;
}

static int YesOrNo(void)
{
	char iChar[2];

	GetLine(iChar, 2, 1);
	printf("\n");
	if ((iChar[0] == 'Y') || (iChar[0] == 'y'))
		return 1;
	else
		return 0;
}

static int CmdSFlw(int argc, char *argv[])
{
	unsigned long dst_flash_addr_offset = 0;
	unsigned long src_RAM_addr = 0;
	unsigned long length = 0;
	unsigned int end_of_RAM_addr;

	if (!require_args(argc, 3, "FLW <dst> <src> <length>"))
		return 1;

	if (!parse_hex_arg(argv[0], &dst_flash_addr_offset, "Dst"))
		return 1;
	if (!parse_hex_arg(argv[1], &src_RAM_addr, "Src"))
		return 1;
	if (!parse_hex_arg(argv[2], &length, "Length"))
		return 1;
	if (!ram_window_ok(src_RAM_addr, length, RAM_LOAD_FLOOR,
			   RAM_RESERVED_TOP, (unsigned long)_ftext,
			   (unsigned long)_end)) {
		printf("Refused: RAM range %x+%x is outside free RAM\n",
		       src_RAM_addr, length);
		return 1;
	}

	end_of_RAM_addr = src_RAM_addr + length;
	printf("Write 0x%x Bytes to SPI flash, offset 0x%x<0x%x>, from RAM "
	       "0x%x to 0x%x\n",
	       (unsigned int)length,
	       (unsigned int)dst_flash_addr_offset,
	       (unsigned int)dst_flash_addr_offset + 0xbd000000,
	       (unsigned int)src_RAM_addr,
	       end_of_RAM_addr);
	printf("(Y)es, (N)o->");
	if (YesOrNo()) {
		if (spi_flw_image((unsigned int)dst_flash_addr_offset,
				  (unsigned char *)src_RAM_addr, length))
			printf("\nFlash Write Succeeded!\n");
		else
			printf("\nFlash Write Failed!\n");
	} else
		printf("Abort!\n");
	return 0;
}

static int TestCmd_MDIOR(int argc, char *argv[])
{
	unsigned int reg;
	unsigned int data;
	int i, phyid;

	if (!require_args(argc, 1, "MDIOR <phyid> <reg>"))
		return 1;

	reg = strtoul((const char *)(argv[0]), (char **)NULL, 10);
	for (i = 0; i < 32; i++) {
		phyid = i;
		rtl8651_getAsicEthernetPHYReg(phyid, reg, &data);
		dprintf("PHYID=0x%02x regID=0x%02x data=0x%04x\r\n", phyid, reg,
			data);
	}
	return 0;
}

static int TestCmd_MDIOW(int argc, char *argv[])
{
	unsigned int phyid, reg, data, tmp;

	if (!require_args(argc, 3, "MDIOW <phyid> <reg> <data>"))
		return 1;
	phyid = strtoul((const char *)(argv[0]), (char **)NULL, 16);
	reg = strtoul((const char *)(argv[1]), (char **)NULL, 10);
	data = strtoul((const char *)(argv[2]), (char **)NULL, 16);
	dprintf("Write PHYID=0x%x regID=0x%x data=0x%x\r\n", phyid, reg, data);
	rtl8651_setAsicEthernetPHYReg(phyid, reg, data);
	rtl8651_getAsicEthernetPHYReg(phyid, reg, &tmp);
	dprintf("Readback PHYID=0x%x regID=0x%x data=0x%x\r\n", phyid, reg, tmp);
	return 0;
}

static int CmdPHYregR(int argc, char *argv[])
{
	unsigned long phyid, regnum;
	unsigned int tmp;

	if (!require_args(argc, 2, "PHYR <phyid> <reg>"))
		return 1;
	phyid = strtoul((const char *)(argv[0]), (char **)NULL, 16);
	regnum = strtoul((const char *)(argv[1]), (char **)NULL, 16);
	rtl8651_getAsicEthernetPHYReg(phyid, regnum, &tmp);
	prom_printf("PHYID=0x%x regID=0x%x data=0x%x\r\n", phyid, regnum, tmp);
	return 0;
}

static int CmdPHYregW(int argc, char *argv[])
{
	unsigned long phyid, regnum;
	unsigned long data;
	unsigned int tmp;

	if (!require_args(argc, 3, "PHYW <phyid> <reg> <data>"))
		return 1;
	phyid = strtoul((const char *)(argv[0]), (char **)NULL, 16);
	regnum = strtoul((const char *)(argv[1]), (char **)NULL, 16);
	data = strtoul((const char *)(argv[2]), (char **)NULL, 16);
	prom_printf("Write PHYID=0x%x regID=0x%x data=0x%x\r\n",
		    phyid, regnum, data);
	rtl8651_setAsicEthernetPHYReg(phyid, regnum, data);
	rtl8651_getAsicEthernetPHYReg(phyid, regnum, &tmp);
	prom_printf("Readback PHYID=0x%x regID=0x%x data=0x%x\r\n",
		    phyid, regnum, tmp);
	return 0;
}
