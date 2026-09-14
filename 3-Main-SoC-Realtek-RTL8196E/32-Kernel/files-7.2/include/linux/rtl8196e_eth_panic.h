/* SPDX-License-Identifier: GPL-2.0 */
/*
 * rtl8196e_eth_panic.h - panic-time snapshot of the RTL8196E Ethernet
 * driver's switch-core recovery state.
 *
 * Cross-subsystem contract between the rtl8196e-eth driver (producer) and
 * the rtl819x watchdog driver (consumer). The watchdog leaves a DRAM
 * post-mortem record that survives the soft-lockup reset; from record v5 it
 * carries this eth snapshot. The same fields are also emitted to the kernel
 * log at every recovery action (rtl8196e_eth_diag_log) — so a self-heal that
 * never panics still records the fingerprint, while this DRAM copy is the
 * fallback for the case the box hangs anyway and the watchdog reboots it.
 *
 * Record v7 reframes the snapshot around the root cause — an unbounded RX
 * poll under a drop-flood. The decisive fields are now
 * poll_budget_hit / rx_stall_run (did the bounded poll saturate with zero
 * delivery, and how far did the stall-detector run climb), the diag block
 * (A: intrinsic switch desync vs B: real runt flood), and the switch-side
 * descriptor pointers/counter cross-checked against the driver's rx_idx.
 *
 * The watchdog carries a __weak default definition that returns false; the
 * eth driver provides the strong override. So the watchdog always has a
 * callable symbol (it links and runs whether or not the eth driver is built
 * in), and — because the watchdog's call site is a normal strong reference —
 * the override survives --gc-sections. The snapshot is read from the panic
 * notifier, so it must touch only plain memory and MMIO — no locks, no sleeping.
 */
#ifndef _LINUX_RTL8196E_ETH_PANIC_H
#define _LINUX_RTL8196E_ETH_PANIC_H

#include <linux/types.h>

struct rtl8196e_eth_panic {
	/* v5 fields (kept) */
	u32 up;		/* 1 if the interface was running at panic, else 0 */
	u32 resync;	/* rx_runout_resync — poll-side full resyncs performed */
	u32 kick;	/* rx_runout_kick   — swcore-watchdog NAPI kicks */
	u32 iisr;	/* live CPUIISR at panic (which IRQ source is asserted) */
	u32 iimr;	/* live CPUIIMR at panic (which sources are unmasked) */
	u32 rx_idx;	/* driver RX ring cursor */
	/*
	 * v6 fields (kept) — switch-core / DMA / ring-progress state, to tell an
	 * RX storm from a broader switch-core or TX-done stall:
	 */
	u32 rx_desc;	/* rx_pkthdr_ring[rx_idx] — OWNED bit + pkthdr ptr (desync proof) */
	u32 tx_prod;	/* TX producer index */
	u32 tx_cons;	/* TX consumer (next-to-reclaim) index */
	u32 tx_free;	/* free TX descriptor slots */
	u32 tx_desc;	/* tx_ring[tx_cons] — OWNED bit + flags (TX-done stuck?) */
	u32 cpuicr;	/* live CPUICR — CPU-port RX/TX DMA enable */
	u32 sirr;	/* live SIRR — switch interface (TRXRDY) */
	u32 rx_packets;	/* dev->stats.rx_packets (low 32) — forward-progress gauge */
	u32 tx_packets;	/* dev->stats.tx_packets (low 32) — forward-progress gauge */
	/*
	 * v7 fields — the unbounded-poll root cause. The bounded
	 * poll's stall-detector state plus the A-vs-B discriminator: under a
	 * drop-flood the poll saturates the budget with zero delivery. The diag
	 * counters classify the drops — wild_pkthdr/wild_mbuf/mbuf_no_shadow/skew
	 * point to an intrinsic switch desync (A); bad_len points to a real runt
	 * flood (B). cpurpdcr0/cpurmdcr0 are the switch's own RX descriptor
	 * pointers (vs rx_idx = switch-vs-driver desync); p6_dcr0 is the CPU-port
	 * descriptor counter on the switch side.
	 */
	u32 poll_budget_hit;	/* polls that saturated the NAPI budget */
	u32 rx_stall_run;	/* consecutive budget-saturating zero-delivery polls (detector run) */
	u32 swcore_deep_reset;	/* switch-core deep resets performed (did recovery fire?) */
	u32 cpurpdcr0;		/* live CPURPDCR0 — switch RX pkthdr descriptor pointer */
	u32 cpurmdcr0;		/* live CPURMDCR0 — switch RX mbuf descriptor pointer */
	u32 wild_pkthdr;	/* diag.rx_wild_pkthdr     \                          */
	u32 wild_mbuf;		/* diag.rx_wild_mbuf        > A: intrinsic switch desync */
	u32 mbuf_no_shadow;	/* diag.rx_mbuf_no_shadow  |                          */
	u32 skew;		/* diag.rx_pkthdr_mbuf_skew /                          */
	u32 bad_len;		/* diag.rx_bad_len — B: real runt flood */
	u32 p6_dcr0;		/* live P6_DCR0 — CPU-port descriptor counter (switch side) */
	/*
	 * v8 fields — the first fully-instrumented field crash (issue #99,
	 * v4.0.0-rc4) proved the v7 detectors structurally blind to the field
	 * storm: every counter above read zero at panic, and the eth state was
	 * indistinguishable from the normal napi_defer window. v8 shifts the
	 * question from "which eth recovery fired" to "was eth even ACTIVE":
	 * activity counters with last-activity jiffies stamps (panic_j −
	 * isr_last_j > storm duration = bystander proof in one subtraction),
	 * the per-jiffy ISR burst high-water (an eth-line hardirq storm cannot
	 * hide from it), the uncounted SCHED-without-masking window (tx-reclaim
	 * timer kicks), the live NAPI state bits, and the RX ring base physical
	 * addresses that make the v7 switch-pointer captures
	 * (cpurpdcr0/cpurmdcr0) decodable into ring indices off-box.
	 */
	u32 isr_cnt;		/* eth ISR invocations since boot */
	u32 isr_last_j;		/* jiffies at last eth ISR entry */
	u32 isr_burst_max;	/* max eth ISR entries observed within one jiffy */
	u32 poll_cnt;		/* NAPI poll invocations since boot */
	u32 poll_last_j;	/* jiffies at last NAPI poll entry */
	u32 poll_delivered;	/* packets delivered by the poll (sum, wraps) */
	u32 kick_txreclaim;	/* tx-reclaim timer napi_schedule kicks (SCHED+unmasked window) */
	u32 kick_txreclaim_j;	/* jiffies at last tx-reclaim kick */
	u32 napi_state;		/* live napi->state bits (SCHED/MISSED/...) */
	u32 rx_ph_base;		/* phys addr of rx_pkthdr_ring[0] (decode cpurpdcr0 → index) */
	u32 rx_mb_base;		/* phys addr of rx_mbuf_ring[0]   (decode cpurmdcr0 → index) */
};

/*
 * Fill *out and return true if the (single) eth instance is up; return false
 * (leaving *out untouched) otherwise. The rtl8196e-eth driver provides the
 * strong definition; the watchdog provides a __weak default returning false,
 * so a kernel without the eth driver still links. Panic-safe: plain reads + MMIO.
 */
bool rtl8196e_eth_panic_snapshot(struct rtl8196e_eth_panic *out);

/*
 * 1 Hz flight-recorder sample of the eth side, consumed by the rtl819x
 * watchdog's storm-proof hrtimer sampler (record v8). Same producer/__weak-
 * consumer linkage as the panic snapshot above. Hardirq-safe by construction:
 * two MMIO loads plus plain word reads.
 */
struct rtl8196e_eth_flight {
	u32 iisr;		/* live CPUIISR */
	u32 iimr;		/* live CPUIIMR */
	u32 isr_cnt;		/* cumulative — the sampler computes deltas */
	u32 poll_cnt;
	u32 poll_delivered;
	u32 rx_packets;		/* dev->stats.rx_packets (low 32) */
	u32 napi_state;		/* live napi->state bits */
};

/*
 * Fill *out and return true if the (single) eth instance is up; false
 * otherwise. Strong definition in the eth driver, __weak default (false) in
 * the watchdog.
 */
bool rtl8196e_eth_flight_sample(struct rtl8196e_eth_flight *out);

#endif /* _LINUX_RTL8196E_ETH_PANIC_H */
