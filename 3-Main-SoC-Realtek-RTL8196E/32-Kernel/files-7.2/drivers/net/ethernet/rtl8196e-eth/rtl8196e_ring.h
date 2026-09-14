/* SPDX-License-Identifier: GPL-2.0 */
/*
 * rtl8196e_ring.h - Public API for the RTL8196E TX/RX descriptor ring module.
 */
#ifndef RTL8196E_RING_H
#define RTL8196E_RING_H

#include <linux/types.h>
#include <linux/netdevice.h>
#include "rtl8196e_desc.h"

struct rtl8196e_ring;

struct rtl8196e_rx_buf {
	struct sk_buff *skb;
};

/*
 * Ring anomaly counters. Incremented at the driver's defensive-check
 * sites, exposed read-only via `ethtool -S eth0`. They stay at zero in
 * nominal flow; any growth is a concrete anomaly. No per-packet logging.
 */
struct rtl8196e_ring_diag {
	u32 rx_wild_pkthdr;	/* RX pkthdr ptr outside the descriptor pool */
	u32 rx_wild_mbuf;	/* RX mbuf ptr outside the RX mbuf range */
	u32 rx_bad_len;		/* RX ph_len outside [ETH_ZLEN, buf_size] */
	u32 rx_no_skb;		/* RX slot had no shadow skb */
	u32 rx_alloc_fail;	/* RX replacement skb allocation failed */
	u32 rx_rearm_badidx;	/* RX rearm mbuf index outside the mbuf ring */
	u32 rx_mbuf_no_shadow;	/* HW mbuf index has no rx_bufs shadow skb */
	u32 rx_pkthdr_mbuf_skew;/* RX mbuf_index != rx_idx: switch paired a skewed mbuf
				 * (saturation-only per the rx_poll note; switch-desync probe) */
	u32 tx_bad_args;	/* TX submit with null/zero arguments */
	u32 tx_bad_len;		/* TX submit length over 1518 */
	u32 tx_ring_full;	/* TX submit found the ring full */
	u32 tx_reclaim_no_skb;	/* TX reclaim of a completed desc with no skb */
	u32 tx_bad_pkthdr;	/* TX pkthdr ptr outside the TX pool */
	u32 tx_bad_mbuf;	/* TX mbuf ptr outside the TX pool */
	u32 tx_defer_queued;	/* TX skbs deferred to the NAPI-poll free path */
	u32 tx_defer_direct;	/* TX skbs freed directly (defer list at cap) */
};

struct rtl8196e_ring *rtl8196e_ring_create(unsigned int tx_cnt,
					   unsigned int rx_cnt,
					   unsigned int rx_mbuf_cnt,
					   size_t buf_size);
void rtl8196e_ring_destroy(struct rtl8196e_ring *ring);

void *rtl8196e_ring_tx_desc_base(struct rtl8196e_ring *ring);
void *rtl8196e_ring_rx_pkthdr_base(struct rtl8196e_ring *ring);
void *rtl8196e_ring_rx_mbuf_base(struct rtl8196e_ring *ring);

int rtl8196e_ring_tx_submit(struct rtl8196e_ring *ring, void *skb,
				   void *data, unsigned int len,
				   u16 vid, u16 portlist,
				   bool *was_empty);

int rtl8196e_ring_tx_reclaim(struct rtl8196e_ring *ring,
				    unsigned int *pkts,
				    unsigned int *bytes,
				    int napi_budget,
				    struct sk_buff_head *defer);

int rtl8196e_ring_rx_poll(struct rtl8196e_ring *ring, int budget,
				 struct napi_struct *napi,
				 struct net_device *dev, int *delivered_out,
				 unsigned int *delivered_bytes_out);

int rtl8196e_ring_tx_free_count(struct rtl8196e_ring *ring);
void rtl8196e_ring_panic_snapshot(struct rtl8196e_ring *ring,
				  u32 *rx_idx, u32 *rx_desc,
				  u32 *tx_prod, u32 *tx_cons,
				  u32 *tx_free, u32 *tx_desc);
bool rtl8196e_ring_tx_done_stuck(struct rtl8196e_ring *ring, u32 *idx);

void rtl8196e_ring_kick_tx(struct rtl8196e_ring *ring, bool was_empty);
void rtl8196e_ring_kick_drain(struct rtl8196e_ring *ring);
void rtl8196e_ring_kick_stats_get(struct rtl8196e_ring *ring,
				  u32 *cold, u32 *thresh, u32 *drain, u32 *total);
void rtl8196e_ring_diag_get(struct rtl8196e_ring *ring,
			    struct rtl8196e_ring_diag *out);
extern unsigned int rtl8196e_kick_threshold;
unsigned int rtl8196e_ring_tx_reset(struct rtl8196e_ring *ring);
void rtl8196e_ring_rx_reset(struct rtl8196e_ring *ring);

#endif /* RTL8196E_RING_H */
