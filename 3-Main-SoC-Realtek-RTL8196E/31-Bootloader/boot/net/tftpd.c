// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * tftpd.c - TFTP server for firmware recovery
 *
 * RTL8196E stage-2 bootloader
 *
 * Implements a minimal TFTP server that accepts WRQ (write request)
 * packets, receives firmware images into RAM, validates checksums,
 * and auto-flashes them to SPI flash.  RRQ serves back whatever was
 * last loaded or read from flash (FLR), which is how a flash dump is
 * pulled off the board.
 *
 * Threat model: these services are reachable only while the board sits
 * at the <RealTek> prompt, on the local segment, and their whole point is
 * to accept an unauthenticated image from that segment.  What is enforced
 * here is that a malformed packet cannot corrupt memory, hang the loader
 * or make it write garbage to flash.
 *
 * kick_tftpd() runs from the main loop (eth_poll()), never from the
 * interrupt handler, so the flash write below may take minutes with the
 * timer alive.
 *
 * Copyright (c) 2009-2020 Realtek Semiconductor Corp.
 * Copyright (c) 2024-2026 J. Nilo
 */

#include "boot_common.h"
#include "boot_soc.h"
#include "boot_irq.h"
#include "boot_net.h"
#include "spi_flash.h"
#include "main.h"
#include "checks.h"
#include <rtl_types.h>
#include "swcore.h"
#include "ramtest_trace.h"

struct arptable_t arptable_tftp[2];

#define FILESTART JUMP_ADDR

#define prom_printf dprintf

/* Flash geometry the auto-flash path checks images against. */
#define FLASH_CHIP_SIZE 0x1000000
#define FLASH_SECTOR_SIZE 0x1000

static int tftpd_is_ready = 0;
static int rx_kickofftime = 0;
static unsigned char one_tftp_lock = 0;

struct nic nic;
static unsigned char eth_packet[ETH_FRAME_LEN + 4];

#define IPTOUL(a, b, c, d) ((a << 24) | (b << 16) | (c << 8) | d)

unsigned long image_address = FILESTART;
static unsigned long address_to_store;

unsigned long file_length_to_server;

/*
 * g_tftp_server_ip - active download-mode server IP (packed a<<24|b<<16|c<<8|d).
 *
 * Defaults to the compiled fallback 192.168.1.6.  It may be overridden:
 *   - at warm reboot, via the boothold DRAM handoff (see boot/main.c), or
 *   - at runtime, via the IPCONFIG console command (see boot/monitor.c CmdIp).
 * tftpd_entry() applies it (and the matching MAC) when the server starts.
 */
unsigned long g_tftp_server_ip = IPTOUL(192, 168, 1, 6);

/* One transmit frame, built in place: IP + UDP + TFTP.  Static, not on
 * the stack — struct tftp_t is 1.5 KiB. */
static struct tftp_t tftp_tx;

static inline struct udphdr *tftp_udp_header(void)
{
	return (struct udphdr *)&nic.packet[ETH_HLEN + sizeof(struct iphdr)];
}

static inline struct tftp_t *tftp_packet(void)
{
	return (struct tftp_t *)&nic.packet[ETH_HLEN];
}

static inline void tftp_capture_client(void)
{
	memcpy(arptable_tftp[TFTP_CLIENT].node,
	       (unsigned char *)&(nic.packet[ETH_ALEN]), ETH_ALEN);
	memcpy(&(arptable_tftp[TFTP_CLIENT].ipaddr.s_addr),
	       (unsigned char *)&nic.packet[ETH_HLEN + 12], 4);
}

void tftp_get_server_ip(unsigned char ip[4])
{
	memcpy(ip, arptable_tftp[TFTP_SERVER].ipaddr.ip, 4);
}

void tftp_set_server_ip(const unsigned char ip[4])
{
	memcpy(arptable_tftp[TFTP_SERVER].ipaddr.ip, ip, 4);
}

void tftp_set_server_mac(const unsigned char mac[6])
{
	memcpy(arptable_tftp[TFTP_SERVER].node, mac, 6);
}

static volatile unsigned short block_expected;


/* State-event machine for TFTP boot downloader */

typedef enum BootStateTag {
	INVALID_BOOT_STATE = -1,
	BOOT_STATE0_INIT_ARP = 0,
	BOOT_STATE1_TFTP_CLIENT_WRQ = 1,
	BOOT_STATE2_TFTP_SERVER_RRQ = 2,
	NUM_OF_BOOT_STATES = 3
} BootState_t;

typedef enum BootEventTag {
	INVALID_BOOT_EVENT = -1,
	BOOT_EVENT0_ARP_REQ = 0,
	BOOT_EVENT1_ARP_REPLY = 1,
	BOOT_EVENT2_TFTP_RRQ = 2,
	BOOT_EVENT3_TFTP_WRQ = 3,
	BOOT_EVENT4_TFTP_DATA = 4,
	BOOT_EVENT5_TFTP_ACK = 5,
	BOOT_EVENT6_TFTP_ERROR = 6,
	BOOT_EVENT7_TFTP_OACK = 7,
	NUM_OF_BOOT_EVENTS = 8
} BootEvent_t;

static BootState_t bootState;

static unsigned long read_src;    /* RAM address of next block to send */
static unsigned long read_remain; /* bytes remaining to send */
static unsigned char read_pct;    /* last printed progress percentage */

static void errorDrop(void);
static void errorTFTP(void);
static void doARPReply(void);
static void updateARPTable(void);
static void setTFTP_WRQ(void);
static void prepareACK(void);
static void handleTFTP_RRQ(void);
static void handleTFTP_ACK(void);
static void tftp_reset_transfer(void);

static unsigned short CLIENT_port;
static unsigned short SERVER_port;

/* A transfer stays bound to the peer that opened it; TFTP has no session ID. */
struct tftp_peer {
	unsigned char mac[ETH_ALEN];
	in_addr ip;
	unsigned short port;
	int valid;
};
static struct tftp_peer transfer_peer;

static void tftp_peer_capture(unsigned short port)
{
	memcpy(transfer_peer.mac, (unsigned char *)&nic.packet[ETH_ALEN],
	       ETH_ALEN);
	memcpy(&transfer_peer.ip.s_addr,
	       (unsigned char *)&nic.packet[ETH_HLEN + 12], 4);
	transfer_peer.port = port;
	transfer_peer.valid = 1;
}

static int tftp_peer_matches(unsigned short port)
{
	return transfer_peer.valid && transfer_peer.port == port &&
	       !memcmp(transfer_peer.mac, (unsigned char *)&nic.packet[ETH_ALEN],
		       ETH_ALEN) &&
	       !memcmp(&transfer_peer.ip.s_addr,
		       (unsigned char *)&nic.packet[ETH_HLEN + 12], 4);
}

static void tftp_peer_clear(void)
{
	transfer_peer.valid = 0;
}

static void tftpd_send_ack(unsigned short number);
static void tftpd_send_error(unsigned short code, const char *msg);
static void tftpd_send_notify(const char *msg);
unsigned short ipheader_chksum(unsigned short *ip, int len);

/*
 * dispatch_event - run the handler for (bootState, event)
 *
 * The protocol has two useful states besides idle: a client upload in
 * progress (WRQ) and a client download in progress (RRQ).  ARP is
 * answered in every state; a request that starts a transfer is accepted
 * from idle (and a WRQ retransmit while uploading, see kick_tftpd());
 * DATA is only meaningful while uploading, ACK only while downloading;
 * anything else is dropped from idle and aborts a transfer otherwise.
 * This replaces the former 3 x 8 table, cell for cell.
 */
static void dispatch_event(BootEvent_t event)
{
	int busy = bootState != BOOT_STATE0_INIT_ARP;

	switch (event) {
	case BOOT_EVENT0_ARP_REQ:
		doARPReply();
		break;
	case BOOT_EVENT1_ARP_REPLY:
		updateARPTable();
		break;
	case BOOT_EVENT2_TFTP_RRQ:
		if (busy)
			errorDrop();
		else
			handleTFTP_RRQ();
		break;
	case BOOT_EVENT3_TFTP_WRQ:
		if (bootState == BOOT_STATE2_TFTP_SERVER_RRQ)
			errorDrop();
		else
			setTFTP_WRQ();
		break;
	case BOOT_EVENT4_TFTP_DATA:
		if (bootState == BOOT_STATE1_TFTP_CLIENT_WRQ)
			prepareACK();
		else
			errorDrop();
		break;
	case BOOT_EVENT5_TFTP_ACK:
		if (bootState == BOOT_STATE2_TFTP_SERVER_RRQ)
			handleTFTP_ACK();
		else
			errorDrop();
		break;
	case BOOT_EVENT6_TFTP_ERROR:
	case BOOT_EVENT7_TFTP_OACK:
		if (busy && tftp_peer_matches(ntohs(tftp_udp_header()->src)))
			errorTFTP();
		else
			errorDrop();
		break;
	default:
		break; /* NUM_OF_BOOT_EVENTS: nothing to dispatch */
	}
}

static void errorDrop(void)
{
	if (!tftpd_is_ready)
		return;
	prom_printf("Boot state error: %d\n", bootState);
}

static void errorTFTP(void)
{
	if (!tftpd_is_ready)
		return;
	tftp_reset_transfer();
}

/* Abort the current transfer and go back to idle. */
static void tftp_reset_transfer(void)
{
	nic.packet = (char *)eth_packet;
	nic.packetlen = 0;
	block_expected = 0;
	address_to_store = image_address;
	file_length_to_server = 0;
	bootState = BOOT_STATE0_INIT_ARP;
	one_tftp_lock = 0;
	tftp_peer_clear();
	SERVER_port++;
}

static void doARPReply(void)
{
	struct arprequest *arppacket;
	struct arprequest arpreply;
	unsigned long targetIP;

	arppacket = (struct arprequest *)&(nic.packet[ETH_HLEN]);

	memcpy(&targetIP, arppacket->tipaddr, 4);

	if (targetIP == arptable_tftp[TFTP_SERVER].ipaddr.s_addr) {
		arpreply.hwtype = htons(1);
		arpreply.protocol = htons(ETH_P_IP);
		arpreply.hwlen = ETH_ALEN;
		arpreply.protolen = 4;
		arpreply.opcode = htons(ARP_REPLY);
		memcpy(&(arpreply.shwaddr),
		       &(arptable_tftp[TFTP_SERVER].node), ETH_ALEN);
		memcpy(&(arpreply.sipaddr),
		       &(arptable_tftp[TFTP_SERVER].ipaddr),
		       sizeof(in_addr));
		memcpy(&(arpreply.thwaddr), arppacket->shwaddr, ETH_ALEN);
		memcpy(&(arpreply.tipaddr), arppacket->sipaddr,
		       sizeof(in_addr));

		prepare_txpkt(0, ETH_P_ARP, (unsigned char *)arppacket->shwaddr,
			      (unsigned char *)&arpreply,
			      (unsigned short)sizeof(arpreply));
	}
}

static void updateARPTable(void) {}

/* Fill the IP + UDP headers of tftp_tx for a payload of @tftp_len bytes. */
static void tftp_tx_headers(unsigned short src_port, unsigned short dst_port,
			    unsigned short tftp_len)
{
	struct iphdr *ip = (struct iphdr *)&tftp_tx;
	struct udphdr *udp =
	    (struct udphdr *)((unsigned char *)&tftp_tx + sizeof(struct iphdr));

	ip->verhdrlen = 0x45;
	ip->service = 0;
	ip->len = htons(sizeof(struct iphdr) + sizeof(struct udphdr) + tftp_len);
	ip->ident = 0;
	ip->frags = 0;
	ip->ttl = 60;
	ip->protocol = IPPROTO_UDP;
	ip->chksum = 0;
	ip->src.s_addr = arptable_tftp[TFTP_SERVER].ipaddr.s_addr;
	ip->dest.s_addr = arptable_tftp[TFTP_CLIENT].ipaddr.s_addr;
	ip->chksum = ipheader_chksum((unsigned short *)&tftp_tx,
				     sizeof(struct iphdr));

	udp->src = htons(src_port);
	udp->dest = htons(dst_port);
	udp->len = htons(sizeof(struct udphdr) + tftp_len);
	udp->chksum = 0;
}

static void tftp_tx_send(unsigned short tftp_len)
{
	prepare_txpkt(0, ETH_P_IP, arptable_tftp[TFTP_CLIENT].node,
		      (unsigned char *)&tftp_tx,
		      (unsigned short)(sizeof(struct iphdr) +
				       sizeof(struct udphdr) + tftp_len));
}

static void tftpd_send_data(unsigned short block, unsigned char *data,
			    unsigned short datalen)
{
	tftp_tx.opcode = htons(TFTP_DATA);
	tftp_tx.u.data.block = htons(block);
	memcpy(tftp_tx.u.data.download, data, datalen);
	tftp_tx_headers(SERVER_port, CLIENT_port, 4 + datalen);
	tftp_tx_send(4 + datalen);
}

static void tftpd_send_ack(unsigned short number)
{
	tftp_tx.opcode = htons(TFTP_ACK);
	tftp_tx.u.ack.block = htons(number);
	tftp_tx_headers(SERVER_port, CLIENT_port, 4);
	tftp_tx_send(4);
}

static void tftpd_send_error(unsigned short code, const char *msg)
{
	unsigned short n = 0;

	tftp_tx.opcode = htons(TFTP_ERROR);
	tftp_tx.u.err.errcode = htons(code);
	while (msg[n] && n < 63) {
		tftp_tx.u.err.errmsg[n] = msg[n];
		n++;
	}
	tftp_tx.u.err.errmsg[n++] = 0;
	tftp_tx_headers(SERVER_port, CLIENT_port, 4 + n);
	tftp_tx_send(4 + n);
}

static void handleTFTP_RRQ(void)
{
	struct udphdr *udpheader;
	unsigned short sent;

	if (!tftpd_is_ready)
		return;

	if (file_length_to_server == 0) {
		prom_printf("**TFTP RRQ Error: no data loaded\n");
		return;
	}

	udpheader = tftp_udp_header();
	if (udpheader->dest != htons(TFTP_PORT))
		return;

	CLIENT_port = ntohs(udpheader->src);
	tftp_capture_client();
	tftp_peer_capture(CLIENT_port);

	read_src = image_address;
	read_remain = file_length_to_server;
	read_pct = 0;
	block_expected = 1;
	one_tftp_lock = 1;
	bootState = BOOT_STATE2_TFTP_SERVER_RRQ;

	sent = (read_remain > TFTP_DEFAULTSIZE_PACKET) ?
		TFTP_DEFAULTSIZE_PACKET : read_remain;
	tftpd_send_data(block_expected, (unsigned char *)read_src, sent);
	read_src += sent;
	read_remain -= sent;

	prom_printf("\n**TFTP Server Download: %X bytes from %X\n",
		    file_length_to_server, image_address);
}

static void handleTFTP_ACK(void)
{
	struct udphdr *udpheader;
	struct tftp_t *tftppacket;
	unsigned short ack_block;
	unsigned short sent;

	if (!tftpd_is_ready)
		return;

	udpheader = tftp_udp_header();
	if (udpheader->dest != htons(SERVER_port))
		return;
	if (!tftp_peer_matches(ntohs(udpheader->src)))
		return;

	tftppacket = tftp_packet();
	ack_block = ntohs(tftppacket->u.ack.block);

	if (ack_block != block_expected)
		return;

	block_expected++;
	sent = (read_remain > TFTP_DEFAULTSIZE_PACKET) ?
		TFTP_DEFAULTSIZE_PACKET : read_remain;
	tftpd_send_data(block_expected, (unsigned char *)read_src, sent);
	read_src += sent;
	read_remain -= sent;

	{
		unsigned char pct = ((file_length_to_server - read_remain) *
				     100) / file_length_to_server;
		if (pct != read_pct) {
			read_pct = pct;
			prom_printf("\r%d%%", pct);
		}
	}

	if (sent < TFTP_DEFAULTSIZE_PACKET) {
		bootState = BOOT_STATE0_INIT_ARP;
		one_tftp_lock = 0;
		tftp_peer_clear();
		SERVER_port++;
		prom_printf("\nTFTP Download Complete!\n%s", "<RealTek>");
	}
}

static void setTFTP_WRQ(void)
{
	struct udphdr *udpheader;
	struct tftp_t *tftppacket;
	unsigned short client_port;

	if (!tftpd_is_ready)
		return;

	udpheader = tftp_udp_header();
	if (udpheader->dest == htons(TFTP_PORT)) {
		client_port = ntohs(udpheader->src);
		if (bootState == BOOT_STATE1_TFTP_CLIENT_WRQ &&
		    !tftp_peer_matches(client_port))
			return;
		CLIENT_port = client_port;
		tftppacket = tftp_packet();
		tftp_capture_client();
		if (bootState == BOOT_STATE0_INIT_ARP)
			tftp_peer_capture(client_port);
		/* The file name is client-supplied and only printed; bound it. */
		tftppacket->u.wrq[sizeof(tftppacket->u.wrq) - 1] = 0;
		prom_printf("\n**TFTP Client Upload, File Name: %s\n",
			    tftppacket->u.wrq);

		address_to_store = image_address;
		file_length_to_server = 0;
		tftpd_send_ack(0x0000);
		block_expected = 1;
		one_tftp_lock = 1;
		bootState = BOOT_STATE1_TFTP_CLIENT_WRQ;
	}
}

SIGN_T sign_tbl[] = { //  signature, name, sig_len, skip, maxSize, reboot
    {(unsigned char *)FW_SIGNATURE, (unsigned char *)"Linux kernel", SIG_LEN, 0, 0x1000000, 1},
    {(unsigned char *)FW_SIGNATURE_WITH_ROOT, (unsigned char *)"Linux kernel (root-fs)", SIG_LEN, 0, 0x1000000, 1},
    {(unsigned char *)ROOT_SIGNATURE, (unsigned char *)"Root filesystem", SIG_LEN, 1, 0x1000000, 1},
    {(unsigned char *)BOOT_SIGNATURE, (unsigned char *)"Boot code", SIG_LEN, 1, 0x1000000, 1},
    {(unsigned char *)ALL1_SIGNATURE, (unsigned char *)"Total Image", SIG_LEN, 1, 0x1000000, 1},
    {(unsigned char *)ALL2_SIGNATURE, (unsigned char *)"Total Image (no check)", SIG_LEN, 1, 0x1000000, 1}};

#define MAX_SIG_TBL (sizeof(sign_tbl) / sizeof(SIGN_T))
int autoBurn = 1;

/**
 * autoreboot - Reset the board after a successful flash
 *
 * Lets the queued UDP notification leave the switch (descriptor handed
 * back, then a short settle for the egress queue), quiesces the switch so
 * no DMA is in flight across the watchdog reset — the reset preserves
 * DRAM and does not reset the switch, which is how a 16 MiB transfer used
 * to corrupt early kernel boot — and pulls the watchdog.
 */
void autoreboot(void)
{
	swNic_wait_tx_idle();
	delay_ms(20);

	REG32(GIMR_REG) = 0; /* mask all interrupts */
	cli();
	swCore_quiesce();
	flush_cache();
	prom_printf("\nreboot.......\n");
	REG32(WDTCNR_REG) = 0; /* arm the watchdog: reset follows */
	for (;;)
		;
}

/* 16-bit checksum over @len bytes at @p (unaligned-safe). Zero when valid. */
static unsigned short image_sum16(const unsigned char *p, unsigned long len)
{
	unsigned short sum = 0, temp;
	unsigned long i;

	for (i = 0; i < len; i += 2) {
		memcpy(&temp, p + i, 2);
		sum += temp;
	}
	return sum;
}

#define MAX_FLASH_PLAN 4

/*
 * Return the on-flash policy for one cvimg header.  A valid checksum is not
 * authority to select an arbitrary flash offset: every image has a fixed
 * partition and the two data partitions are the only valid ROOT targets.
 */
static int flash_image_policy(const IMG_HEADER_T *header,
			      unsigned long burn_len, int *skip_header)
{
	if (!memcmp(header->signature, FW_SIGNATURE, SIG_LEN) ||
	    !memcmp(header->signature, FW_SIGNATURE_WITH_ROOT, SIG_LEN)) {
		*skip_header = 0;
		return header->burnAddr == KERNEL_PARTITION_OFFSET &&
		       flash_partition_ok(header->burnAddr, burn_len,
					  KERNEL_PARTITION_OFFSET,
					  KERNEL_PARTITION_SIZE);
	}
	if (!memcmp(header->signature, BOOT_SIGNATURE, SIG_LEN)) {
		*skip_header = 1;
		return header->burnAddr == BOOT_PARTITION_OFFSET &&
		       flash_partition_ok(header->burnAddr, burn_len,
					  BOOT_PARTITION_OFFSET,
					  BOOT_PARTITION_SIZE);
	}
	if (!memcmp(header->signature, ROOT_SIGNATURE, SIG_LEN)) {
		*skip_header = 1;
		if (header->burnAddr == ROOTFS_PARTITION_OFFSET)
			return flash_partition_ok(header->burnAddr, burn_len,
						  ROOTFS_PARTITION_OFFSET,
						  ROOTFS_PARTITION_SIZE);
		if (header->burnAddr == USERDATA_PARTITION_OFFSET)
			return flash_partition_ok(header->burnAddr, burn_len,
						  USERDATA_PARTITION_OFFSET,
						  USERDATA_PARTITION_SIZE);
	}
	return 0;
}

/* Validate the entire cvimg package before the first flash erase. */
static int autoflash_preflight(unsigned long start_addr, unsigned long len)
{
	unsigned long offset = 0, starts[MAX_FLASH_PLAN], lens[MAX_FLASH_PLAN];
	unsigned int count = 0, i;
	IMG_HEADER_T header;
	int skip_header;
	unsigned long burn_len;

	if (len < sizeof(header) || (len & 1))
		return 0;
	memcpy(&header, (void *)start_addr, sizeof(header));
	if (!memcmp(header.signature, ALL1_SIGNATURE, SIG_LEN)) {
		/* ALL1 is a checksummed container, never a flashable partition. */
		if (header.len != len - sizeof(header) || (header.len & 1) ||
		    image_sum16((unsigned char *)start_addr + sizeof(header),
				header.len))
			return 0;
		offset = sizeof(header);
	}

	while (offset < len) {
		/*
		 * cvimg -a pads kernel images to their alignment with zeros;
		 * the tail is not a header, the plan ends here. The legacy
		 * loop tolerated it by accident (a zero length advanced it
		 * 16 bytes at a time); this is the same tolerance, stated.
		 */
		if (upload_tail_is_padding((const unsigned char *)start_addr +
						   offset, len - offset))
			break;
		if (len - offset < sizeof(header) || count == MAX_FLASH_PLAN)
			return 0;
		memcpy(&header, (void *)(start_addr + offset), sizeof(header));
		/* ALL2 deliberately bypassed type checks in the legacy loader. */
		if (!memcmp(header.signature, ALL1_SIGNATURE, SIG_LEN) ||
		    !memcmp(header.signature, ALL2_SIGNATURE, SIG_LEN))
			return 0;

		if (!memcmp(header.signature, FW_SIGNATURE, SIG_LEN) ||
		    !memcmp(header.signature, FW_SIGNATURE_WITH_ROOT, SIG_LEN))
			skip_header = 0;
		else if (!memcmp(header.signature, BOOT_SIGNATURE, SIG_LEN) ||
			 !memcmp(header.signature, ROOT_SIGNATURE, SIG_LEN))
			skip_header = 1;
		else
			return 0;
		burn_len = header.len + (skip_header ? 0 : sizeof(header));
		if (!autoflash_header_ok(offset, sizeof(header), header.len, len,
					 header.burnAddr, burn_len, SPI_FLASH_SIZE,
					 FLASH_SECTOR_SIZE) ||
		    !flash_image_policy(&header, burn_len, &skip_header) ||
		    image_sum16((unsigned char *)(start_addr + offset +
					      sizeof(header)), header.len))
			return 0;
		for (i = 0; i < count; i++)
			if (ranges_overlap(header.burnAddr, burn_len, starts[i], lens[i]))
				return 0;
		starts[count] = header.burnAddr;
		lens[count++] = burn_len;
		offset += sizeof(header) + header.len;
	}
	return count != 0;
}

/* Write @len bytes at flash @dst, report on the console. 1 = ok. */
static int flash_part(unsigned long dst, unsigned long src, unsigned long len)
{
	int ok;

	prom_printf("Flash write: dst=0x%x src=0x%x len=0x%x (%d bytes)\n",
		    dst, src, len, (int)len);
	ok = spi_flw_image(dst, (unsigned char *)src, len);
	prom_printf(ok ? "\nFlash Write Succeeded!\n" : "\nFlash Write Failed!\n");
	return ok;
}

/*
 * Raw 16 MiB fullflash (build_fullflash.sh): bootloader at 0 (first word
 * is the reset-vector jump), kernel with its cs6c header at 0x20000,
 * squashfs at 0x200000.  The kernel checksum is verified before anything
 * is erased, and the bootloader partition is written last so the window
 * during which the board has no bootloader is the ~1 s it takes to
 * program two blocks, not the minutes the rest takes.
 *
 * Return: 1 written and verified, 0 refused or failed (notification sent).
 */
/**
 * fullflash_crc_ok - check the CRC trailer of a raw fullflash, if any
 * @startAddr: RAM address of the 16 MiB image
 *
 * The trailer (see checks.h) covers the whole image but its own 16 bytes.
 * A trailer is mandatory. A mismatch is a corrupted transfer or a stale
 * backup; foreign or absent bytes are refused before anything is written
 * (re-run backup_gateway.sh, or lib/fullflash_crc.sh write, to sign it).
 *
 * Return: 1 to go on, 0 to refuse the image
 */
static int fullflash_crc_ok(unsigned long startAddr)
{
	unsigned long want, got;
	int t0;

	switch (fullflash_trailer_parse((unsigned char *)startAddr +
						FULLFLASH_TRAILER_OFFSET,
					FLASH_CHIP_SIZE, &want)) {
	case FULLFLASH_TRAILER_NONE:
		prom_printf("fullflash CRC trailer is required\n");
		return 0;
	case FULLFLASH_TRAILER_FOREIGN:
		prom_printf("unrecognised data at 0x%x: not a CRC trailer\n",
			    FULLFLASH_TRAILER_OFFSET);
		return 0;
	case FULLFLASH_TRAILER_VALID:
		break;
	}

	t0 = get_timer_jiffies();
	rt_wdt_kick();
	got = crc32(0, (void *)startAddr, FULLFLASH_TRAILER_OFFSET);
	rt_wdt_kick();
	got = crc32(got,
		    (void *)(startAddr + FULLFLASH_TRAILER_OFFSET +
			     FULLFLASH_TRAILER_LEN),
		    FLASH_CHIP_SIZE - FULLFLASH_TRAILER_OFFSET -
			FULLFLASH_TRAILER_LEN);
	rt_wdt_kick();
	if (got != want) {
		prom_printf("fullflash CRC error: image %08x, trailer %08x\n",
			    got, want);
		return 0;
	}
	prom_printf("fullflash CRC Ok ! (%08x, %d ms)\n", got,
		    (get_timer_jiffies() - t0) * 10);
	return 1;
}

static int flash_raw_fullflash(unsigned long startAddr)
{
	IMG_HEADER_T kh;

	prom_printf("\nRaw fullflash detected (16 MiB).\n");
	memcpy(&kh, (void *)(startAddr + KERNEL_PARTITION_OFFSET), sizeof(kh));
	if (kh.len == 0 || (kh.len & 1) ||
	    kh.len > KERNEL_PARTITION_SIZE - sizeof(kh) ||
	    image_sum16((unsigned char *)(startAddr + KERNEL_PARTITION_OFFSET +
					  sizeof(kh)),
			kh.len)) {
		prom_printf("fullflash kernel checksum error at 0x%x!\n",
			    KERNEL_PARTITION_OFFSET);
		tftpd_send_notify("FAIL");
		return 0;
	}
	prom_printf("kernel checksum Ok !\n");

	if (!fullflash_crc_ok(startAddr)) {
		tftpd_send_notify("FAIL");
		return 0;
	}

	if (!flash_part(KERNEL_PARTITION_OFFSET,
			startAddr + KERNEL_PARTITION_OFFSET,
			FLASH_CHIP_SIZE - KERNEL_PARTITION_OFFSET) ||
	    !flash_part(0, startAddr, KERNEL_PARTITION_OFFSET)) {
		prom_printf("%s", "<RealTek>");
		tftpd_send_notify("FAIL");
		return 0;
	}
	prom_printf("%s", "<RealTek>");
	tftpd_send_notify("OK");
	autoreboot();
}

/**
 * checkAutoFlashing - Identify an upload and write it to flash
 * @startAddr: RAM address of the upload
 * @len: bytes uploaded
 *
 * Handles the raw 16 MiB fullflash, and one or more concatenated cvimg
 * images (boot / kernel / rootfs / userdata, each with a 16-byte header
 * and a trailing 16-bit checksum).  Sends OK or FAIL on UDP:9999 and
 * reboots after a successful write.
 *
 * Return: 1 if everything was written, 0 otherwise
 */
int checkAutoFlashing(unsigned long startAddr, unsigned long len)
{
	unsigned int i = 0;
	unsigned long head_offset = 0, srcAddr, burnLen;
	unsigned short sum = 0;
	int skip_header = 0;
	int reboot = 0;
	IMG_HEADER_T Header;
	int skip_check_signature = 0;
	int flash_ok = 0;

	if (len == FLASH_CHIP_SIZE) {
		unsigned int m_boot = *((volatile unsigned int *)startAddr);
		unsigned int m_kern = *((volatile unsigned int *)(startAddr + KERNEL_PARTITION_OFFSET));
		unsigned int m_root = *((volatile unsigned int *)(startAddr + 0x200000));
		if (m_boot == 0x0bf00004 &&
		    m_kern == 0x63733663 &&   /* cs6c */
		    m_root == 0x68737173)     /* hsqs */
			return flash_raw_fullflash(startAddr);
	}
	if (!autoflash_preflight(startAddr, len)) {
		prom_printf("auto-flash package rejected before write\n%s", "<RealTek>");
		tftpd_send_notify("FAIL");
		return 0;
	}

	while ((head_offset + sizeof(IMG_HEADER_T)) < len) {
		memcpy(&Header, ((char *)startAddr + head_offset),
		       sizeof(IMG_HEADER_T));

		if (!skip_check_signature) {
			for (i = 0; i < MAX_SIG_TBL; i++) {
				if (!memcmp(Header.signature,
					    (char *)sign_tbl[i].signature,
					    sign_tbl[i].sig_len))
					break;
			}
			if (i == MAX_SIG_TBL) {
				if (Header.len > len)
					break; /* not an image at all */
				head_offset +=
				    Header.len + sizeof(IMG_HEADER_T);
				continue;
			}
			skip_header = sign_tbl[i].skip;
			reboot |= sign_tbl[i].reboot;
			prom_printf("\n%s upgrade.\n", sign_tbl[i].comment);
		} else {
			if (!memcmp(Header.signature, BOOT_SIGNATURE, SIG_LEN))
				skip_header = 1;
			else {
				unsigned char *pRoot =
				    ((unsigned char *)startAddr) + head_offset +
				    sizeof(IMG_HEADER_T);
				skip_header =
				    !memcmp(pRoot, SQSH_SIGNATURE, SIG_LEN);
			}
		}
		if (skip_header) {
			srcAddr = startAddr + head_offset + sizeof(IMG_HEADER_T);
			burnLen = Header.len;
		} else {
			srcAddr = startAddr + head_offset;
			burnLen = Header.len + sizeof(IMG_HEADER_T);
		}

		/*
		 * The header is part of the upload: a length claiming more
		 * than was received would make the checksum and the write
		 * read stale RAM past the image, and a destination outside
		 * the chip or off a sector boundary would program the wrong
		 * place (a misaligned burnAddr near zero reaches into the
		 * bootloader through the read-modify-write of the neighbour).
		 */
		if (!autoflash_header_ok(head_offset, sizeof(IMG_HEADER_T),
					 Header.len, len, Header.burnAddr,
					 burnLen, FLASH_CHIP_SIZE,
					 FLASH_SECTOR_SIZE)) {
			prom_printf("%.4s image header rejected: burn=0x%x "
				    "len=0x%x (upload 0x%x)\n%s",
				    Header.signature, Header.burnAddr,
				    Header.len, len, "<RealTek>");
			tftpd_send_notify("FAIL");
			return 0;
		}

		/* 16-bit checksum (all cvimg-generated images) */
		if (skip_check_signature &&
		    (!memcmp(Header.signature, ALL1_SIGNATURE, SIG_LEN) ||
		     !memcmp(Header.signature, ALL2_SIGNATURE, SIG_LEN)))
			sum = image_sum16((unsigned char *)(startAddr + head_offset),
					  Header.len + sizeof(IMG_HEADER_T));
		else
			sum = image_sum16((unsigned char *)(startAddr + head_offset +
							    sizeof(IMG_HEADER_T)),
					  Header.len);
		if (sum) {
			prom_printf("%.4s image checksum error at %X!\n%s",
				    Header.signature, startAddr + head_offset,
				    "<RealTek>");
			tftpd_send_notify("FAIL");
			return 0;
		}
		if (skip_check_signature) {
			if (!memcmp(Header.signature, ALL1_SIGNATURE, SIG_LEN)) {
				head_offset += sizeof(IMG_HEADER_T);
				continue;
			}
			if (!memcmp(Header.signature, ALL2_SIGNATURE, SIG_LEN)) {
				skip_check_signature = 1;
				head_offset += sizeof(IMG_HEADER_T);
				continue;
			}
		}
		prom_printf("checksum Ok !\n");

		if (!flash_part(Header.burnAddr, srcAddr, burnLen)) {
			prom_printf("%s", "<RealTek>");
			tftpd_send_notify("FAIL");
			return 0;
		}
		prom_printf("%s", "<RealTek>");
		flash_ok = 1;

		head_offset += Header.len + sizeof(IMG_HEADER_T);
	}
	tftpd_send_notify(flash_ok ? "OK" : "FAIL");
	if (flash_ok && reboot)
		autoreboot();
	return flash_ok;
}

static void prepareACK(void)
{
	struct udphdr *udpheader;
	struct tftp_t *tftppacket;
	unsigned long tftpdata_length;
	unsigned short block_received;

	if (!tftpd_is_ready)
		return;
	udpheader = tftp_udp_header();
	if (udpheader->dest != htons(SERVER_port))
		return;

	if (!tftp_peer_matches(ntohs(udpheader->src)))
		return;
	tftppacket = tftp_packet();
	block_received = ntohs(tftppacket->u.data.block);
	if (block_received != block_expected) {
		prom_printf("TFTP #\n");
		tftpd_send_ack(block_expected - 1);
		return;
	}

	/*
	 * The UDP length is the sender's word: below 12 it would underflow
	 * to gigabytes, above 12+512 it would copy from beyond the receive
	 * cluster (the frame length was already checked against it).
	 */
	if (!tftp_udp_len_ok(ntohs(udpheader->len), TFTP_DEFAULTSIZE_PACKET)) {
		prom_printf("\nTFTP DATA with bad length %d, aborting\n%s",
			    ntohs(udpheader->len), "<RealTek>");
		tftpd_send_error(TFTP_ERR_ILLEGAL, "bad block length");
		tftp_reset_transfer();
		return;
	}
	tftpdata_length = ntohs(udpheader->len) - TFTP_DATA_HDR;

	/*
	 * The destination must stay inside DRAM, below the reserved pages,
	 * and clear of this loader: an upload larger than the free RAM
	 * would otherwise wrap over the exception vectors or the code that
	 * is receiving it.
	 */
	if (!ram_window_ok(address_to_store, tftpdata_length, RAM_LOAD_FLOOR,
			   RAM_RESERVED_TOP, (unsigned long)_ftext,
			   (unsigned long)_end)) {
		prom_printf("\nUpload does not fit at %X (%X bytes so far), "
			    "aborting\n%s",
			    address_to_store, file_length_to_server,
			    "<RealTek>");
		tftpd_send_error(TFTP_ERR_DISKFULL, "no room in RAM");
		tftp_reset_transfer();
		return;
	}

	rt_set(RT_BLOCK, block_expected);
	rt_set(RT_ADDR, address_to_store);
	rt_set(RT_PHASE, RT_PHASE_DATA);
	memcpy((void *)address_to_store, tftppacket->u.data.download,
	       tftpdata_length);
	rt_set(RT_PHASE, RT_PHASE_COPIED);
	address_to_store += tftpdata_length;
	file_length_to_server += tftpdata_length;
	twiddle();
	tftpd_send_ack(block_expected);
	rt_set(RT_PHASE, RT_PHASE_ACKED);
	block_expected++;
	if (tftpdata_length < TFTP_DEFAULTSIZE_PACKET) {
		unsigned long total = file_length_to_server;

		prom_printf("\n**TFTP Client Upload File Size = %X Bytes at %X\n",
			    total, image_address);
		/* keep file_length_to_server: RRQ serves it back */
		nic.packet = (char *)eth_packet;
		nic.packetlen = 0;
		block_expected = 0;
		address_to_store = image_address;
		bootState = BOOT_STATE0_INIT_ARP;
		one_tftp_lock = 0;
		tftp_peer_clear();
		SERVER_port++;

		prom_printf("\nSuccess!\n%s", "<RealTek>");

		if (autoBurn)
			checkAutoFlashing(image_address, total);
	}
}

/**
 * tftpd_entry - Initialize the TFTP server state machine
 *
 * Applies g_tftp_server_ip (compiled default, IPCONFIG, or the boothold
 * hand-off), initializes the ARP table, packet buffer, and state machine.
 * After this call, kick_tftpd() processes incoming packets.
 */
void tftpd_entry(void)
{
	arptable_tftp[TFTP_SERVER].ipaddr.s_addr = g_tftp_server_ip;
	arptable_tftp[TFTP_CLIENT].ipaddr.s_addr = 0; /* learned from the first request */

	/*
	 * Mirror the IP->MAC coupling that IPCONFIG performs (boot/monitor.c
	 * CmdIp): the middle four MAC bytes track the server IP, so ARP stays
	 * consistent whether the IP came from the compiled default, the IPCONFIG
	 * command, or the boothold DRAM handoff.
	 */
	eth0_mac[1] = (g_tftp_server_ip >> 24) & 0xFF;
	eth0_mac[2] = (g_tftp_server_ip >> 16) & 0xFF;
	eth0_mac[3] = (g_tftp_server_ip >> 8) & 0xFF;
	eth0_mac[4] = g_tftp_server_ip & 0xFF;

	memcpy(arptable_tftp[TFTP_SERVER].node, eth0_mac, 6);

	prom_printf("TFTP server IP: %d.%d.%d.%d\n",
		    (int)((g_tftp_server_ip >> 24) & 0xFF),
		    (int)((g_tftp_server_ip >> 16) & 0xFF),
		    (int)((g_tftp_server_ip >> 8) & 0xFF),
		    (int)(g_tftp_server_ip & 0xFF));

	bootState = BOOT_STATE0_INIT_ARP;
	nic.packet = (char *)eth_packet;
	nic.packetlen = 0;

	block_expected = 0;
	one_tftp_lock = 0;
	tftp_peer_clear();

	address_to_store = image_address;

	file_length_to_server = 0;

	SERVER_port = 2098;

	tftpd_is_ready = 1;
}

#define NOTIFY_PORT 9999

/**
 * tftpd_send_notify - Send a UDP notification to the TFTP client
 * @msg: null-terminated message string (e.g., "OK" or "FAIL")
 *
 * Sends a small UDP packet to the client on NOTIFY_PORT after a flash
 * operation completes. The host script listens on this port to detect
 * completion without requiring serial console confirmation.
 */
static void tftpd_send_notify(const char *msg)
{
	struct iphdr *ip;
	struct udphdr *udp;
	unsigned short msglen;
	unsigned char pkt[sizeof(struct iphdr) + sizeof(struct udphdr) + 32];

	for (msglen = 0; msg[msglen] && msglen < 31; msglen++)
		;
	msglen++; /* include the NUL terminator */

	ip = (struct iphdr *)pkt;
	udp = (struct udphdr *)(pkt + sizeof(struct iphdr));

	ip->verhdrlen = 0x45;
	ip->service = 0;
	ip->len = htons(sizeof(struct iphdr) + sizeof(struct udphdr) + msglen);
	ip->ident = 0;
	ip->frags = 0;
	ip->ttl = 60;
	ip->protocol = IPPROTO_UDP;
	ip->chksum = 0;
	ip->src.s_addr = arptable_tftp[TFTP_SERVER].ipaddr.s_addr;
	ip->dest.s_addr = arptable_tftp[TFTP_CLIENT].ipaddr.s_addr;
	ip->chksum = ipheader_chksum((unsigned short *)pkt,
				     sizeof(struct iphdr));

	udp->src = htons(NOTIFY_PORT);
	udp->dest = htons(NOTIFY_PORT);
	udp->len = htons(sizeof(struct udphdr) + msglen);
	udp->chksum = 0;

	memcpy(pkt + sizeof(struct iphdr) + sizeof(struct udphdr), msg, msglen);

	prepare_txpkt(0, ETH_P_IP, arptable_tftp[TFTP_CLIENT].node,
		      pkt,
		      (unsigned short)(sizeof(struct iphdr) +
				       sizeof(struct udphdr) + msglen));
}

/**
 * handle_icmp_echo - Reply to an ICMP Echo Request (ping)
 * @ipheader: pointer to the IP header of the received packet
 *
 * Validates the ICMP Echo Request, builds an Echo Reply with swapped
 * src/dest addresses, recomputes IP and ICMP checksums, and sends it.
 * The reply length is bounded by what the NIC actually delivered, not
 * only by the IP length field, so a short frame claiming 1500 bytes
 * cannot leak the previous packet's bytes.
 */
static void handle_icmp_echo(struct iphdr *ipheader)
{
	struct icmphdr *icmp;
	unsigned short icmp_len;
	/* Static reply buffer: IP header + ICMP payload (up to 1480 bytes) */
	static unsigned char icmp_reply[sizeof(struct iphdr) + 1480];
	struct iphdr *reply_ip;
	struct icmphdr *reply_icmp;

	icmp = (struct icmphdr *)((unsigned char *)ipheader +
				  sizeof(struct iphdr));
	if (icmp->type != ICMP_ECHO || icmp->code != 0)
		return;

	icmp_len = ntohs(ipheader->len) - sizeof(struct iphdr);
	if (icmp_len < sizeof(struct icmphdr) ||
	    icmp_len > 1480)
		return;
	if (nic.packetlen < ETH_HLEN + sizeof(struct iphdr) + icmp_len)
		return;

	/* Build IP reply header: swap src/dest, recompute checksum */
	reply_ip = (struct iphdr *)icmp_reply;
	*reply_ip = *ipheader;
	reply_ip->src  = ipheader->dest;
	reply_ip->dest = ipheader->src;
	reply_ip->chksum = 0;
	reply_ip->chksum = ipheader_chksum((unsigned short *)reply_ip,
					   sizeof(struct iphdr));

	/* Copy ICMP header + data, set type to Echo Reply */
	reply_icmp = (struct icmphdr *)(icmp_reply + sizeof(struct iphdr));
	memcpy(reply_icmp, icmp, icmp_len);
	reply_icmp->type   = ICMP_ECHOREPLY;
	reply_icmp->chksum = 0;
	reply_icmp->chksum = ipheader_chksum((unsigned short *)reply_icmp,
					     icmp_len);

	/* nic.packet[ETH_ALEN..ETH_ALEN+5] is the source MAC of the request */
	prepare_txpkt(0, ETH_P_IP,
		      (unsigned char *)&nic.packet[ETH_ALEN],
		      icmp_reply,
		      (unsigned short)(sizeof(struct iphdr) + icmp_len));
}

/**
 * kick_tftpd - Process one received Ethernet packet
 *
 * Called from eth_poll() (main loop) for each received frame.
 * Classifies the packet (ARP request/reply, ICMP echo, TFTP WRQ/DATA/ERROR)
 * and dispatches to the appropriate handler.  Length fields carried by
 * the packet are checked against what the NIC delivered before any
 * parser trusts them.
 */
void kick_tftpd(void)
{
	unsigned short pkttype = 0;
	struct arprequest *arppacket;
	unsigned short arpopcode;
	struct tftp_t *tftppacket;
	struct udphdr *udpheader;
	unsigned short tftpopcode;
	struct iphdr *ipheader;
	in_addr ip_addr;
	BootEvent_t kick_event = NUM_OF_BOOT_EVENTS;

	if (nic.packetlen >= ETH_HLEN + sizeof(struct arprequest)) {
		pkttype =
		    ((unsigned short)(nic.packet[12] << 8) |
		     (unsigned short)(nic.packet[13]));
	}

	switch (pkttype) {
	case ETH_P_ARP:
		arppacket = (struct arprequest *)&nic.packet[ETH_HLEN];
		arpopcode = arppacket->opcode;

		switch (arpopcode) {
		case htons(ARP_REQUEST):
			if (!memcmp(arppacket->tipaddr,
				    &arptable_tftp[TFTP_SERVER].ipaddr, 4))
				kick_event = BOOT_EVENT0_ARP_REQ;
			break;
		case htons(ARP_REPLY):
			kick_event = BOOT_EVENT1_ARP_REPLY;
			break;
		}
		dispatch_event(kick_event);
		break;

	case ETH_P_IP:
		ipheader = (struct iphdr *)&nic.packet[ETH_HLEN];
		/* word-aligned copy of destination IP */
		ip_addr.ip[0] = ipheader->dest.ip[0];
		ip_addr.ip[1] = ipheader->dest.ip[1];
		ip_addr.ip[2] = ipheader->dest.ip[2];
		ip_addr.ip[3] = ipheader->dest.ip[3];

		if (ipheader->verhdrlen != 0x45)
			break;
		if (ip_addr.s_addr != arptable_tftp[TFTP_SERVER].ipaddr.s_addr)
			break;
		if (ipheader_chksum((unsigned short *)ipheader,
				    sizeof(struct iphdr)))
			break;
		if (nic.packetlen < ETH_HLEN + ntohs(ipheader->len))
			break;

		if (ipheader->protocol == IPPROTO_ICMP) {
			handle_icmp_echo(ipheader);
			break;
		}
		if (ipheader->protocol != IPPROTO_UDP)
			break;

		udpheader = tftp_udp_header();
		if (!udp_frame_lens_ok(nic.packetlen, ntohs(ipheader->len),
				       ntohs(udpheader->len)))
			break;
		if (ntohs(udpheader->len) < sizeof(struct udphdr) + 4)
			break; /* no room for a TFTP opcode + block/name */

		tftppacket = (struct tftp_t *)&nic.packet[ETH_HLEN];
		tftpopcode = tftppacket->opcode;
		switch (tftpopcode) {
		case htons(TFTP_RRQ):
			if (one_tftp_lock == 0) {
				kick_event = BOOT_EVENT2_TFTP_RRQ;
				rx_kickofftime = get_timer_jiffies();
			}
			break;
		case htons(TFTP_WRQ):
			if (one_tftp_lock == 0) {
				kick_event = BOOT_EVENT3_TFTP_WRQ;
				rx_kickofftime = get_timer_jiffies();
			} else {
				/* WRQ retransmit or timeout (20s) */
				if ((block_expected == 1) ||
				    ((get_timer_jiffies() - rx_kickofftime) >
				     2000)) {
					kick_event = BOOT_EVENT3_TFTP_WRQ;
					rx_kickofftime = get_timer_jiffies();
				}
			}
			break;
		case htons(TFTP_DATA):
			kick_event = BOOT_EVENT4_TFTP_DATA;
			rx_kickofftime = get_timer_jiffies();
			break;
		case htons(TFTP_ACK):
			if (bootState == BOOT_STATE2_TFTP_SERVER_RRQ) {
				kick_event = BOOT_EVENT5_TFTP_ACK;
				rx_kickofftime = get_timer_jiffies();
			}
			break;
		case htons(TFTP_ERROR):
			kick_event = BOOT_EVENT6_TFTP_ERROR;
			break;
		case htons(TFTP_OACK):
			kick_event = BOOT_EVENT7_TFTP_OACK;
			break;
		}

		dispatch_event(kick_event);
		break;
	}
}

unsigned short ipheader_chksum(unsigned short *ip, int len)
{
	unsigned long sum = 0;
	len >>= 1;
	while (len--) {
		sum += *(ip++);
		if (sum > 0xFFFF)
			sum -= 0xFFFF;
	}
	return ((~sum) & 0x0000FFFF);
}
