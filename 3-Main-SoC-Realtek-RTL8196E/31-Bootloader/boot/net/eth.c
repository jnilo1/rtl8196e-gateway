// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * eth.c - Ethernet interface driver
 *
 * RTL8196E stage-2 bootloader
 *
 * The switch interrupt does nothing but acknowledge the status bits: the
 * receive descriptors are drained by eth_poll(), which the console reader
 * calls from the main loop while it waits for a keystroke.  Everything the
 * TFTP server does — including a multi-minute flash write — therefore runs
 * with interrupts enabled, on the main stack, with the timer alive.
 *
 * Copyright (c) 2009-2020 Realtek Semiconductor Corp.
 * Copyright (c) 2024-2026 J. Nilo
 */

#include "boot_irq.h"
#include <stdlib.h>
#include "boot_common.h"
#include "boot_soc.h"
#include "boot_net.h"
#include <rtl_types.h>
#include "swcore_regs.h"
#include "swcore.h"
#include "ramtest_trace.h"

#ifdef RAMTEST_TRACE
extern unsigned int g_spurious_irq;
int get_timer_jiffies(void);
#endif

#define BUF_SIZE 1600 /* one Ethernet frame plus headroom */

/*
 * Fixed, locally administered MAC.  Bytes 1..4 are overwritten with the
 * server IP by tftpd_entry() and the IPCONFIG command, so the MAC follows
 * the address the host talks to.
 */
char eth0_mac[6] = {0x56, 0xaa, 0xa5, 0x5a, 0x7d, 0xe8};

static unsigned char ETH0_tx_buf[BUF_SIZE];
static int ETH0_IRQ = 15;
static void eth_interrupt(int irq, void *dev_id, struct pt_regs *regs);
static struct irqaction irq_eth15 = {eth_interrupt, 0, 15, "eth0", NULL, NULL};

/*
 * Consecutive eth_poll() passes that received nothing while the run-out
 * status stayed asserted before the rings are rebuilt.  The status is
 * level: with the CPU holding at most one descriptor, a stalled switch
 * and an empty ring cannot both be true unless the switch's pointer has
 * left the ring the CPU indexes.
 */
#define RX_RUNOUT_IDLE_POLLS 3
static unsigned int rx_runout_idle;

#ifdef RAMTEST_TRACE
/* Probe: EW it to 1 (address in boot.nm) and the next poll rebuilds the rings. */
unsigned int g_rx_resync_request;
#endif

static void eth_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	unsigned int status = REG32(CPUIISR);

	REG32(CPUIISR) = status; /* W1C */
	/*
	 * A run-out cannot be serviced here (descriptors are returned from
	 * the main loop) and its status stays set while the switch is
	 * stalled: acknowledging it only re-raises the line.  Its mask is
	 * never enabled; should it ever be, mask it before the storm starts.
	 */
	if (status & RX_RUNOUT_IP_ALL)
		REG32(CPUIIMR) &= ~PKTHDR_DESC_RUNOUT_IE_ALL;
	rt_inc(RT_ISR_CNT);
	rt_set(RT_ISR_STATUS, status);
}

/**
 * eth_poll - Hand every received frame to the TFTP server
 *
 * Runs from the main loop.  Cheap when idle (one descriptor read), so it
 * is called unconditionally rather than gated on the interrupt flag.
 */
void eth_poll(void)
{
	unsigned int before = swNic_rx_count();

	nic.packetlen = 0;
	rt_wdt_kick();
	rt_set(RT_PHASE, RT_PHASE_IDLE);
#ifdef RAMTEST_TRACE
	rt_set(RT_JIFFIES, get_timer_jiffies());
	rt_set(RT_SPURIOUS, g_spurious_irq);
#endif
	while (swNic_receive((void **)&nic.packet, &nic.packetlen) == 0) {
		swNic_txDone();
		rt_set(RT_PHASE, RT_PHASE_FRAME);
		kick_tftpd();
		nic.packetlen = 0;
		rt_wdt_kick(); /* one frame handled: the main loop is alive */
	}
	swNic_txDone();

	if (REG32(CPUIISR) & RX_RUNOUT_IP_ALL) {
		REG32(CPUIISR) = RX_RUNOUT_IP_ALL;
		if (swNic_rx_count() != before) {
			rx_runout_idle = 0; /* the ring moves: a plain run-out */
		} else if (++rx_runout_idle >= RX_RUNOUT_IDLE_POLLS) {
			rx_runout_idle = 0;
			swNic_rx_resync();
		}
	} else {
		rx_runout_idle = 0;
	}
#ifdef RAMTEST_TRACE
	if (g_rx_resync_request) {
		g_rx_resync_request = 0;
		swNic_rx_resync();
	}
#endif
}

/**
 * eth_startup - Initialize the Ethernet subsystem for TFTP recovery
 * @etherport: port number (unused, always 0)
 *
 * Initializes the switch core and NIC descriptor rings, creates the
 * VLAN and network interface, and registers the switch interrupt.
 */
int eth_startup(int etherport)
{
	/* avoid download bin checksum error */
	uint32 rx[6] = {4, 0, 0, 0, 0, 0};
	uint32 tx[4] = {4, 2, 2, 2};
	rtl_vlan_param_t vp;
	rtl_netif_param_t np;
	int32 ret;

	if (swCore_init()) {
		dprintf("\nSwitch core initialization failed!\n");
		return -1;
	}

	/* Initialize NIC module */
	if (swNic_init(rx, 4, tx, MBUF_LEN)) {
		dprintf("\nSwitch nic initialization failed!\n");
		return -1;
	}

	/* Create Netif */
	memset((void *)&np, 0, sizeof(rtl_netif_param_t));
	np.vid = 8;
	np.valid = 1;
	np.enableRoute = 0;
	np.inAclEnd = 0;
	np.inAclStart = 0;
	np.outAclEnd = 0;
	np.outAclStart = 0;
	memcpy(&np.gMac, &eth0_mac[0], 6);
	np.macAddrNumber = 1;
	np.mtu = 1500;
	ret = swCore_netifCreate(0, &np);
	if (ret != 0) {
		printf("Creating intif fails:%d\n", ret);
		return -1;
	}

	/* Create vlan */
	memset((void *)&vp, 0, sizeof(rtl_vlan_param_t));
	vp.egressUntag = ALL_PORT_MASK;
	vp.memberPort = ALL_PORT_MASK;
	ret = vlanTable_create(8, &vp);
	if (ret != 0) {
		printf("Creating vlan fails:%d\n", ret);
		return -1;
	}

	/* Set interrupt routing register */
	REG32(IRR1_REG) |= (3 << 28);

	request_IRQ(ETH0_IRQ, &irq_eth15, NULL);
	return 0;
}

/**
 * prepare_txpkt - Build an Ethernet frame around @data and send it
 * @etherport: unused
 * @type: EtherType (host order)
 * @destaddr: destination MAC
 * @data: payload
 * @len: payload length
 */
void prepare_txpkt(int etherport, unsigned short type, unsigned char *destaddr,
		   unsigned char *data, unsigned short len)
{
	unsigned char *tx_buffer = ETH0_tx_buf;
	unsigned short nstype;
	int Length = len;

	if (Length > BUF_SIZE - ETH_HLEN)
		Length = BUF_SIZE - ETH_HLEN;

	memcpy(tx_buffer, destaddr, 6);

	/*Source Address*/
	memcpy(tx_buffer + 6, eth0_mac, 6);

	/*Payload type*/
	nstype = htons(type);
	memcpy(tx_buffer + 12, (unsigned char *)&nstype, 2);

	/*Payload */
	memcpy(tx_buffer + 14, (unsigned char *)data, Length);
	Length += 14;

	swNic_send(tx_buffer, Length);
}
