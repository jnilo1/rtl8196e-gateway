/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * boot_net.h - The loader's network: frame formats and the eth/TFTP API
 *
 * Ethernet, ARP, IP, UDP, ICMP and TFTP as the ARP / ICMP echo / TFTP server
 * of net/tftpd.c spells them (Etherboot lineage), the one-packet NIC buffer,
 * and what net/eth.c and net/tftpd.c export.
 *
 * Copyright (C) 1993 Martin Renters (Etherboot)
 * Copyright (c) 2024-2026 J. Nilo
 */
#ifndef _BOOT_NET_H_
#define _BOOT_NET_H_

#include "boot_common.h"

/* Network byte order is the CPU's: the loader is big-endian only. */
#define ntohs(x) (x)
#define htons(x) (x)

/* --- Ethernet, ARP, IP ------------------------------------------------------ */

#define ETH_ALEN 6	   /* Size of Ethernet address */
#define ETH_HLEN 14	   /* Size of ethernet header */
#define ETH_FRAME_LEN 1514 /* Maximum packet */

#define ETH_P_IP 0x0800
#define ETH_P_ARP 0x0806

#define ARP_REQUEST 1
#define ARP_REPLY 2

#define IPPROTO_ICMP 1
#define IPPROTO_UDP 17

#define ICMP_ECHO 8
#define ICMP_ECHOREPLY 0

typedef union {
	unsigned long s_addr;
	unsigned char ip[4];
} in_addr;

struct arptable_t {
	in_addr ipaddr;
	unsigned char node[6];
};

/* sipaddr and tipaddr are not longword aligned: bytes, not in_addr. */
struct arprequest {
	unsigned short hwtype;
	unsigned short protocol;
	char hwlen;
	char protolen;
	unsigned short opcode;
	char shwaddr[6];
	char sipaddr[4];
	char thwaddr[6];
	char tipaddr[4];
};

/*
 * IP header, Etherboot naming (verhdrlen = version + ihl, service = tos,
 * len = tot_len, ident = id, frags = frag_off, chksum = check).
 */
struct iphdr {
	char verhdrlen;
	char service;
	unsigned short len;
	unsigned short ident;
	unsigned short frags;
	char ttl;
	char protocol;
	unsigned short chksum;
	in_addr src;
	in_addr dest;
};

struct udphdr {
	unsigned short src;
	unsigned short dest;
	unsigned short len;
	unsigned short chksum;
};

struct icmphdr {
	unsigned char type;
	unsigned char code;
	unsigned short chksum;
	unsigned short id;
	unsigned short seq;
};

/* --- TFTP (RFC 1350) -------------------------------------------------------- */

#define TFTP_PORT 69

#define TFTP_DEFAULTSIZE_PACKET 512
#define TFTP_MAX_PACKET 1432

#define TFTP_RRQ 1
#define TFTP_WRQ 2
#define TFTP_DATA 3
#define TFTP_ACK 4
#define TFTP_ERROR 5
#define TFTP_OACK 6

#define TFTP_ERR_DISKFULL 3
#define TFTP_ERR_ILLEGAL 4

/* Role of the loader in the transfer */
#define TFTP_SERVER 0
#define TFTP_CLIENT 1

struct tftp_t {
	struct iphdr ip;
	struct udphdr udp;
	unsigned short opcode;
	union {
		char rrq[TFTP_DEFAULTSIZE_PACKET];
		char wrq[TFTP_DEFAULTSIZE_PACKET];
		struct {
			unsigned short block;
			char download[TFTP_MAX_PACKET];
		} data;
		struct {
			unsigned short block;
		} ack;
		struct {
			unsigned short errcode;
			char errmsg[TFTP_DEFAULTSIZE_PACKET];
		} err;
		struct {
			char data[TFTP_DEFAULTSIZE_PACKET + 2];
		} oack;
	} u;
};

/* --- The received packet (net/eth.c fills it, net/tftpd.c parses it) -------- */

struct nic {
	char *packet;
	unsigned int packetlen;
};
extern struct nic nic;

/* --- net/eth.c --------------------------------------------------------------- */

extern char eth0_mac[6];
int eth_startup(int etherport);
void eth_poll(void);
void prepare_txpkt(int etherport, unsigned short type, unsigned char *destaddr,
		   unsigned char *data, unsigned short len);

/* --- net/tftpd.c ------------------------------------------------------------- */

extern struct arptable_t arptable_tftp[2];
extern unsigned long file_length_to_server;
extern unsigned long image_address;
extern unsigned long g_tftp_server_ip;
extern int autoBurn;
void kick_tftpd(void);
void tftpd_entry(void);
void tftp_get_server_ip(unsigned char ip[4]);
void tftp_set_server_ip(const unsigned char ip[4]);
void tftp_set_server_mac(const unsigned char mac[6]);
void autoreboot(void) __attribute__((noreturn));

#endif /* _BOOT_NET_H_ */
