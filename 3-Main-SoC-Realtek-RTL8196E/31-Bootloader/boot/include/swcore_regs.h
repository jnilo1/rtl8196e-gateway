/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * swcore_regs.h - RTL8196E switch core: the registers the loader addresses
 *
 * The CPU interface (descriptor rings, interrupts), the MAC and port
 * configuration, the table access engine and the ASIC table window.  Only
 * what swCore.c, swNic.c, swTable.c and their callers use; the field bits
 * are listed under the register they belong to.  Derived from the Realtek
 * RTL865xC asicregs.h.
 *
 * Copyright (c) 2002-2020 Realtek Semiconductor Corp.
 * Copyright (c) 2024-2026 J. Nilo
 */
#ifndef _SWCORE_REGS_H_
#define _SWCORE_REGS_H_

#include <stdint.h>

#define SWCORE_BASE 0xBB800000

/* --- CPU interface (0xB8010000) ------------------------------------------- */

#define CPU_IFACE_BASE 0xB8010000
#define CPUICR (0x000 + CPU_IFACE_BASE)	   /* Interface control */
#define CPURPDCR0 (0x004 + CPU_IFACE_BASE) /* Rx pkthdr descriptor ring 0..5 */
#define CPURPDCR1 (0x008 + CPU_IFACE_BASE)
#define CPURPDCR2 (0x00c + CPU_IFACE_BASE)
#define CPURPDCR3 (0x010 + CPU_IFACE_BASE)
#define CPURPDCR4 (0x014 + CPU_IFACE_BASE)
#define CPURPDCR5 (0x018 + CPU_IFACE_BASE)
#define CPURMDCR0 (0x01c + CPU_IFACE_BASE) /* Rx mbuf descriptor ring */
#define CPUTPDCR0 (0x020 + CPU_IFACE_BASE) /* Tx pkthdr descriptor ring 0..3 */
#define CPUTPDCR1 (0x024 + CPU_IFACE_BASE)
#define CPUTPDCR2 (0x060 + CPU_IFACE_BASE)
#define CPUTPDCR3 (0x064 + CPU_IFACE_BASE)
#define CPUIIMR (0x028 + CPU_IFACE_BASE) /* Interrupt mask */
#define CPUIISR (0x02c + CPU_IFACE_BASE) /* Interrupt status */

/* CPUICR */
#define TXCMD (1 << 31) /* Enable Tx */
#define RXCMD (1 << 30) /* Enable Rx */
#define BUSBURST_32WORDS 0
#define MBUF_2048BYTES (4 << 24)
#define TXFD (1 << 23) /* Notify Tx descriptor fetch */

/* CPUIIMR / CPUIISR */
#define PKTHDR_DESC_RUNOUT_IE_ALL (0x3f << 17) /* Any Rx pkthdr ring ran out */
#define PKTHDR_DESC_RUNOUT_IP_ALL (0x3f << 17)
#define MBUF_DESC_RUNOUT_IP_ALL (1 << 16) /* The Rx mbuf ring ran out */
/* Either ring: the switch is stalled until a descriptor comes back. */
#define RX_RUNOUT_IP_ALL (PKTHDR_DESC_RUNOUT_IP_ALL | MBUF_DESC_RUNOUT_IP_ALL)
#define TX_DONE_IE_ALL (0x3 << 9)  /* Any Tx ring: one packet done */
#define RX_DONE_IE_ALL (0x3f << 3) /* Any Rx ring: one packet done */

/* Descriptor word 0 */
#define DESC_OWNED_BIT (1 << 0)
#define DESC_RISC_OWNED (0 << 0)
#define DESC_SWCORE_OWNED (1 << 0)
#define DESC_WRAP (1 << 1)

/* --- MAC (SWCORE + 0x4000) ------------------------------------------------ */

#define MACCR (0x000 + SWCORE_BASE + 0x4000)   /* MAC configuration */
#define MDCIOCR (0x004 + SWCORE_BASE + 0x4000) /* MDC/MDIO command */
#define MDCIOSR (0x008 + SWCORE_BASE + 0x4000) /* MDC/MDIO status */

/* MACCR */
#define SELIPG_MASK (0x3 << 18)	 /* Min. IPG between backpressure data */
#define SELIPG_11 (2 << 18)	 /* 11, unit: byte-time */
#define CF_RXIPG_MASK (0xf << 0) /* Min. IPG for Rx, 6..12 */

/* MDCIOCR */
#define COMMAND_READ (0 << 31)
#define COMMAND_WRITE (1 << 31)
#define PHYADD_OFFSET (24) /* PHY address */
#define REGADD_OFFSET (16) /* PHY register */

/* MDCIOSR */
#define STATUS (1 << 31) /* 1: in progress */

/* --- Port configuration (SWCORE + 0x4100) --------------------------------- */

#define PITCR (0x000 + SWCORE_BASE + 0x4100) /* Port interface type */
#define PCRP0 (0x004 + SWCORE_BASE + 0x4100) /* Port configuration, port 0..4 */
#define PCRP1 (0x008 + SWCORE_BASE + 0x4100)
#define PCRP2 (0x00C + SWCORE_BASE + 0x4100)
#define PCRP3 (0x010 + SWCORE_BASE + 0x4100)
#define PCRP4 (0x014 + SWCORE_BASE + 0x4100)
#define P0GMIICR (0x04C + SWCORE_BASE + 0x4100) /* Port 0 GMII configuration */

/* PCRPn */
#define ExtPHYID_OFFSET (26)
#define EnForceMode (1 << 25) /* Force link/speed/duplex/flow */
#define ForceLink (1 << 23)   /* 0: link down, 1: link up */
#define ForceSpeed100M (1 << 19)
#define ForceSpeed1000M (2 << 19)
#define ForceDuplex (1 << 18)
#define AutoNegoSts_MASK (0x1f << 18)
#define MIIcfg_RXER (1 << 13)
#define MacSwReset (1 << 3) /* 0: reset state, 1: normal state */
#define EnablePHYIf (1 << 0)

/* P0GMIICR */
#define LINK_RGMII 0
#define LINK_MII_MAC 1 /* GMII/MII MAC auto mode */
#define LINK_MII_PHY 2 /* GMII/MII PHY auto mode */
#define Conf_done (1 << 6)

/* PHY control register (through MDIO) */
#define RESTART_AUTONEGO (1 << 9)

/* --- Misc (SWCORE + 0x4200), LED (+ 0x4300), ALE (+ 0x4400) --------------- */

#define SIRR (0x04 + SWCORE_BASE + 0x4200)  /* System initial and reset */
#define MEMCR (0x34 + SWCORE_BASE + 0x4200) /* Memory control */
#define TRXRDY (1 << 0)			    /* SIRR: start normal Tx and Rx */

#define LEDCR (0x000 + SWCORE_BASE + 0x4300) /* LED control */

#define MSCR (0x10 + SWCORE_BASE + 0x4400)   /* Module switch control */
#define SWTCR0 (0x18 + SWCORE_BASE + 0x4400) /* Switch table control 0 */
#define FFCR (0x28 + SWCORE_BASE + 0x4400)   /* Frame forwarding configuration */
#define EN_L2 (1 << 0)			     /* MSCR: enable L2 */
#define EN_UNUNICAST_TOCPU (1 << 1) /* FFCR: trap unknown unicast to CPU */
#define EN_UNMCAST_TOCPU (1 << 0)   /* FFCR: trap unknown multicast to CPU */

/* SWTCR0 */
#define STOP_TLU_READY (1 << 19)
#define EN_STOP_TLU (1 << 18)

/* --- Queues (SWCORE + 0x4700), port VLAN (+ 0x4A00) ----------------------- */

#define QNUMCR (0x54 + SWCORE_BASE + 0x4700) /* Output queue number */
#define P0QNum_1 (1 << 0)		     /* one output queue per port */
#define P1QNum_1 (1 << 3)
#define P2QNum_1 (1 << 6)
#define P3QNum_1 (1 << 9)
#define P4QNum_1 (1 << 12)

#define PVCR0 (0x08 + 0x4A00 + SWCORE_BASE) /* Port VLAN control 0..3 */
#define PVCR1 (0x0C + 0x4A00 + SWCORE_BASE)
#define PVCR2 (0x10 + 0x4A00 + SWCORE_BASE)
#define PVCR3 (0x14 + 0x4A00 + SWCORE_BASE)

/* --- Table access (SWCORE + 0x4D00) --------------------------------------- */

#define SWTACR (0x000 + SWCORE_BASE + 0x4D00) /* Table access control */
#define SWTASR (0x004 + SWCORE_BASE + 0x4D00) /* Table access status */
#define SWTAA (0x008 + SWCORE_BASE + 0x4D00)  /* Table access address */
#define TCR0 (0x020 + SWCORE_BASE + 0x4D00)   /* Entry words 0..7 */
#define TCR1 (0x024 + SWCORE_BASE + 0x4D00)
#define TCR2 (0x028 + SWCORE_BASE + 0x4D00)
#define TCR3 (0x02C + SWCORE_BASE + 0x4D00)
#define TCR4 (0x030 + SWCORE_BASE + 0x4D00)
#define TCR5 (0x034 + SWCORE_BASE + 0x4D00)
#define TCR6 (0x038 + SWCORE_BASE + 0x4D00)
#define TCR7 (0x03C + SWCORE_BASE + 0x4D00)

/* SWTACR */
#define ACTION_MASK 1
#define ACTION_DONE 0
#define ACTION_START 1
#define CMD_ADD (1 << 1)
#define CMD_FORCE (1 << 3)

/* SWTASR */
#define TABSTS_MASK 1
#define TABSTS_SUCCESS 0

/* --- ASIC tables (0xBB000000, one 64 KiB window per table) ---------------- */

enum {
	TYPE_L2_SWITCH_TABLE = 0,
	TYPE_ARP_TABLE,
	TYPE_L3_ROUTING_TABLE,
	TYPE_MULTICAST_TABLE,
	TYPE_NETINTERFACE_TABLE,
	TYPE_EXT_INT_IP_TABLE,
	TYPE_VLAN_TABLE,
	TYPE_VLAN1_TABLE,
	TYPE_SERVER_PORT_TABLE,
	TYPE_L4_TCP_UDP_TABLE,
	TYPE_L4_ICMP_TABLE,
	TYPE_PPPOE_TABLE,
	TYPE_ACL_RULE_TABLE,
	TYPE_NEXT_HOP_TABLE,
	TYPE_RATE_LIMIT_TABLE,
	TYPE_ALG_TABLE,
};
#define table_access_addr_base(type) (0xbb000000 + 0x10000 * (type))
#define TABLE_ENTRY_DISTANCE (8 * sizeof(uint32_t)) /* 8 words per entry */

#define MAX_PORT_NUMBER 6
#define ALL_PORT_MASK 0x3F
#define RTL8651_L2TBL_ROW 256
#define RTL8651_L2TBL_COLUMN 4
#define RTL8651_IPMULTICASTTBL_SIZE 64
#define RTL865XC_NETINTERFACE_NUMBER 8

#endif /* _SWCORE_REGS_H_ */
