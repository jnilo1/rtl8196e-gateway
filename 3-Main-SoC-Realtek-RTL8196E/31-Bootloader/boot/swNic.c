// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * swNic.c - NIC polling driver for the switch ASIC
 *
 * RTL8196E stage-2 bootloader
 *
 * Copyright (c) 2002-2020 Realtek Semiconductor Corp.
 * Copyright (c) 2024-2026 J. Nilo
 */
#include "boot_common.h"
#include "boot_soc.h"
#include "boot_net.h"
#include <stdlib.h>
#include <rtl_types.h>
#include "swcore_regs.h"
#include "swcore.h"
#include "ramtest_trace.h"

/* refer to rtl865xc_swNic.c & rtl865xc_swNic.h
 */
#define RTL865X_SWNIC_RXRING_MAX_PKTDESC 6
#define RTL865X_SWNIC_TXRING_MAX_PKTDESC 4

/* RX Ring */
static uint32
    *rxPkthdrRing[RTL865X_SWNIC_RXRING_MAX_PKTDESC]; /* Point to the starting
							address of RX pkt Hdr
							Ring */
static uint32
    rxPkthdrRingCnt[RTL865X_SWNIC_RXRING_MAX_PKTDESC]; /* Total pkt count for
							  each Rx descriptor
							  Ring */
// static uint32	rxPkthdrRingIndex[RTL865X_SWNIC_RXRING_MAX_PKTDESC];
// /* Current Index for each Rx descriptor Ring */

/* TX Ring */
static uint32
    *txPkthdrRing[RTL865X_SWNIC_TXRING_MAX_PKTDESC]; /* Point to the starting
							address of TX pkt Hdr
							Ring */

static uint32
    txPkthdrRingCnt[RTL865X_SWNIC_TXRING_MAX_PKTDESC]; /* Total pkt count for
							  each Tx descriptor
							  Ring */
// static uint32	txPkthdrRingFreeIndex[RTL865X_SWNIC_TXRING_MAX_PKTDESC];
// /* Point to the entry can be set to SEND packet */

#define txPktHdrRingFull(idx)                                                  \
	(((txPkthdrRingFreeIndex[idx] + 1) & (txPkthdrRingMaxIndex[idx])) ==   \
	 (txPkthdrRingDoneIndex[idx]))

/* Mbuf */
uint32 *rxMbufRing = NULL; /* Point to the starting address of MBUF Ring */
uint32 rxMbufRingCnt;	   /* Total MBUF count */

static uint32 size_of_cluster;

/* descriptor ring tracing pointers */
static int32
    currRxPkthdrDescIndex; /* Rx pkthdr descriptor to be handled by CPU */
static int32 currRxMbufDescIndex; /* Rx mbuf descriptor to be handled by CPU */
static int32
    currTxPkthdrDescIndex; /* Tx pkthdr descriptor to be handled by CPU */
static int32 txPktDoneDescIndex;

/* debug counters */
static int32 rxPktCounter;
static int32 txPktCounter;

/* First receive since init or resync: no previous descriptor to return. */
static int32 rxFirstTime = 1;

/* Probes, visible in boot.nm: run-outs seen with a frame waiting, rings rebuilt. */
unsigned int g_rx_runout;
unsigned int g_rx_resync;

#define BUF_FREE 0x00	    /* Buffer is Free  */
#define BUF_USED 0x80	    /* Buffer is occupied */
#define BUF_ASICHOLD 0x80   /* Buffer is hold by ASIC */
#define BUF_DRIVERHOLD 0xc0 /* Buffer is hold by driver */

//--------------------------------------------------------------------------
/* mbuf header associated with each cluster
 */
struct mBuf {
	struct mBuf *m_next;
	struct pktHdr *m_pkthdr; /* Points to the pkthdr structure */
	uint16 m_len;		 /* data bytes used in this cluster */
	uint16 m_flags;		 /* mbuf flags; see below */
#define MBUF_FREE BUF_FREE /* Free. Not occupied. should be on free list   */
#define MBUF_USED BUF_USED /* Buffer is occupied */
#define MBUF_EXT                                                               \
	0x10 /* has associated with an external cluster, this is always set.   \
	      */
#define MBUF_PKTHDR 0x08  /* is the 1st mbuf of this packet */
#define MBUF_EOR 0x04	  /* is the last mbuf of this packet. Set only by ASIC*/
	uint8 *m_data;	  /*  location of data in the cluster */
	uint8 *m_extbuf;  /* start of buffer*/
	uint16 m_extsize; /* sizeof the cluster */
	int8 m_reserved[2]; /* padding */
};

extern char eth0_mac[6];
//--------------------------------------------------------------------------
/* pkthdr records packet specific information. Each pkthdr is exactly 32 bytes.
 first 20 bytes are for ASIC, the rest 12 bytes are for driver and software
 usage.
*/
struct pktHdr {
	union {
		struct pktHdr *pkthdr_next; /*  next pkthdr in free list */
		struct mBuf *mbuf_first;    /*  1st mbuf of this pkt */
	} PKTHDRNXT;
#define ph_nextfree PKTHDRNXT.pkthdr_next
#define ph_mbuf PKTHDRNXT.mbuf_first
	uint16 ph_len;		 /*   total packet length */
	uint16 ph_reserved1 : 1; /* reserved */
	uint16 ph_queueId : 3;	 /* bit 2~0: Queue ID */
	uint16
	    ph_extPortList : 4; /* dest extension port list. must be 0 for TX */
	uint16 ph_reserved2 : 3;  /* reserved */
	uint16 ph_hwFwd : 1;	  /* hwFwd - copy from HSA bit 200 */
	uint16 ph_isOriginal : 1; /* isOriginal - DP included cpu port or more
				     than one ext port */
	uint16 ph_l2Trans : 1;	  /* l2Trans - copy from HSA bit 129 */
	uint16 ph_srcExtPortNum : 2; /* Both in RX & TX. Source extension port
					number. */

	uint16 ph_type : 3;
#define PKTHDR_ETHERNET 0
#define PKTHDR_IP 2
#define PKTHDR_ICMP 3
#define PKTHDR_IGMP 4
#define PKTHDR_TCP 5
#define PKTHDR_UDP 6
	uint16 ph_vlanTagged : 1; /* the tag status after ALE */
	uint16 ph_LLCTagged : 1;  /* the tag status after ALE */
	uint16 ph_pppeTagged : 1; /* the tag status after ALE */
	uint16 ph_pppoeIdx : 3;
	uint16 ph_linkID : 7; /* for WLAN WDS multiple tunnel */
	uint16 ph_reason;     /* indicates wht the packet is received by CPU */

	uint16 ph_flags; /*  NEW:Packet header status bits */
#define PKTHDR_FREE                                                            \
	(BUF_FREE << 8) /* Free. Not occupied. should be on free list   */
#define PKTHDR_USED (BUF_USED << 8)
#define PKTHDR_ASICHOLD (BUF_ASICHOLD << 8)	/* Hold by ASIC */
#define PKTHDR_DRIVERHOLD (BUF_DRIVERHOLD << 8) /* Hold by driver */
#define PKTHDR_CPU_OWNED 0x4000
#define PKT_INCOMING 0x1000	    /* Incoming: packet is incoming */
#define PKT_OUTGOING 0x0800	    /*  Outgoing: packet is outgoing */
#define PKT_BCAST 0x0100	    /*send/received as link-level broadcast  */
#define PKT_MCAST 0x0080	    /*send/received as link-level multicast   */
#define PKTHDR_PPPOE_AUTOADD 0x0004 /* PPPoE header auto-add */
#define CSUM_TCPUDP_OK 0x0001	    /*Incoming:TCP or UDP cksum checked */
#define CSUM_IP_OK 0x0002	    /* Incoming: IP header cksum has checked */
#define CSUM_TCPUDP 0x0001 /*Outgoing:TCP or UDP cksum offload to ASIC*/
#define CSUM_IP 0x0002	   /* Outgoing: IP header cksum offload to ASIC*/

	uint8 ph_orgtos; /* RX: original TOS of IP header's value before
			    remarking, TX: undefined */
	uint8
	    ph_portlist; /* RX: source port number, TX: destination portmask */

	uint16 ph_vlanId_resv : 1;
	uint16 ph_txPriority : 3;
	uint16 ph_vlanId : 12;
	uint16 ph_flags2;
	uint8 ph_ptpResv : 1;
	uint8 ph_ptpMsgType : 4; /* message type */
	uint8 ph_ptpVer : 2; /* PTP version, 0: 1588v1; 1: 1588v2 or 802.1as;
				others: reserved */
	uint8 ph_ptpPkt : 1; /* 1: PTP */
	int8 ph_reserved[3]; /* padding */
};
//--------------------------------------------------------------------------

/**
 * swNic_receive - Receive one packet from the switch
 * @input: output pointer set to the received packet data
 * @pLen: output pointer set to the packet length
 *
 * Reads one packet from the RX descriptor ring and returns ownership
 * of the previous descriptor back to the switch core.
 *
 * Return: 0 if a packet was received, -1 if no packet available
 */
/*************************************************************************
 *   FUNCTION
 *       swNic_receive
 *
 *   DESCRIPTION
 *       This function reads one packet from rx descriptors, and return the
 *       previous read one to the switch core. This mechanism is based on
 *       the assumption that packets are read only when the handling
 *       previous read one is done.
 *
 *   INPUTS
 *       None
 *
 *   OUTPUTS
 *       None
 *************************************************************************/
int32 swNic_receive(void **input, uint32 *pLen)
{
	struct pktHdr *pPkthdr;
	int32 pkthdr_index;
	int32 mbuf_index;
	char *data;
	int ret = -1;

	/* Check OWN bit of descriptors */
	if ((rxPkthdrRing[0][currRxPkthdrDescIndex] & DESC_OWNED_BIT) ==
	    DESC_RISC_OWNED) {
		// ASSERT_ISR(currRxPkthdrDescIndex < rxPkthdrRingCnt[0]);

		/* Fetch pkthdr */
		pPkthdr =
		    (struct pktHdr *)(rxPkthdrRing[0][currRxPkthdrDescIndex] &
				      ~(DESC_OWNED_BIT | DESC_WRAP));

		// ASSERT_ISR(pPkthdr->ph_len); /* Not allow zero packet length
		// */ ASSERT_ISR(pPkthdr->ph_len >= 64);
		// ASSERT_ISR(pPkthdr->ph_len <= 1522);

		/* Increment counter */
		rxPktCounter++;

		data = pPkthdr->ph_mbuf->m_data;
		if ((data[0] & 0x1) || !memcmp(data, eth0_mac, 6)) {
			/* Output packet */
			*input = data;
			*pLen = pPkthdr->ph_len - 4;
			ret = 0;
		} else
			ret = -1;

		if (!rxFirstTime) {
			/* Calculate previous pkthdr and mbuf index */
			pkthdr_index = currRxPkthdrDescIndex;
			if (--pkthdr_index < 0)
				pkthdr_index = rxPkthdrRingCnt[0] - 1;
			// for rx descriptor runout
			pPkthdr =
			    (struct pktHdr *)(rxPkthdrRing[0][pkthdr_index] &
					      ~(DESC_OWNED_BIT | DESC_WRAP));
			mbuf_index =
			    ((uint32)(pPkthdr->ph_mbuf) -
			     (rxMbufRing[0] & ~(DESC_OWNED_BIT | DESC_WRAP))) /
			    (sizeof(struct mBuf));

			/* Reset OWN bit */
			rxPkthdrRing[0][pkthdr_index] |= DESC_SWCORE_OWNED;
			rxMbufRing[mbuf_index] |= DESC_SWCORE_OWNED;
		} else
			rxFirstTime = 0;

		/* Increment index */
		if (++currRxPkthdrDescIndex == rxPkthdrRingCnt[0])
			currRxPkthdrDescIndex = 0;
		if (++currRxMbufDescIndex == rxMbufRingCnt)
			currRxMbufDescIndex = 0;

		if (REG32(CPUIISR) & RX_RUNOUT_IP_ALL) {
			/*
			 * The switch ran out of descriptors while this frame
			 * was waiting; the descriptor returned above is the
			 * one it stalled on, so the ring moves again.  Count
			 * it and clear the status.  The run-out interrupt is
			 * never enabled: the status is level — it stays set
			 * for as long as the switch is stalled, and the ISR
			 * cannot un-stall it — so an enabled mask turns the
			 * next run-out into an interrupt storm that starves
			 * the only code able to return descriptors (the main
			 * loop).  A ring the switch has given up on, with no
			 * frame for the CPU either, is rebuilt by
			 * swNic_rx_resync() from eth_poll().
			 */
			g_rx_runout++;
			rt_inc(RT_RUNOUT_CNT);
			rt_set(RT_RUNOUT_IMR, REG32(CPUIIMR));
			REG32(CPUIISR) = RX_RUNOUT_IP_ALL;
		}
		return ret;
	} else
		return -1;
}

/**
 * swNic_rx_count - Frames taken from the RX ring since init
 *
 * eth_poll() compares two readings to tell a ring that is still moving
 * from one the switch has given up on.
 */
uint32 swNic_rx_count(void) { return (uint32)rxPktCounter; }

/**
 * swNic_rx_resync - Rebuild both rings after the switch lost the RX ring
 *
 * Called from the main loop when the run-out status stays asserted across
 * several polls that found nothing to receive: the switch's RX pointer no
 * longer matches the CPU's index.  Stops the DMA engines, hands every RX
 * descriptor back to the switch, drops whatever the TX ring still holds
 * (an ACK the client will ask for again), reprograms the ring bases and
 * restarts — TRXRDY puts the switch back at descriptor 0 of every ring, so
 * the CPU indexes are reset in lockstep.  Same recovery as the kernel
 * driver's ring resync.
 */
void swNic_rx_resync(void)
{
	uint32 i, j, k = 0;
	struct pktHdr *pPkthdr;
	struct mBuf *pMbuf;

	g_rx_resync++;

	REG32(CPUICR) &= ~(TXCMD | RXCMD);
	REG32(SIRR) &= ~TRXRDY;

	for (i = 0; i < txPkthdrRingCnt[0]; i++)
		txPkthdrRing[0][i] &= ~DESC_OWNED_BIT; /* RISC owned */
	currTxPkthdrDescIndex = 0;
	txPktDoneDescIndex = 0;

	for (i = 0; i < RTL865X_SWNIC_RXRING_MAX_PKTDESC; i++) {
		for (j = 0; j < rxPkthdrRingCnt[i]; j++) {
			pPkthdr = (struct pktHdr *)(rxPkthdrRing[i][j] &
						    ~(DESC_OWNED_BIT | DESC_WRAP));
			pMbuf = pPkthdr->ph_mbuf;
			pPkthdr->ph_len = 0;
			pMbuf->m_len = 0;
			pMbuf->m_data = pMbuf->m_extbuf;
			rxPkthdrRing[i][j] = (uint32)pPkthdr | DESC_SWCORE_OWNED;
			rxMbufRing[k] = (uint32)pMbuf | DESC_SWCORE_OWNED;
			k++;
		}
		if (rxPkthdrRingCnt[i] != 0)
			rxPkthdrRing[i][rxPkthdrRingCnt[i] - 1] |= DESC_WRAP;
	}
	rxMbufRing[rxMbufRingCnt - 1] |= DESC_WRAP;
	currRxPkthdrDescIndex = 0;
	currRxMbufDescIndex = 0;
	rxFirstTime = 1;

	REG32(CPUTPDCR0) = (uint32)txPkthdrRing[0];
	REG32(CPURPDCR0) = (uint32)rxPkthdrRing[0];
	REG32(CPURPDCR1) = (uint32)rxPkthdrRing[1];
	REG32(CPURPDCR2) = (uint32)rxPkthdrRing[2];
	REG32(CPURPDCR3) = (uint32)rxPkthdrRing[3];
	REG32(CPURPDCR4) = (uint32)rxPkthdrRing[4];
	REG32(CPURPDCR5) = (uint32)rxPkthdrRing[5];
	REG32(CPURMDCR0) = (uint32)rxMbufRing;

	REG32(CPUIISR) = RX_RUNOUT_IP_ALL;
	REG32(CPUICR) = TXCMD | RXCMD | BUSBURST_32WORDS | MBUF_2048BYTES;
	REG32(SIRR) |= TRXRDY;
}

/*
 * One transmit buffer per TX descriptor of ring 0.  A descriptor is handed
 * to the switch with its own buffer and only reused once the switch has
 * returned it (RISC-owned again), so back-to-back sends — the last-block
 * ACK immediately followed by the UDP notification, or an ARP reply racing
 * an ACK — can no longer overwrite a frame the switch is still reading.
 */
#define TX_BUF_COUNT 4
#define TX_BUF_SIZE 1600
static uint8 txbuf[TX_BUF_COUNT][TX_BUF_SIZE] __attribute__((aligned(4)));

/* Bound on the spin for a descriptor to come back from the switch. */
#define TX_WAIT_LOOPS 2000000

/**
 * swNic_send - Transmit one packet through the switch
 * @output: pointer to the packet data
 * @len: packet length in bytes
 *
 * Copies the packet to the descriptor's own uncached buffer, sets up the
 * TX descriptor, and triggers transmission.  Pads packets shorter than
 * 60 bytes.  Does not wait for the frame to leave; see swNic_wait_tx_idle().
 *
 * Return: 0 on success, -1 if the TX ring stayed full
 */
int32 swNic_send(void *output, uint32 len)
{
	struct pktHdr *pPkthdr;
	uint8 *pktbuf_alligned;
	uint32 spins = 0;

	int next_index;
	if ((currTxPkthdrDescIndex + 1) == txPkthdrRingCnt[0])
		next_index = 0;
	else
		next_index = currTxPkthdrDescIndex + 1;
	while (next_index == txPktDoneDescIndex) {
		swNic_txDone();
		if (next_index != txPktDoneDescIndex)
			break;
		if (++spins > TX_WAIT_LOOPS) {
			dprintf("Tx Desc full!\n");
			return -1;
		}
	}

	if (len > TX_BUF_SIZE)
		len = TX_BUF_SIZE;
	pktbuf_alligned =
	    (uint8 *)(((uint32)txbuf[currTxPkthdrDescIndex % TX_BUF_COUNT] &
		       0x1fffffff) |
		      0xa0000000);
	/* Copy Packet Content */
	memcpy(pktbuf_alligned, output, len);

	ASSERT_CSP(((int32)txPkthdrRing[0][currTxPkthdrDescIndex] &
		    DESC_OWNED_BIT) == DESC_RISC_OWNED);

	/* Fetch packet header from Tx ring */
	pPkthdr =
	    (struct pktHdr *)((int32)txPkthdrRing[0][currTxPkthdrDescIndex] &
			      ~(DESC_OWNED_BIT | DESC_WRAP));

	/* Pad small packets and add CRC */
	if (len < 60)
		pPkthdr->ph_len = 64;
	else
		pPkthdr->ph_len = len + 4;
	pPkthdr->ph_mbuf->m_len = pPkthdr->ph_len;
	pPkthdr->ph_mbuf->m_extsize = pPkthdr->ph_len;

	/* Set cluster pointer to buffer */
	pPkthdr->ph_mbuf->m_data = pktbuf_alligned;
	pPkthdr->ph_mbuf->m_extbuf = pktbuf_alligned;
	pPkthdr->ph_ptpPkt = 0;

	/* Set destination port */
	pPkthdr->ph_portlist = ALL_PORT_MASK;
	/* Give descriptor to switch core */
	txPkthdrRing[0][currTxPkthdrDescIndex] |= DESC_SWCORE_OWNED;

	/* Set TXFD bit to start send */
	REG32(CPUICR) |= TXFD;
	txPktCounter++;

	currTxPkthdrDescIndex = next_index;
	return 0;
}

/**
 * swNic_txDone - Reclaim completed TX descriptors
 *
 * Walks the TX done index forward, freeing descriptors whose
 * ownership has been returned by the switch ASIC.
 */
void swNic_txDone(void)
{
	while (txPktDoneDescIndex != currTxPkthdrDescIndex) {
		if ((*(volatile uint32 *)&txPkthdrRing[0][txPktDoneDescIndex] &
		     DESC_OWNED_BIT) == DESC_RISC_OWNED) {
			if (++txPktDoneDescIndex == txPkthdrRingCnt[0])
				txPktDoneDescIndex = 0;
		} else
			break;
	}
}

/**
 * swNic_wait_tx_idle - Wait until the switch has taken every queued frame
 *
 * Returns when all TX descriptors are back in RISC ownership, or after a
 * bounded spin.  Ownership means the switch has read the buffer; the
 * frame may still sit in the egress queue for a short while.
 */
void swNic_wait_tx_idle(void)
{
	uint32 spins = 0;

	for (;;) {
		swNic_txDone();
		if (txPktDoneDescIndex == currTxPkthdrDescIndex)
			return;
		if (++spins > TX_WAIT_LOOPS)
			return;
	}
}

/**
 * swNic_init - Initialize NIC descriptor rings and DMA
 * @userNeedRxPkthdrRingCnt: RX packet header ring sizes (6 rings)
 * @userNeedRxMbufRingCnt: number of RX mbuf descriptors
 * @userNeedTxPkthdrRingCnt: TX packet header ring sizes (4 rings)
 * @clusterSize: size of each receive cluster buffer
 *
 * Allocates and initializes all descriptor rings, packet headers,
 * mbufs, and cluster buffers.  Programs the ASIC's DMA registers
 * and enables RX/TX.
 *
 * Return: SUCCESS on success, EINVAL if counts are zero
 */
/*************************************************************************
 *   FUNCTION
 *       swNic_init
 *
 *   DESCRIPTION
 *       This function initializes descriptors and data structures.
 *
 *   INPUTS
 *       userNeedRxPkthdrRingCnt[RTL865X_SWNIC_RXRING_MAX_PKTDESC] :
 *          Number of Rx pkthdr descriptors of each ring.
 *       userNeedRxMbufRingCnt :
 *          Number of Tx mbuf descriptors.
 *       userNeedTxPkthdrRingCnt[RTL865X_SWNIC_TXRING_MAX_PKTDESC] :
 *          Number of Tx pkthdr descriptors of each ring.
 *       clusterSize :
 *          Size of cluster.
 *
 *   OUTPUTS
 *       Status.
 *************************************************************************/

int32 swNic_init(
    uint32 userNeedRxPkthdrRingCnt[RTL865X_SWNIC_RXRING_MAX_PKTDESC],
    uint32 userNeedRxMbufRingCnt,
    uint32 userNeedTxPkthdrRingCnt[RTL865X_SWNIC_TXRING_MAX_PKTDESC],
    uint32 clusterSize)
{
	uint32 i, j, k;
	uint32 totalRxPkthdrRingCnt = 0, totalTxPkthdrRingCnt = 0;
	struct pktHdr *pPkthdrList;
	struct mBuf *pMbufList;
	uint8 *pClusterList;
	struct pktHdr *pPkthdr;
	struct mBuf *pMbuf;

	/* Cluster size is always 2048 */
	size_of_cluster = 2048;

	/* Allocate Rx descriptors of rings */
	for (i = 0; i < RTL865X_SWNIC_RXRING_MAX_PKTDESC; i++) {
		rxPkthdrRingCnt[i] = userNeedRxPkthdrRingCnt[i];
		if (rxPkthdrRingCnt[i] == 0)
			continue;

		rxPkthdrRing[i] = (uint32 *)UNCACHED_MALLOC(rxPkthdrRingCnt[i] *
							    sizeof(uint32));
		ASSERT_CSP((uint32)rxPkthdrRing[i] & 0x0fffffff);
		memset(rxPkthdrRing[i], 0, rxPkthdrRingCnt[i] * sizeof(uint32));
		totalRxPkthdrRingCnt += rxPkthdrRingCnt[i];
	}

	if (totalRxPkthdrRingCnt == 0)
		return EINVAL;

	/* Allocate Tx descriptors of rings */
	for (i = 0; i < RTL865X_SWNIC_TXRING_MAX_PKTDESC; i++) {
		txPkthdrRingCnt[i] = userNeedTxPkthdrRingCnt[i];

		if (txPkthdrRingCnt[i] == 0)
			continue;

		txPkthdrRing[i] = (uint32 *)UNCACHED_MALLOC(txPkthdrRingCnt[i] *
							    sizeof(uint32));

		ASSERT_CSP((uint32)txPkthdrRing[i] & 0x0fffffff);
		memset(txPkthdrRing[i], 0,
		       (txPkthdrRingCnt[i] * sizeof(uint32)));
		totalTxPkthdrRingCnt += txPkthdrRingCnt[i];
	}

	if (totalTxPkthdrRingCnt == 0)
		return EINVAL;

	/* Allocate MBuf descriptors of rings */
	rxMbufRingCnt = userNeedRxMbufRingCnt;

	if (userNeedRxMbufRingCnt == 0)
		return EINVAL;

	rxMbufRing =
	    (uint32 *)UNCACHED_MALLOC(userNeedRxMbufRingCnt * sizeof(uint32));
	ASSERT_CSP((uint32)rxMbufRing & 0x0fffffff);
	memset(rxMbufRing, 0, userNeedRxMbufRingCnt * sizeof(uint32));
	/* Allocate pkthdr */
	pPkthdrList = (struct pktHdr *)UNCACHED_MALLOC(
	    (totalRxPkthdrRingCnt + totalTxPkthdrRingCnt) *
	    sizeof(struct pktHdr));
	ASSERT_CSP((uint32)pPkthdrList & 0x0fffffff);
	memset(pPkthdrList, 0,
	       (totalRxPkthdrRingCnt + totalTxPkthdrRingCnt) *
		   sizeof(struct pktHdr));
	/* Allocate mbufs */
	pMbufList = (struct mBuf *)UNCACHED_MALLOC(
	    (rxMbufRingCnt + totalTxPkthdrRingCnt) * sizeof(struct mBuf));
	ASSERT_CSP((uint32)pMbufList & 0x0fffffff);
	memset(pMbufList, 0,
	       ((rxMbufRingCnt + totalTxPkthdrRingCnt) * sizeof(struct mBuf)));
	/* Allocate clusters */
	pClusterList = (uint8 *)UNCACHED_MALLOC(
	    rxMbufRingCnt * size_of_cluster + 8 - 1 + 2 * rxMbufRingCnt);
	ASSERT_CSP((uint32)pClusterList & 0x0fffffff);
	memset(pClusterList, 0,
	       (rxMbufRingCnt * size_of_cluster + 8 - 1 + 2 * rxMbufRingCnt));
	pClusterList = (uint8 *)(((uint32)pClusterList + 8 - 1) & ~(8 - 1));

	/* Initialize interrupt statistics counter */
	rxPktCounter = txPktCounter = 0;

	/* Initialize index of Tx pkthdr descriptor */
	currTxPkthdrDescIndex = 0;
	txPktDoneDescIndex = 0;

	/* Initialize Tx packet header descriptors */
	for (i = 0; i < RTL865X_SWNIC_TXRING_MAX_PKTDESC; i++) {
		for (j = 0; j < txPkthdrRingCnt[i]; j++) {
			/* Dequeue pkthdr and mbuf */
			pPkthdr = pPkthdrList++;
			pMbuf = pMbufList++;

			memset((void *)pPkthdr, 0, sizeof(struct pktHdr));
			memset((void *)pMbuf, 0, sizeof(struct mBuf));

			pPkthdr->ph_mbuf = pMbuf;
			pPkthdr->ph_len = 0;
			pPkthdr->ph_flags = PKTHDR_USED | PKT_OUTGOING;
			pPkthdr->ph_type = PKTHDR_ETHERNET;
			pPkthdr->ph_portlist = 0;

			pMbuf->m_next = NULL;
			pMbuf->m_pkthdr = pPkthdr;
			pMbuf->m_flags =
			    MBUF_USED | MBUF_EXT | MBUF_PKTHDR | MBUF_EOR;
			pMbuf->m_data = NULL;
			pMbuf->m_extbuf = NULL;
			pMbuf->m_extsize = 0;

			txPkthdrRing[i][j] = (int32)pPkthdr | DESC_RISC_OWNED;
		}

		/* Set wrap bit of the last descriptor */
		if (txPkthdrRingCnt[i] != 0) {
			txPkthdrRing[i][txPkthdrRingCnt[i] - 1] |= DESC_WRAP;
		}
	}

	/* Fill Tx packet header FDP */
	REG32(CPUTPDCR0) = (uint32)txPkthdrRing[0];
	REG32(CPUTPDCR1) = (uint32)txPkthdrRing[1];
	REG32(CPUTPDCR2) = (uint32)txPkthdrRing[2];
	REG32(CPUTPDCR3) = (uint32)txPkthdrRing[3];

	/* Initialize index of current Rx pkthdr descriptor */
	currRxPkthdrDescIndex = 0;

	/* Initialize index of current Rx Mbuf descriptor */
	currRxMbufDescIndex = 0;

	/* Initialize Rx packet header descriptors */
	k = 0;

	for (i = 0; i < RTL865X_SWNIC_RXRING_MAX_PKTDESC; i++) {
		for (j = 0; j < rxPkthdrRingCnt[i]; j++) {
			/* Dequeue pkthdr and mbuf */
			pPkthdr = pPkthdrList++;
			pMbuf = pMbufList++;

			memset((void *)pPkthdr, 0, sizeof(struct pktHdr));
			memset((void *)pMbuf, 0, sizeof(struct mBuf));

			/* Setup pkthdr and mbuf */
			pPkthdr->ph_mbuf = pMbuf;
			pPkthdr->ph_len = 0;
			pPkthdr->ph_flags = PKTHDR_USED | PKT_INCOMING;
			pPkthdr->ph_type = PKTHDR_ETHERNET;
			pPkthdr->ph_portlist = 0;
			pMbuf->m_next = NULL;
			pMbuf->m_pkthdr = pPkthdr;
			pMbuf->m_len = 0;
			pMbuf->m_flags =
			    MBUF_USED | MBUF_EXT | MBUF_PKTHDR | MBUF_EOR;
			pMbuf->m_data = NULL;
			pMbuf->m_extsize = size_of_cluster;
			/*offset 2 bytes for 4 bytes align of ip packet*/
			pMbuf->m_data = pMbuf->m_extbuf = (pClusterList + 2);
			pClusterList += size_of_cluster;

			/* Setup descriptors */
			rxPkthdrRing[i][j] = (int32)pPkthdr | DESC_SWCORE_OWNED;
			rxMbufRing[k++] = (int32)pMbuf | DESC_SWCORE_OWNED;
		}

		/* Set wrap bit of the last descriptor */
		if (rxPkthdrRingCnt[i] != 0)
			rxPkthdrRing[i][rxPkthdrRingCnt[i] - 1] |= DESC_WRAP;
	}

	rxMbufRing[rxMbufRingCnt - 1] |= DESC_WRAP;

	/* Fill Rx packet header FDP */
	REG32(CPURPDCR0) = (uint32)rxPkthdrRing[0];
	REG32(CPURPDCR1) = (uint32)rxPkthdrRing[1];
	REG32(CPURPDCR2) = (uint32)rxPkthdrRing[2];
	REG32(CPURPDCR3) = (uint32)rxPkthdrRing[3];
	REG32(CPURPDCR4) = (uint32)rxPkthdrRing[4];
	REG32(CPURPDCR5) = (uint32)rxPkthdrRing[5];

	REG32(CPURMDCR0) = (uint32)rxMbufRing;

	/* Enable Rx & Tx. Config bus burst size and mbuf size. */
	REG32(CPUICR) = TXCMD | RXCMD | BUSBURST_32WORDS | MBUF_2048BYTES;

	REG32(CPUIIMR) = RX_DONE_IE_ALL | TX_DONE_IE_ALL;

	REG32(MDCIOCR) = 0x96181441; // enable Giga port 8211B LED

	return SUCCESS;
}
