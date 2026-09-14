// SPDX-License-Identifier: GPL-2.0
/*
 * rtl8196e_ring.c - TX/RX descriptor ring management.
 *
 * Allocates and manages two RX rings (pkthdr + mbuf) and one TX ring backed by
 * pools of struct rtl_pktHdr and struct rtl_mBuf descriptors in KSEG1 (uncached)
 * memory.  Handles SKB lifecycle, DMA cache coherency, and NAPI buffer recycling.
 */
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/etherdevice.h>
#include <linux/if_ether.h>
#include <linux/if_vlan.h>
#include <linux/ip.h>
#include <linux/skbuff.h>
#include <linux/kernel.h>
#include <linux/build_bug.h>
#include <linux/stddef.h>
#include <linux/moduleparam.h>
#include <net/ip.h>
#include <asm/io.h>
#include <asm/mach-realtek/imem.h>
#include "rtl8196e_ring.h"
#include "rtl8196e_regs.h"

/*
 * Compile-time guard on the descriptor ABI shared with the switch ASIC.
 * The hardware writes ph_len/ph_flags/ph_reason and reads m_data/m_extbuf
 * at fixed byte offsets in main memory. Any silent re-ordering or padding
 * change (toolchain bump, struct edit) would corrupt RX/TX in ways that
 * are very expensive to diagnose on this single-core MIPS-BE platform.
 *
 * Sizes are checked because the descriptor ownership bits (LSB and bit 1)
 * live in the low two bits of the pool address — descriptor sizes must
 * therefore be at least 4-byte aligned.
 */
static inline void rtl8196e_desc_layout_check(void)
{
	BUILD_BUG_ON(sizeof(struct rtl_pktHdr) != 20);
	BUILD_BUG_ON(sizeof(struct rtl_mBuf) != 32);
	BUILD_BUG_ON(sizeof(struct rtl_pktHdr) % 4 != 0);
	BUILD_BUG_ON(sizeof(struct rtl_mBuf) % 4 != 0);

	/* rtl_pktHdr: pointer-aligned head + ph_len at +4 */
	BUILD_BUG_ON(offsetof(struct rtl_pktHdr, ph_len) != 4);

	/* rtl_mBuf: m_pkthdr/m_data offsets read by both CPU and ASIC */
	BUILD_BUG_ON(offsetof(struct rtl_mBuf, m_pkthdr) != 4);
	BUILD_BUG_ON(offsetof(struct rtl_mBuf, m_data) != 12);
	BUILD_BUG_ON(offsetof(struct rtl_mBuf, m_extbuf) != 16);
}

/*
 * Give each pkthdr its own cache line. At the native 20-byte stride two
 * neighbours share a 32-byte L1 line; the RX rearm writes ph_len/ph_flags
 * cached and then wback_inv(ph, 20) writes the whole line back, which would
 * clobber a switch DMA update to the neighbour landing in the fill->wback
 * window (observed as a spurious rx_bad_len drop). Padding to the cache line
 * removes the shared line without changing the 20-byte HW descriptor ABI.
 * The mbuf is already 32 bytes, so only the pkthdr pool needs this.
 */
struct rtl8196e_pkthdr_slot {
	struct rtl_pktHdr ph;
} __aligned(L1_CACHE_BYTES);

/* Single-producer (xmit) / single-consumer (NAPI) ring.
 * tx_prod and tx_cons are accessed from different contexts;
 * use READ_ONCE/WRITE_ONCE to prevent compiler tearing. */
struct rtl8196e_ring {
	u32 *tx_ring;
	u32 *rx_pkthdr_ring;
	u32 *rx_mbuf_ring;
	void *tx_ring_alloc;
	void *rx_pkthdr_ring_alloc;
	void *rx_mbuf_ring_alloc;
	struct rtl8196e_pkthdr_slot *pkthdr_pool;
	struct rtl_mBuf *mbuf_pool;
	void *pkthdr_alloc;
	void *mbuf_alloc;
	struct rtl_mBuf *rx_mbuf_base;
	unsigned int tx_cnt;
	unsigned int rx_cnt;
	unsigned int rx_mbuf_cnt;
	unsigned int tx_prod;
	unsigned int tx_cons;
	unsigned int rx_idx;
	unsigned int rx_debug_once;
	unsigned int rx_debug_bad;
	size_t buf_size;
	struct rtl8196e_rx_buf *rx_bufs;
	/* Coalescing counter for rtl8196e_ring_kick_tx(). */
	unsigned int pending_kicks;
	/* TX kick stats (exposed via ethtool -S). */
	u32 kicks_cold;		/* was_empty fast-path pulses */
	u32 kicks_threshold;	/* threshold-triggered pulses */
	u32 kicks_drain;	/* end-of-NAPI drain pulses */
	u32 kicks_total;	/* sum of the three above */
	/* Ring anomaly counters (exposed via ethtool -S). */
	struct rtl8196e_ring_diag diag;
};

/* Address of the pkthdr living in pool slot @idx. */
static __always_inline struct rtl_pktHdr *rtl8196e_ph(struct rtl8196e_ring *ring,
						      unsigned int idx)
{
	return &ring->pkthdr_pool[idx].ph;
}

/*
 * RX checksum policy. Only unfragmented IPv4 UDP has been characterized on
 * this switch: CSUM_IP_OK and CSUM_TCPUDP_OK together mean that both checksums
 * were verified. Everything else stays CHECKSUM_NONE until its descriptor-bit
 * semantics have been measured with deliberately corrupted traffic. This
 * deliberately trades TCP throughput for the kernel's checksum-integrity
 * contract instead of trusting an unverified hardware indication.
 *
 * @skb->data points at the IP header here because eth_type_trans() pulled the
 * Ethernet header. VLAN-encapsulated traffic therefore takes the safe default.
 */
static __always_inline void rtl8196e_rx_set_csum(struct net_device *dev,
						 struct sk_buff *skb, u16 ph_flags)
{
	const struct iphdr *iph;

	skb->ip_summed = CHECKSUM_NONE;
	if (unlikely(!(dev->features & NETIF_F_RXCSUM)) ||
	    skb->protocol != htons(ETH_P_IP) ||
	    skb->len < sizeof(struct iphdr))
		return;

	iph = (const struct iphdr *)skb->data;
	if (iph->version != 4 || iph->ihl < 5 ||
	    skb->len < (unsigned int)iph->ihl * 4 ||
	    iph->protocol != IPPROTO_UDP || ip_is_fragment(iph))
		return;

	if ((ph_flags & (CSUM_IP_OK | CSUM_TCPUDP_OK)) ==
	    (CSUM_IP_OK | CSUM_TCPUDP_OK)) {
		skb->ip_summed = CHECKSUM_UNNECESSARY;
	}
}

/* Allocate @size bytes, store the original pointer in *orig_out, return KSEG1 address. */
static void *rtl8196e_alloc_uncached(size_t size, void **orig_out)
{
	void *p = kmalloc(size, GFP_KERNEL);
	if (!p)
		return NULL;
	/*
	 * Flush-and-discard the cached alias before the memory is ever
	 * touched through KSEG1: the lines may still sit
	 * dirty in the write-back L1 from the allocation's previous
	 * lifetime (kfree does not flush), and a later eviction would
	 * write that stale data over ring entries maintained through the
	 * uncached alias — corrupt ownership bits or wild descriptor
	 * pointers. One-time cost at ring creation.
	 */
	dma_cache_wback_inv((unsigned long)p, size);
	if (orig_out)
		*orig_out = p;
	return rtl8196e_uncached_addr(p);
}

/* Extract the descriptor pointer from a raw ring entry (strip ownership and wrap bits). */
static __always_inline struct rtl_pktHdr *rtl8196e_desc_ptr(u32 entry)
{
	return (struct rtl_pktHdr *)(entry & ~(RTL8196E_DESC_OWNED_BIT | RTL8196E_DESC_WRAP));
}

/*
 * Pool bounds check. A descriptor pointer read back from a ring entry must
 * land on the exact start of an element inside the pool the driver allocated;
 * anything else is a corrupt entry and dereferencing it would chase a wild or
 * mid-object pointer.
 */
static __always_inline bool rtl8196e_ptr_in_pool(const void *ptr, const void *base,
						 unsigned int count, size_t size)
{
	unsigned long addr = (unsigned long)ptr;
	unsigned long start = (unsigned long)base;
	unsigned long end = start + (unsigned long)count * size;

	/*
	 * Require the pointer to land on an element boundary, not merely inside
	 * the pool: a corrupt ring entry masked to 4-byte alignment could point
	 * mid-descriptor and still be in range. The modulo (by 20 or 32) is a
	 * few cycles at per-packet rates on this CPU — negligible against the
	 * MMIO the callers already do. size is a multiple of 4, so this also
	 * subsumes the old 4-byte alignment check.
	 */
	return addr >= start && addr < end && (addr - start) % size == 0;
}

/* TX pkthdr/mbuf descriptors live in the first tx_cnt slots of each pool. */
static __always_inline bool rtl8196e_tx_pkthdr_valid(struct rtl8196e_ring *ring,
						     const struct rtl_pktHdr *ph)
{
	/* pkthdrs sit at cache-line stride (one per slot); validate against the
	 * slot stride, not sizeof(rtl_pktHdr). ph == &slot.ph, so a valid ph is
	 * slot-aligned. */
	return rtl8196e_ptr_in_pool(ph, ring->pkthdr_pool, ring->tx_cnt,
				    sizeof(*ring->pkthdr_pool));
}

static __always_inline bool rtl8196e_tx_mbuf_valid(struct rtl8196e_ring *ring,
						   const struct rtl_mBuf *mb)
{
	return rtl8196e_ptr_in_pool(mb, ring->mbuf_pool, ring->tx_cnt, sizeof(*mb));
}

/* RX descriptors live in their exact sub-pools after the TX descriptors. */
static __always_inline bool rtl8196e_rx_pkthdr_valid(struct rtl8196e_ring *ring,
						     const struct rtl_pktHdr *ph)
{
	return rtl8196e_ptr_in_pool(ph, rtl8196e_ph(ring, ring->tx_cnt),
				    ring->rx_cnt, sizeof(*ring->pkthdr_pool));
}

static __always_inline bool rtl8196e_rx_mbuf_valid(struct rtl8196e_ring *ring,
						   const struct rtl_mBuf *mb)
{
	return rtl8196e_ptr_in_pool(mb, ring->rx_mbuf_base,
				    ring->rx_mbuf_cnt, sizeof(*mb));
}

/* Allocate and initialise TX/RX descriptor rings with pre-allocated SKB buffers. */
struct rtl8196e_ring *rtl8196e_ring_create(unsigned int tx_cnt,
					   unsigned int rx_cnt,
					   unsigned int rx_mbuf_cnt,
					   size_t buf_size)
{
	struct rtl8196e_ring *ring;
	unsigned int i;
	unsigned int pkthdr_cnt;
	unsigned int mbuf_cnt;
	size_t alloc_size;

	rtl8196e_desc_layout_check();

	ring = kzalloc(sizeof(*ring), GFP_KERNEL);
	if (!ring)
		return NULL;

	/*
	 * Only rx_mbuf_cnt == rx_cnt is initialised: the RX loop sets up one
	 * mbuf per pkthdr and this driver never chains mbufs (buf_size > MTU).
	 * A larger mbuf ring would leave entries [rx_cnt, rx_mbuf_cnt) as kmalloc
	 * garbage yet handed to the switch. Reject any mismatch outright.
	 */
	if (rx_mbuf_cnt != rx_cnt)
		goto err;

	ring->tx_cnt = tx_cnt;
	ring->rx_cnt = rx_cnt;
	ring->rx_mbuf_cnt = rx_mbuf_cnt;
	ring->buf_size = buf_size;

	ring->tx_ring = rtl8196e_alloc_uncached(tx_cnt * sizeof(u32), &ring->tx_ring_alloc);
	ring->rx_pkthdr_ring = rtl8196e_alloc_uncached(rx_cnt * sizeof(u32), &ring->rx_pkthdr_ring_alloc);
	ring->rx_mbuf_ring = rtl8196e_alloc_uncached(rx_mbuf_cnt * sizeof(u32), &ring->rx_mbuf_ring_alloc);

	if (!ring->tx_ring || !ring->rx_pkthdr_ring || !ring->rx_mbuf_ring)
		goto err;

	pkthdr_cnt = tx_cnt + rx_cnt;
	mbuf_cnt = tx_cnt + rx_mbuf_cnt;

	alloc_size = pkthdr_cnt * sizeof(struct rtl8196e_pkthdr_slot) + L1_CACHE_BYTES;
	ring->pkthdr_alloc = kmalloc(alloc_size, GFP_KERNEL);
	if (!ring->pkthdr_alloc)
		goto err;
	ring->pkthdr_pool = (struct rtl8196e_pkthdr_slot *)ALIGN((unsigned long)ring->pkthdr_alloc, L1_CACHE_BYTES);

	alloc_size = mbuf_cnt * sizeof(struct rtl_mBuf) + L1_CACHE_BYTES;
	ring->mbuf_alloc = kmalloc(alloc_size, GFP_KERNEL);
	if (!ring->mbuf_alloc)
		goto err;
	ring->mbuf_pool = (struct rtl_mBuf *)ALIGN((unsigned long)ring->mbuf_alloc, L1_CACHE_BYTES);
	ring->rx_mbuf_base = ring->mbuf_pool + tx_cnt;

	/* Init TX descriptors */
	for (i = 0; i < tx_cnt; i++) {
		struct rtl_pktHdr *ph = rtl8196e_ph(ring, i);
		struct rtl_mBuf *mb = &ring->mbuf_pool[i];

		memset(ph, 0, sizeof(*ph));
		memset(mb, 0, sizeof(*mb));

		ph->ph_mbuf = mb;
		ph->ph_flags = PKTHDR_USED | PKT_OUTGOING;
		ph->ph_type = PKTHDR_ETHERNET;
		ph->ph_portlist = 0;

		mb->m_pkthdr = ph;
		mb->m_flags = MBUF_USED | MBUF_EXT | MBUF_PKTHDR | MBUF_EOR;
		mb->m_data = NULL;
		mb->m_extbuf = NULL;
		mb->m_extsize = 0;
		mb->skb = NULL;

		ring->tx_ring[i] = (u32)ph | RTL8196E_DESC_RISC_OWNED;

		dma_cache_wback_inv((unsigned long)ph, sizeof(*ph));
		dma_cache_wback_inv((unsigned long)mb, sizeof(*mb));
	}
	if (tx_cnt)
		ring->tx_ring[tx_cnt - 1] |= RTL8196E_DESC_WRAP;

	/* Allocate RX buffer shadow array */
	ring->rx_bufs = kcalloc(rx_cnt, sizeof(*ring->rx_bufs), GFP_KERNEL);
	if (!ring->rx_bufs)
		goto err;

	/* Init RX descriptors */
	for (i = 0; i < rx_cnt; i++) {
		struct rtl_pktHdr *ph = rtl8196e_ph(ring, tx_cnt + i);
		struct rtl_mBuf *mb = &ring->mbuf_pool[tx_cnt + i];
		struct sk_buff *skb;

		memset(ph, 0, sizeof(*ph));
		memset(mb, 0, sizeof(*mb));

		ph->ph_mbuf = mb;
		ph->ph_flags = PKTHDR_USED | PKT_INCOMING;
		ph->ph_type = PKTHDR_ETHERNET;
		ph->ph_portlist = 0;

		mb->m_pkthdr = ph;
		mb->m_flags = MBUF_USED | MBUF_EXT | MBUF_PKTHDR | MBUF_EOR;
		mb->m_len = 0;
		mb->m_extsize = buf_size;

		/* Match napi_alloc_skb() layout exactly: NET_SKB_PAD + NET_IP_ALIGN
		 * headroom and buf_size tailroom. netdev_alloc_skb_ip_align()
		 * already reserves both internally; an additional skb_reserve()
		 * would offset skb->data past the rearm-path layout and leave
		 * the trailing cache lines outside the wback span below.
		 */
		skb = netdev_alloc_skb_ip_align(NULL, buf_size);
		if (!skb)
			goto err;

		mb->m_data = skb->data;
		mb->m_extbuf = skb->data;
		mb->skb = NULL;

		ring->rx_bufs[i].skb = skb;

		ring->rx_pkthdr_ring[i] = (u32)ph | RTL8196E_DESC_SWCORE_OWNED;
		ring->rx_mbuf_ring[i] = (u32)mb | RTL8196E_DESC_SWCORE_OWNED;

		dma_cache_wback_inv((unsigned long)skb->head,
				    NET_SKB_PAD + NET_IP_ALIGN + buf_size);
		dma_cache_wback_inv((unsigned long)ph, sizeof(*ph));
		dma_cache_wback_inv((unsigned long)mb, sizeof(*mb));
	}
	if (rx_cnt)
		ring->rx_pkthdr_ring[rx_cnt - 1] |= RTL8196E_DESC_WRAP;
	if (rx_mbuf_cnt)
		ring->rx_mbuf_ring[rx_mbuf_cnt - 1] |= RTL8196E_DESC_WRAP;

	/* Flush descriptor structures */
	dma_cache_wback_inv((unsigned long)ring->pkthdr_pool, pkthdr_cnt * sizeof(struct rtl8196e_pkthdr_slot));
	dma_cache_wback_inv((unsigned long)ring->mbuf_pool, mbuf_cnt * sizeof(struct rtl_mBuf));

	return ring;

err:
	rtl8196e_ring_destroy(ring);
	return NULL;
}

/* Free all ring memory: release in-flight SKBs, descriptor pools, and ring arrays. */
void rtl8196e_ring_destroy(struct rtl8196e_ring *ring)
{
	unsigned int i;

	if (!ring)
		return;

	/* Free TX SKBs */
	if (ring->mbuf_pool) {
		for (i = 0; i < ring->tx_cnt; i++) {
			if (ring->mbuf_pool[i].skb) {
				dev_kfree_skb_any((struct sk_buff *)ring->mbuf_pool[i].skb);
				ring->mbuf_pool[i].skb = NULL;
			}
		}
	}

	/* Free RX SKBs */
	if (ring->rx_bufs) {
		for (i = 0; i < ring->rx_cnt; i++) {
			if (ring->rx_bufs[i].skb)
				dev_kfree_skb_any(ring->rx_bufs[i].skb);
		}
	}
	kfree(ring->rx_bufs);

	kfree(ring->tx_ring_alloc);
	kfree(ring->rx_pkthdr_ring_alloc);
	kfree(ring->rx_mbuf_ring_alloc);
	kfree(ring->pkthdr_alloc);
	kfree(ring->mbuf_alloc);
	kfree(ring);
}

/* Return the KSEG1 base address of the TX descriptor array. */
void *rtl8196e_ring_tx_desc_base(struct rtl8196e_ring *ring)
{
	return ring ? ring->tx_ring : NULL;
}

/* Return the KSEG1 base address of the RX pkthdr descriptor array. */
void *rtl8196e_ring_rx_pkthdr_base(struct rtl8196e_ring *ring)
{
	return ring ? ring->rx_pkthdr_ring : NULL;
}

/* Return the KSEG1 base address of the RX mbuf descriptor array. */
void *rtl8196e_ring_rx_mbuf_base(struct rtl8196e_ring *ring)
{
	return ring ? ring->rx_mbuf_ring : NULL;
}

/* Fill the next TX descriptor with @skb's data and hand ownership to the hardware. */
__iram int rtl8196e_ring_tx_submit(struct rtl8196e_ring *ring, void *skb,
				   void *data, unsigned int len,
				   u16 vid, u16 portlist,
				   bool *was_empty)
{
	unsigned int next;
	struct rtl_pktHdr *ph;
	struct rtl_mBuf *mb;

	if (unlikely(!ring))
		return -EINVAL;
	if (unlikely(!skb || !data || len == 0)) {
		ring->diag.tx_bad_args++;
		return -EINVAL;
	}

	if (len < ETH_ZLEN)
		len = ETH_ZLEN;
	if (unlikely(len > 1518)) {
		ring->diag.tx_bad_len++;
		return -EINVAL;
	}

	next = ring->tx_prod + 1;
	if (next >= ring->tx_cnt)
		next = 0;

	if (unlikely(next == READ_ONCE(ring->tx_cons))) {
		ring->diag.tx_ring_full++;
		return -ENOSPC;
	}

	if (was_empty)
		*was_empty = (ring->tx_prod == ring->tx_cons);

	ph = rtl8196e_desc_ptr(ring->tx_ring[ring->tx_prod]);
	if (unlikely(!rtl8196e_tx_pkthdr_valid(ring, ph))) {
		ring->diag.tx_bad_pkthdr++;
		return -EIO;
	}
	mb = ph->ph_mbuf;
	if (unlikely(!rtl8196e_tx_mbuf_valid(ring, mb))) {
		ring->diag.tx_bad_mbuf++;
		return -EIO;
	}

	mb->m_len = len;
	mb->m_extsize = len;
	mb->m_data = data;
	mb->m_extbuf = data;
	mb->skb = skb;

	ph->ph_len = len;
	ph->ph_vlanId = vid;
	ph->ph_portlist = portlist & 0x3f;
	ph->ph_srcExtPortNum = 0;
	/* Constant at every call site since the bring-up era — owned here. */
	ph->ph_flags = PKTHDR_USED | PKT_OUTGOING;

	/* Flush descriptors (packet data flushed by caller) */
	dma_cache_wback_inv((unsigned long)ph, sizeof(*ph));
	dma_cache_wback_inv((unsigned long)mb, sizeof(*mb));

	/* Hand over to hardware - atomic write preserving WRAP bit */
	wmb();
	ring->tx_ring[ring->tx_prod] = (u32)ph | RTL8196E_DESC_SWCORE_OWNED |
					(ring->tx_ring[ring->tx_prod] & RTL8196E_DESC_WRAP);
	wmb();

	WRITE_ONCE(ring->tx_prod, next);

	return 0;
}

/*
 * Cap on the xmit-context deferred-free list. Bounds the skbs held between
 * two NAPI polls (a poll always runs within RTL8196E_TX_RECLAIM_MS of a
 * queue stop, and on every RX interrupt); past the cap the reclaim falls
 * back to freeing directly, i.e. degrades to the pre-defer behaviour.
 */
#define RTL8196E_TX_DEFER_CAP 64

/*
 * Walk the TX consumer ring, free completed SKBs, return the number of
 * packets reclaimed. When @defer is non-NULL (xmit context, napi_budget 0)
 * completed skbs are queued there instead of freed inline — the NAPI poll
 * drains them through napi_consume_skb() with a real budget. Descriptor
 * slots are released by the cursor advance either way, so deferring the
 * skb free never delays ring availability.
 */
__iram int rtl8196e_ring_tx_reclaim(struct rtl8196e_ring *ring,
				    unsigned int *pkts,
				    unsigned int *bytes,
				    int napi_budget,
				    struct sk_buff_head *defer)
{
	unsigned int done_pkts = 0;
	unsigned int done_bytes = 0;

	if (unlikely(!ring))
		return 0;

	while (ring->tx_cons != READ_ONCE(ring->tx_prod)) {
		u32 entry;
		struct rtl_pktHdr *ph;
		struct rtl_mBuf *mb;
		struct sk_buff *skb;

		/* tx_ring is the uncached KSEG1 alias: the switch's ownership
		 * writeback is visible directly, no cache invalidate needed.
		 * rmb() orders this read before the cached ph/mb reads below. */
		rmb();
		entry = ring->tx_ring[ring->tx_cons];
		if (entry & RTL8196E_DESC_OWNED_BIT)
			break;

		ph = rtl8196e_desc_ptr(entry);
		if (unlikely(!rtl8196e_tx_pkthdr_valid(ring, ph))) {
			ring->diag.tx_bad_pkthdr++;
			break;
		}
		dma_cache_inv((unsigned long)ph, sizeof(*ph));
		mb = ph->ph_mbuf;
		if (unlikely(!rtl8196e_tx_mbuf_valid(ring, mb))) {
			ring->diag.tx_bad_mbuf++;
			break;
		}
		dma_cache_inv((unsigned long)mb, sizeof(*mb));

		skb = (struct sk_buff *)mb->skb;
		if (likely(skb)) {
			done_pkts++;
			done_bytes += skb->len;
			if (defer && skb_queue_len(defer) < RTL8196E_TX_DEFER_CAP) {
				__skb_queue_tail(defer, skb);
				ring->diag.tx_defer_queued++;
			} else {
				napi_consume_skb(skb, napi_budget);
				if (defer)
					ring->diag.tx_defer_direct++;
			}
			mb->skb = NULL;
		} else {
			ring->diag.tx_reclaim_no_skb++;
		}

		{
			unsigned int next_cons = ring->tx_cons + 1;
			if (next_cons >= ring->tx_cnt)
				next_cons = 0;
			WRITE_ONCE(ring->tx_cons, next_cons);
		}
	}

	if (pkts)
		*pkts = done_pkts;
	if (bytes)
		*bytes = done_bytes;

	return done_pkts;
}

/* NAPI RX poll: process up to @budget received packets and hand them to the stack. */
__iram int rtl8196e_ring_rx_poll(struct rtl8196e_ring *ring, int budget,
				 struct napi_struct *napi,
				 struct net_device *dev, int *delivered_out,
				 unsigned int *delivered_bytes_out)
{
	int processed = 0;
	int delivered = 0;
	unsigned int delivered_bytes = 0;

	if (unlikely(!ring))
		return 0;

	while (processed < budget) {
		u32 entry = ring->rx_pkthdr_ring[ring->rx_idx];
		struct rtl_pktHdr *ph;
		struct rtl_mBuf *mb;
		struct rtl8196e_rx_buf *rxb;
		struct sk_buff *skb, *new_skb;
		unsigned int len;
		unsigned int mbuf_index;

		if (entry & RTL8196E_DESC_OWNED_BIT)
			break;
		/*
		 * Bound the poll by descriptors *processed*, not packets delivered.
		 * Every drop path below re-arms and advances rx_idx without
		 * delivering; counting only deliveries (the old work_done) let a
		 * flood of droppable descriptors spin this poll forever: the poll
		 * never returns, CPU pinned in NET_RX softirq -> soft-lockup.
		 * The vendor RX DSR bounds by total iterations (rtl_nic.c:4068).
		 */
		processed++;

		ph = rtl8196e_desc_ptr(entry);
		/* Defense in depth: the ring entry came from HW/uncached memory;
		 * a corrupt entry could carry a pkthdr pointer outside the pool.
		 * Range-check before we dereference (dma_cache_inv + ph->ph_mbuf)
		 * and fall back on the static index->ph mapping. */
		if (unlikely(!rtl8196e_rx_pkthdr_valid(ring, ph))) {
			ring->diag.rx_wild_pkthdr++;
			dev->stats.rx_errors++;
			ph = rtl8196e_ph(ring, ring->tx_cnt + ring->rx_idx);
		}
		dma_cache_inv((unsigned long)ph, sizeof(*ph));
		mb = ph->ph_mbuf;
		/* Defense in depth: HW wrote the descriptor; a silicon or RAM
		 * corruption could plant a wild pointer in ph_mbuf. Fall back
		 * on the static index->mb mapping set at ring_create(). */
		if (unlikely(!rtl8196e_rx_mbuf_valid(ring, mb))) {
			ring->diag.rx_wild_mbuf++;
			dev->stats.rx_errors++;
			mb = &ring->rx_mbuf_base[ring->rx_idx];
		}
		dma_cache_inv((unsigned long)mb, sizeof(*mb));
		mbuf_index = ((unsigned long)mb -
			       (unsigned long)ring->rx_mbuf_base) / sizeof(*mb);

		/*
		 * Switch-desync probe: count how often the switch pairs pkthdr[rx_idx]
		 * with an mbuf whose ring index differs from rx_idx. The note below
		 * says this skew appears only at/near RX saturation; the leading
		 * hypothesis is that sustained skew is what eventually desyncs the
		 * switch RX pointer from rx_idx. Cheap: one compare per consumed
		 * descriptor, no hot-path cost in the common (no-skew) case.
		 */
		if (unlikely(mbuf_index != ring->rx_idx))
			ring->diag.rx_pkthdr_mbuf_skew++;

		/*
		 * Under RX ring saturation the switch links pkthdr[rx_idx] to a
		 * mbuf whose ring index differs from rx_idx (skew appears only
		 * at/near saturation, never in flow-controlled TCP). The shadow
		 * skb backing the DMA'd data is the one bound to *that* mbuf, so
		 * index rx_bufs by the mbuf index, not rx_idx. Guard the lookup:
		 * rx_bufs holds rx_cnt entries and ring_create() enforces
		 * rx_mbuf_cnt == rx_cnt, but keep the bound as defense in depth
		 * against a wild mbuf index.
		 */
		if (unlikely(mbuf_index >= ring->rx_cnt)) {
			ring->diag.rx_mbuf_no_shadow++;
			rxb = NULL;
			goto rearm_drop;
		}
		rxb = &ring->rx_bufs[mbuf_index];
		skb = rxb->skb;
		if (unlikely(!skb)) {
			ring->diag.rx_no_skb++;
			goto rearm_drop;
		}

		len = ph->ph_len;
		/*
		 * Bound RX length by the logical maximum (MTU + L2/VLAN headers,
		 * plus an FCS margin) as well as the raw buffer size — tighter
		 * than buf_size alone, defense in depth behind the switch's own
		 * ingress length limit.
		 */
		if (unlikely(len < ETH_ZLEN ||
			     len > dev->mtu + VLAN_ETH_HLEN + ETH_FCS_LEN ||
			     len > ring->buf_size))
			goto rearm_bad;

		/* Invalidate cache on packet data */
		dma_cache_inv((unsigned long)skb->data, len);

		/* Allocate a fresh SKB for the descriptor (NAPI-optimized) */
		new_skb = napi_alloc_skb(napi, ring->buf_size);
		if (unlikely(!new_skb)) {
			ring->diag.rx_alloc_fail++;
			goto rearm_drop;
		}

		/* Set length on received SKB and hand to stack */
		skb_put(skb, len);
		skb->dev = dev;
		dev->stats.rx_packets++;
		dev->stats.rx_bytes += len;
		skb->protocol = eth_type_trans(skb, dev);
		/* Trust hardware checksums only for characterized IPv4 UDP. */
		rtl8196e_rx_set_csum(dev, skb, ph->ph_flags);
		if (unlikely(ring->rx_debug_once == 0)) {
			ring->rx_debug_once = 1;
			netdev_dbg(dev, "rx first len=%u flags=0x%04x port=0x%02x vid=%u\n",
				    len, ph->ph_flags, ph->ph_portlist, ph->ph_vlanId);
		}

		/* Install fresh SKB in the slot; descriptor field reset
		 * happens at the shared rearm: label below so all paths
		 * (nominal, drop, bad-length) leave HW a canonical view.
		 */
		rxb->skb = new_skb;

		napi_gro_receive(napi, skb);

		delivered++;
		delivered_bytes += len;
		goto rearm;

rearm_drop:
		dev->stats.rx_dropped++;
		goto rearm;

rearm_bad:
		ring->diag.rx_bad_len++;
		dev->stats.rx_errors++;
		dev->stats.rx_length_errors++;
		if (unlikely(ring->rx_debug_bad < 3)) {
			ring->rx_debug_bad++;
			netdev_warn(dev, "rx bad len=%u flags=0x%04x port=0x%02x vid=%u\n",
				    len, ph->ph_flags, ph->ph_portlist, ph->ph_vlanId);
		}

rearm:
		/* Re-arm from a canonical descriptor state on every path
		 * (nominal, drop, bad-length). Hardware must not see stale
		 * ph_len / ph_flags or mb buffer fields from the frame we
		 * just discarded or handed up the stack.
		 *
		 * If rxb->skb is NULL (defensive: should not happen in
		 * nominal flow), leave m_data / m_extbuf untouched so we do
		 * not plant a wild pointer. The descriptor still gets handed
		 * back to the switch — losing one slot is preferable to
		 * stalling the ring.
		 */
		ph->ph_len = 0;
		ph->ph_flags = PKTHDR_USED | PKT_INCOMING;
		mb->m_len = 0;
		mb->m_extsize = ring->buf_size;
		if (likely(rxb && rxb->skb)) {
			mb->m_data = rxb->skb->data;
			mb->m_extbuf = rxb->skb->data;
		}
		mb->skb = NULL;

		/* Push cached writes (skb buffer + ph/mb fields) to DRAM BEFORE
		 * handing ownership to the switch. The ring entries below live in
		 * KSEG1 (uncached) so the SWCORE_OWNED flip is visible to HW the
		 * instant we write it; if the new mb->m_data / m_extbuf were still
		 * sitting in the L1 D-cache, HW could fetch a stale m_data and DMA
		 * into the previous skb's buffer (already passed up the stack via
		 * napi_gro_receive). Mirrors rtl8196e_ring_tx_submit() which uses
		 * the same wback-then-wmb-then-handover-then-wmb sequence.
		 */
		if (likely(rxb && rxb->skb))
			dma_cache_wback_inv((unsigned long)rxb->skb->head,
					    NET_SKB_PAD + NET_IP_ALIGN + ring->buf_size);
		dma_cache_wback_inv((unsigned long)ph, sizeof(*ph));
		dma_cache_wback_inv((unsigned long)mb, sizeof(*mb));
		wmb();

		/* mbuf_index was computed up front from the same mb. */
		if (likely(mbuf_index < ring->rx_mbuf_cnt)) {
			/* Atomic write preserving WRAP bit */
			ring->rx_mbuf_ring[mbuf_index] = (u32)mb | RTL8196E_DESC_SWCORE_OWNED |
							  (ring->rx_mbuf_ring[mbuf_index] & RTL8196E_DESC_WRAP);
		} else {
			ring->diag.rx_rearm_badidx++;
		}

		ring->rx_pkthdr_ring[ring->rx_idx] =
			(u32)ph | (ring->rx_pkthdr_ring[ring->rx_idx] & RTL8196E_DESC_WRAP) | RTL8196E_DESC_SWCORE_OWNED;
		wmb();

		ring->rx_idx++;
		if (ring->rx_idx >= ring->rx_cnt)
			ring->rx_idx = 0;
	}

	if (delivered_out)
		*delivered_out = delivered;
	if (delivered_bytes_out)
		*delivered_bytes_out = delivered_bytes;
	return processed;
}

/* Return the number of free TX descriptor slots available for new submissions. */
__iram int rtl8196e_ring_tx_free_count(struct rtl8196e_ring *ring)
{
	int used;

	if (!ring || ring->tx_cnt == 0)
		return 0;

	{
		unsigned int prod = READ_ONCE(ring->tx_prod);
		unsigned int cons = READ_ONCE(ring->tx_cons);
		if (prod >= cons)
			used = prod - cons;
		else
			used = ring->tx_cnt - cons + prod;
	}

	return (int)ring->tx_cnt - 1 - used;
}

/*
 * Read-only ring snapshot for the panic-time recovery capture
 * (rtl8196e_eth_panic_snapshot). The ring internals are
 * private to this module; this hands the watchdog the cursors and the two
 * descriptor words that show whether HW handed buffers back:
 *   rx_desc = rx_pkthdr_ring[rx_idx] — if OWNED (SWCORE) the switch still owns
 *             the slot the poll is parked on = the §6 RX desync.
 *   tx_desc = tx_ring[tx_cons]      — if OWNED across the stall, TX-done is
 *             stuck (the vendor SDK's hang condition).
 * Reads are plain loads of uncached KSEG1 ring memory — panic-safe, no locks,
 * no cache ops, coherent with HW.
 */
void rtl8196e_ring_panic_snapshot(struct rtl8196e_ring *ring,
				  u32 *rx_idx, u32 *rx_desc,
				  u32 *tx_prod, u32 *tx_cons,
				  u32 *tx_free, u32 *tx_desc)
{
	if (!ring) {
		*rx_idx = *rx_desc = *tx_prod = *tx_cons = *tx_free = *tx_desc = 0;
		return;
	}

	*rx_idx  = ring->rx_idx;
	*rx_desc = (ring->rx_pkthdr_ring && ring->rx_idx < ring->rx_cnt)
			? ring->rx_pkthdr_ring[ring->rx_idx] : 0;
	*tx_prod = ring->tx_prod;
	*tx_cons = ring->tx_cons;
	*tx_free = (u32)rtl8196e_ring_tx_free_count(ring);
	*tx_desc = (ring->tx_ring && ring->tx_cons < ring->tx_cnt)
			? ring->tx_ring[ring->tx_cons] : 0;
}

/*
 * TX-done hang probe for the periodic
 * watchdog. Returns true (and sets *idx = tx_cons) when the TX ring has an
 * outstanding descriptor (tx_cons != tx_prod) that the switch core still owns
 * (SWCORE_OWNED) at tx_cons — i.e. TX-done is not advancing. Mirrors the
 * vendor rtl_check_tx_done_desc_swCore_own. Ring memory is uncached KSEG1, so
 * a plain load is coherent with HW; READ_ONCE on the cursors matches the
 * lockless tx_prod/tx_cons discipline used elsewhere in this module.
 */
bool rtl8196e_ring_tx_done_stuck(struct rtl8196e_ring *ring, u32 *idx)
{
	unsigned int cons, prod;

	if (!ring || !ring->tx_ring)
		return false;
	cons = READ_ONCE(ring->tx_cons);
	prod = READ_ONCE(ring->tx_prod);
	if (cons == prod || cons >= ring->tx_cnt)
		return false;
	if (idx)
		*idx = cons;
	return (ring->tx_ring[cons] & RTL8196E_DESC_OWNED_BIT) != 0;
}

/*
 * TX kick coalescing.  Pulse TXFD on CPUICR at most once per
 * @rtl8196e_kick_threshold submits, except on cold-start (was_empty),
 * which always kicks immediately so the ASIC TX DMA engine wakes up.
 *
 * Profiled cost on RLX4181 @ 380 MHz: a single pulse takes ~1.44 µs
 * (3 register writes + 3 posting reads on the slow MMIO bus).
 * Coalescing 4 packets into 1 kick recovers ~4.3 µs of bus time per
 * batch.  Drained at the end of every NAPI poll so sub-threshold
 * bursts can't sit in the ring after the queue goes idle.
 *
 * Race: pending_kicks is touched from start_xmit (process context)
 * and NAPI poll (softirq).  Worst-case interleaving on UP causes one
 * extra kick — pulses are idempotent on a non-empty ring.
 */
unsigned int rtl8196e_kick_threshold = 4;
EXPORT_SYMBOL(rtl8196e_kick_threshold);

static __iram void rtl8196e_ring_kick_pulse(void)
{
	u32 icr = rtl8196e_readl(CPUICR);

	rtl8196e_writel(icr | TXFD, CPUICR);
	wmb();
	(void)rtl8196e_readl(CPUICR);		/* posting read: ensure pulse visible */
	rtl8196e_writel(icr, CPUICR);
	mb();
	(void)rtl8196e_readl(CPUICR);
}

__iram void rtl8196e_ring_kick_tx(struct rtl8196e_ring *ring, bool was_empty)
{
	if (was_empty) {
		ring->pending_kicks = 0;
		ring->kicks_cold++;
		ring->kicks_total++;
		rtl8196e_ring_kick_pulse();
		return;
	}

	if (++ring->pending_kicks >= READ_ONCE(rtl8196e_kick_threshold)) {
		ring->pending_kicks = 0;
		ring->kicks_threshold++;
		ring->kicks_total++;
		rtl8196e_ring_kick_pulse();
	}
}

__iram void rtl8196e_ring_kick_drain(struct rtl8196e_ring *ring)
{
	if (ring->pending_kicks) {
		ring->pending_kicks = 0;
		ring->kicks_drain++;
		ring->kicks_total++;
		rtl8196e_ring_kick_pulse();
	}
}

void rtl8196e_ring_kick_stats_get(struct rtl8196e_ring *ring,
				  u32 *cold, u32 *thresh, u32 *drain, u32 *total)
{
	if (cold)
		*cold = ring->kicks_cold;
	if (thresh)
		*thresh = ring->kicks_threshold;
	if (drain)
		*drain = ring->kicks_drain;
	if (total)
		*total = ring->kicks_total;
}

/* Copy the ring anomaly counters out for ethtool -S. */
void rtl8196e_ring_diag_get(struct rtl8196e_ring *ring,
			    struct rtl8196e_ring_diag *out)
{
	if (ring && out)
		*out = ring->diag;
}

/*
 * Reinitialise the RX ring so a subsequent open() (or a mid-flight recovery)
 * starts from a known state. Mirrors the RX init block in
 * rtl8196e_ring_create(): same flags, same flush span, same ownership flip
 * order. Reuses every shadow SKB instead of freeing/reallocating, which
 * keeps the cost bounded and avoids OOM in tight `ip link down/up` loops.
 *
 * Callers — .ndo_stop, the poll-side ring resync (rtl8196e_hw_ring_resync),
 * and the deep-reset worker — must have stopped the switch RX engine and
 * quiesced NAPI first, so neither the ASIC nor a poll can touch the ring
 * while it is rebuilt.
 */
void rtl8196e_ring_rx_reset(struct rtl8196e_ring *ring)
{
	unsigned int i;

	if (!ring)
		return;

	for (i = 0; i < ring->rx_cnt; i++) {
		struct rtl_pktHdr *ph = rtl8196e_ph(ring, ring->tx_cnt + i);
		struct rtl_mBuf *mb = &ring->mbuf_pool[ring->tx_cnt + i];
		struct sk_buff *skb = ring->rx_bufs[i].skb;

		memset(ph, 0, sizeof(*ph));
		memset(mb, 0, sizeof(*mb));

		ph->ph_mbuf = mb;
		ph->ph_flags = PKTHDR_USED | PKT_INCOMING;
		ph->ph_type = PKTHDR_ETHERNET;
		ph->ph_portlist = 0;

		mb->m_pkthdr = ph;
		mb->m_flags = MBUF_USED | MBUF_EXT | MBUF_PKTHDR | MBUF_EOR;
		mb->m_len = 0;
		mb->m_extsize = ring->buf_size;

		/*
		 * The shadow SKB allocated by ring_create() (or by a prior
		 * rearm in rx_poll()) is reused as-is. If a previous stop()
		 * left this slot without an SKB — should not happen since we
		 * keep the pool intact across down/up — leave the descriptor
		 * unprogrammed and let the next open() pick it up cleanly.
		 */
		if (!skb) {
			ring->rx_pkthdr_ring[i] = (u32)ph | RTL8196E_DESC_RISC_OWNED;
			ring->rx_mbuf_ring[i] = (u32)mb | RTL8196E_DESC_RISC_OWNED;
			continue;
		}

		mb->m_data = skb->data;
		mb->m_extbuf = skb->data;
		mb->skb = NULL;

		ring->rx_pkthdr_ring[i] = (u32)ph | RTL8196E_DESC_SWCORE_OWNED;
		ring->rx_mbuf_ring[i] = (u32)mb | RTL8196E_DESC_SWCORE_OWNED;

		dma_cache_wback_inv((unsigned long)skb->head,
				    NET_SKB_PAD + NET_IP_ALIGN + ring->buf_size);
		dma_cache_wback_inv((unsigned long)ph, sizeof(*ph));
		dma_cache_wback_inv((unsigned long)mb, sizeof(*mb));
	}

	if (ring->rx_cnt)
		ring->rx_pkthdr_ring[ring->rx_cnt - 1] |= RTL8196E_DESC_WRAP;
	if (ring->rx_mbuf_cnt)
		ring->rx_mbuf_ring[ring->rx_mbuf_cnt - 1] |= RTL8196E_DESC_WRAP;

	ring->rx_idx = 0;
}

/*
 * Reinitialise the TX ring after a watchdog timeout / recovery, freeing any
 * remaining in-flight SKBs. Callers reclaim completed descriptors first, then
 * use the return value for tx_dropped: these SKBs were counted at submit but
 * were still owned by the stopped datapath.
 */
unsigned int rtl8196e_ring_tx_reset(struct rtl8196e_ring *ring)
{
	unsigned int i;
	unsigned int dropped = 0;

	if (!ring)
		return 0;

	for (i = 0; i < ring->tx_cnt; i++) {
		struct rtl_pktHdr *ph = rtl8196e_ph(ring, i);
		struct rtl_mBuf *mb = &ring->mbuf_pool[i];

		if (mb->skb) {
			dev_kfree_skb_any((struct sk_buff *)mb->skb);
			mb->skb = NULL;
			dropped++;
		}

		memset(ph, 0, sizeof(*ph));
		memset(mb, 0, sizeof(*mb));

		ph->ph_mbuf = mb;
		ph->ph_flags = PKTHDR_USED | PKT_OUTGOING;
		ph->ph_type = PKTHDR_ETHERNET;
		ph->ph_portlist = 0;

		mb->m_pkthdr = ph;
		mb->m_flags = MBUF_USED | MBUF_EXT | MBUF_PKTHDR | MBUF_EOR;
		mb->m_data = NULL;
		mb->m_extbuf = NULL;
		mb->m_extsize = 0;

		ring->tx_ring[i] = (u32)ph | RTL8196E_DESC_RISC_OWNED;

		dma_cache_wback_inv((unsigned long)ph, sizeof(*ph));
		dma_cache_wback_inv((unsigned long)mb, sizeof(*mb));
	}

	if (ring->tx_cnt)
		ring->tx_ring[ring->tx_cnt - 1] |= RTL8196E_DESC_WRAP;

	ring->tx_prod = 0;
	ring->tx_cons = 0;

	return dropped;
}
