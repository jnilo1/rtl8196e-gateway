/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RTL8196E_DESC_H
#define RTL8196E_DESC_H

#include <linux/types.h>
#include <asm/byteorder.h>

/*
 * The rtl_pktHdr / rtl_mBuf bitfield layout below mirrors the on-wire
 * descriptor format consumed by the RTL8196E switch ASIC. C bitfield
 * packing is endianness-dependent: this driver targets big-endian MIPS
 * (Lexra RLX4181) only. Block any accidental little-endian build at
 * compile time so we never ship a binary that compiles cleanly but
 * fills the wrong bits in TX/RX descriptors.
 */
#if !defined(__BIG_ENDIAN_BITFIELD)
# error "rtl8196e-eth: descriptor layout requires __BIG_ENDIAN_BITFIELD"
#endif

/* Mbuf layout: must match RTL819x hardware expectations. */
#define BUF_FREE 0x00
#define BUF_USED 0x80
#define BUF_ASICHOLD 0x80
#define BUF_DRIVERHOLD 0xc0

struct rtl_mBuf {
	struct rtl_mBuf *m_next;
	struct rtl_pktHdr *m_pkthdr;
	u16 m_len;
	s8 m_flags;
#define MBUF_FREE BUF_FREE
#define MBUF_USED BUF_USED
#define MBUF_EXT 0x10
#define MBUF_PKTHDR 0x08
#define MBUF_EOR 0x04
	u8 *m_data;
	u8 *m_extbuf;
	u16 m_extsize;
	s8 m_reserved[2];
	void *skb;
	u32 pending0;
};

struct rtl_pktHdr {
	union {
		struct rtl_pktHdr *pkthdr_next;
		struct rtl_mBuf *mbuf_first;
	} PKTHDRNXT;
#define ph_nextfree PKTHDRNXT.pkthdr_next
#define ph_mbuf     PKTHDRNXT.mbuf_first
	u16 ph_len;
	u16 ph_reserved1 : 1;
	u16 ph_queueId : 3;
	u16 ph_extPortList : 4;
#define PKTHDR_EXTPORT_LIST_CPU 3
	u16 ph_reserved2 : 3;
	u16 ph_hwFwd : 1;
	u16 ph_isOriginal : 1;
	u16 ph_l2Trans : 1;
	u16 ph_srcExtPortNum : 2;
	u16 ph_type : 3;
#define ph_proto ph_type
#define PKTHDR_ETHERNET 0
	u16 ph_vlanTagged : 1;
	u16 ph_LLCTagged : 1;
	u16 ph_pppeTagged : 1;
	u16 ph_pppoeIdx : 3;
	u16 ph_linkID : 7;
	u16 ph_reason;
	u16 ph_flags;
#define PKTHDR_FREE (BUF_FREE << 8)
#define PKTHDR_USED (BUF_USED << 8)
#define PKTHDR_ASICHOLD (BUF_ASICHOLD << 8)
#define PKTHDR_DRIVERHOLD (BUF_DRIVERHOLD << 8)
#define PKTHDR_CPU_OWNED 0x4000
#define PKT_INCOMING 0x1000
#define PKT_OUTGOING 0x0800
#define PKT_BCAST 0x0100
#define PKT_MCAST 0x0080
#define PKTHDR_BRIDGING 0x0040
#define PKTHDR_HWLOOKUP 0x0020
#define PKTHDR_PPPOE_AUTOADD 0x0004
#define CSUM_TCPUDP_OK 0x0001
#define CSUM_IP_OK 0x0002
	u8 ph_orgtos;
	u8 ph_portlist;
	u16 ph_vlanId_resv : 1;
	u16 ph_txPriority : 3;
	u16 ph_vlanId : 12;
	union {
		u16 _flags2;
		struct {
			u16 _reserved : 1;
			u16 _rxPktPriority : 3;
			u16 _svlanId : 12;
		} _rx;
		struct {
			u16 _reserved : 10;
			u16 _txCVlanTagAutoAdd : 6;
		} _tx;
	} _flags2;
#define ph_flags2 _flags2._flags2
};

/* Descriptor ownership bits */
#define RTL8196E_DESC_OWNED_BIT    (1 << 0)
#define RTL8196E_DESC_RISC_OWNED   (0 << 0)
#define RTL8196E_DESC_SWCORE_OWNED (1 << 0)
#define RTL8196E_DESC_WRAP         (1 << 1)

#endif /* RTL8196E_DESC_H */
