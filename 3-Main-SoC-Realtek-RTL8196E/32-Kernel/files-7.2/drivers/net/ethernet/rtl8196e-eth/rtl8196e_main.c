// SPDX-License-Identifier: GPL-2.0
/*
 * rtl8196e_main.c - RTL8196E Ethernet driver core.
 *
 * Covers: net_device lifecycle, NAPI poll, interrupt handler, TX path,
 * ethtool statistics, and platform probe/remove.
 * Targets a single physical port (port 4) on the Lidl Silvercrest Zigbee gateway.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/of.h>
#include <linux/string.h>
#include <linux/interrupt.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/errno.h>
#include <linux/ethtool.h>
#include <linux/gpio/consumer.h>
#include <linux/u64_stats_sync.h>
#include <linux/mfd/syscon.h>
#include <linux/rtl8196e_eth_panic.h>
#include <asm/cacheflush.h>
#include <asm/mach-realtek/imem.h>
#include "rtl8196e_dt.h"
#include "rtl8196e_hw.h"
#include "rtl8196e_ring.h"
#include "rtl8196e_regs.h"

#define RTL8196E_DRV_NAME "rtl8196e-eth"
#define RTL8196E_DRV_VERSION "2.24"

#define RTL8196E_TX_DESC      128
#define RTL8196E_RX_DESC      128
#define RTL8196E_RX_MBUF_DESC 128
#define RTL8196E_CLUSTER_SIZE 1700

#define RTL8196E_TX_STOP_THRESH 4
#define RTL8196E_TX_WAKE_THRESH 16
/*
 * Software TX-reclaim timer period. The TX_ALL_DONE IRQ is left masked (TX
 * reclaim runs in start_xmit and NAPI poll for throughput), so a TX queue that
 * stops (ring-full XOFF or BQL byte-limit XOFF) while no RX is arriving has no
 * path to reclaim its in-flight descriptors before the 10 s netdev watchdog.
 * This short timer polls reclaim in exactly that window.
 */
#define RTL8196E_TX_RECLAIM_MS 4

/*
 * Switch-core RX-runout recovery. The switch RX engine and the driver rx_idx
 * cursor can desync (e.g. a TRXRDY cycle that rewinds the switch RX pointer
 * without resyncing rx_idx). The switch then asserts PKTHDR_DESC_RUNOUT
 * continuously and the ISR -> NAPI-poll -> re-enable handshake spins doing zero
 * work — a self-sustaining interrupt storm that pins the CPU until the watchdog
 * reboots. The vendor SDK recovers via a periodic stuck-detector that re-inits
 * the switch core (rtl_check_swCore_tx_hang -> rtl865x_reinitSwitchCore); this
 * from-scratch driver restores an equivalent safety net with two complementary
 * detectors:
 *   - poll-side: RTL8196E_RUNOUT_RESYNC_THRESH consecutive zero-work polls taken
 *     while PKTHDR_DESC_RUNOUT is asserted trigger a full ring resync from poll
 *     context (breaks the storm in a few microseconds — the primary path);
 *   - periodic: every RTL8196E_SWCORE_CHECK_MS a watchdog timer checks for
 *     PKTHDR_DESC_RUNOUT staying asserted across RTL8196E_SWCORE_HANG_THRESH
 *     checks and kicks a poll, covering a non-CPU-pinning stall where NAPI is
 *     not otherwise being driven (belt-and-suspenders).
 */
#define RTL8196E_RUNOUT_RESYNC_THRESH 3
#define RTL8196E_SWCORE_CHECK_MS      1000
#define RTL8196E_SWCORE_HANG_THRESH   3

/*
 * Switch-core deep-reset escalation thresholds.
 * RTL8196E_DEEP_RESET_THRESH: consecutive shallow ring resyncs that fail to
 * recover (no productive poll in between) before escalating to a full
 * switch-core deep reset.
 * RTL8196E_TX_HANG_THRESH: consecutive 1 s checks finding the TX-done
 * descriptor still SWCORE_OWNED at the same index before declaring the switch
 * core wedged and forcing a deep reset (mirrors the vendor's
 * rtl_reinit_swCore_threshold in rtl_check_swCore_tx_hang).
 */
#define RTL8196E_DEEP_RESET_THRESH    3
#define RTL8196E_TX_HANG_THRESH       3
#define RTL8196E_RESET_RETRY_MAX      3
#define RTL8196E_RESET_RETRY_BASE_MS  1000

/*
 * Switch-core PHY-interface watchdog: consecutive 1 s checks finding the
 * switch-core per-port EnablePHYIf bit cleared on an administratively-up port
 * before forcing a deep reset. Mirrors the vendor one_sec_timer() PHY-interface
 * recovery branch (rtl_nic.c, the "(PCRP & EnablePHYIf) == 0" path that runs
 * rtl865x_reinitSwitchCore). >1 so the brief clear-then-reenable window of a
 * deep reset cannot self-trigger.
 */
#define RTL8196E_PHYIF_LOST_THRESH    3

/*
 * Consecutive budget-saturating polls that delivered ZERO packets before
 * escalating to a switch-core deep reset. A drop-dominated RX flood (intrinsic
 * switch desync or runt storm) saturates `processed` while `delivered` stays
 * 0 — a RUNOUT-independent soft-lockup signature. Bench-tuned; a real workload
 * delivers something within a few polls so the run never accumulates.
 */
static unsigned int rtl8196e_rx_stall_thresh = 32;
module_param(rtl8196e_rx_stall_thresh, uint, 0644);
MODULE_PARM_DESC(rtl8196e_rx_stall_thresh,
		 "consecutive zero-delivery budget-saturating polls before a switch-core deep reset (0 disables)");

static unsigned int link_poll_ms;
module_param(link_poll_ms, uint, 0644);
MODULE_PARM_DESC(link_poll_ms, "Link poll interval in ms (0=disabled)");

static unsigned int rtl8196e_cpu_port_mask = RTL8196E_CPU_PORT_MASK;
module_param(rtl8196e_cpu_port_mask, uint, 0644);
MODULE_PARM_DESC(rtl8196e_cpu_port_mask, "CPU port mask for VLAN/L2 (default=0x20)");

/*
 * led_mode intent. The switch-core deep reset rewrites LEDCREG unconditionally
 * (rtl8196e_hw_swcore_reset), so an operator's "dim"/"off" would silently
 * revert on the first reset. Store the requested mode and replay it after each
 * deep reset. Default 0 = bright, matching the reset value hw_init leaves.
 */
enum { RTL8196E_LED_BRIGHT = 0, RTL8196E_LED_DIM, RTL8196E_LED_OFF };

struct rtl8196e_priv {
	struct net_device *ndev;
	struct napi_struct napi;
	struct rtl8196e_hw hw;
	struct rtl8196e_ring *ring;
	struct rtl8196e_dt_iface iface;
	struct timer_list link_timer;
	struct timer_list tx_reclaim_timer;
	struct timer_list swcore_check_timer;
	struct timer_list swcore_retry_timer;
	u32 rx_runout_zero;	/* consecutive zero-work polls under RUNOUT */
	u32 swcore_runout_seen;	/* consecutive periodic checks with RUNOUT asserted */
	u32 rx_runout_resync;	/* storm resyncs performed (ethtool) */
	u32 rx_runout_kick;	/* periodic-watchdog NAPI kicks (ethtool) */
	/*
	 * Deep switch-core reset escalation. The in-poll ring resync above is
	 * shallow (it resyncs the RX/TX ring pointers only); if it fires
	 * repeatedly without clearing the storm, or the TX-done watchdog sees the
	 * switch core wedged, escalate to a full FullAndSemiReset (SIRR FULL_RST +
	 * switch-core clock cycle, the vendor rtl865x_reinitSwitchCore depth) from
	 * process context.
	 */
	struct work_struct swcore_reset_work;
	u32 rx_runout_resync_run;	/* consecutive resyncs that did not recover */
	u32 swcore_deep_reset;		/* full switch-core deep resets performed */
	u32 tx_hang_last_idx;		/* last TX-done index seen stuck */
	u32 tx_hang_cnt;		/* consecutive checks stuck at the same idx */
	/* RUNOUT-independent drop-flood stall detector + counters. */
	u32 rx_stall_run;	/* consecutive budget-saturating zero-delivery polls */
	u32 rx_stall_a_base;	/* structural-desync counter sum snapshot at run start */
	u32 poll_budget_hit;	/* polls that saturated the budget (ethtool) */
	/*
	 * issue #99 record-v8 activity taps. The first fully-instrumented
	 * field crash showed every recovery counter at zero — the open
	 * question moved from "which recovery fired" to "was eth even active
	 * during the storm". Counters + last-activity jiffies stamps answer
	 * that in one subtraction against the panic-time jiffies; the
	 * per-jiffy ISR burst high-water is the witness an eth-line hardirq
	 * storm cannot dodge; the tx-reclaim kick pair counts the only
	 * uncounted NAPI-SCHED-without-masking window. Plain u32 stores on
	 * paths already dominated by MMIO.
	 */
	u32 isr_cnt;		/* ISR invocations */
	u32 isr_last_j;		/* jiffies at last ISR entry */
	u32 isr_burst_j;	/* jiffy the current burst window belongs to */
	u32 isr_burst;		/* ISR entries within the current jiffy */
	u32 isr_burst_max;	/* high-water of isr_burst */
	u32 poll_cnt;		/* NAPI poll invocations */
	u32 poll_last_j;	/* jiffies at last poll entry */
	u32 poll_delivered;	/* packets delivered by the poll (sum, wraps) */
	u32 kick_txreclaim;	/* tx-reclaim timer napi_schedule kicks */
	u32 kick_txreclaim_j;	/* jiffies at last tx-reclaim kick */
	/* Switch-core PHY-interface watchdog. */
	u32 phyif_lost_cnt;	/* consecutive checks with EnablePHYIf read back clear */
	/* xmit_stopped hint observability (see rtl8196e_start_xmit/_poll). */
	u32 tx_xoff_seen_xmit;	/* start_xmit found the TX queue stopped after submit */
	u32 tx_xoff_seen_poll;	/* NAPI poll found the TX queue stopped after reclaim */
	/*
	 * TX skbs reclaimed in start_xmit context, parked here for the NAPI
	 * poll to free through napi_consume_skb() with a real budget instead
	 * of dev_consume_skb_any() inline. Softirq-serialized on this UP
	 * core (xmit runs BH-disabled; poll and the reclaim timer are
	 * softirq), so the lockless __skb queue ops are safe — same argument
	 * as the driver's bare counters.
	 */
	struct sk_buff_head tx_defer_list;
	u16 vlan_id;
	u16 portmask;
	int phy_port;
	int phy_id;
	unsigned int link_poll_ms;
	u32 l2_check_ok;
	u32 l2_check_fail;
	int l2_check_last;
	u32 swcore_reprogram_fail;	/* deep resets whose hw_reprogram failed */
	u8 swcore_retry_count;		/* retries scheduled for the current failure */
	bool swcore_recovery_failed;	/* keep carrier, queue and IRQs down */
	u8 led_mode;			/* RTL8196E_LED_* intent, replayed after deep reset */
	struct gpio_desc *lan_led_gpio;	/* inactive GPIO state used by true OFF */
	u32 lan_led_mux_mask;		/* this board's B2..B6 field in PIN_MUX_SEL_2 */
	/*
	 * 64-bit stat accumulators. ndev->stats fields are unsigned long (32-bit
	 * on this arch) and wrap in days at line rate; these back ndo_get_stats64
	 * so userspace sees non-wrapping packet/byte totals. The shared syncp is
	 * safe because the !SMP, BH-serialized xmit/poll paths provide one writer.
	 */
	struct {
		u64_stats_t rx_packets;
		u64_stats_t rx_bytes;
		u64_stats_t tx_packets;
		u64_stats_t tx_bytes;
		struct u64_stats_sync syncp;
	} stats64;
};

/*
 * Program the front-panel LAN LED for @mode.
 *
 * DIRECTLCR is a duty-scale register, not an output-disable register: zero
 * still leaves a visible minimum-duty glow. Likewise LEDCREG=0 selects scan
 * topology, which is the DIM mode. A true OFF therefore parks the active-low
 * LED pad at logical zero (physical high) through its GPIO descriptor and
 * switches only that pad from LED_PORTn to GPIO. BRIGHT/DIM program the ASIC
 * first, then reconnect the pad, so no low-going glitch is exposed.
 *
 * Shared by the led_mode sysfs store and the post-deep-reset replay so both
 * stay in lockstep.
 */
static int rtl8196e_apply_led_mode(struct rtl8196e_priv *priv, u8 mode)
{
	int ret;

	switch (mode) {
	case RTL8196E_LED_DIM:
		rtl8196e_writel(DIRECTLCR_DEFAULT, DIRECTLCR);
		rtl8196e_writel(0, LEDCREG);
		break;
	case RTL8196E_LED_OFF:
		if (!priv->lan_led_gpio)
			return -EOPNOTSUPP;

		gpiod_set_value_cansleep(priv->lan_led_gpio, 0);
		return regmap_update_bits(priv->hw.syscon, 0x44,
					  priv->lan_led_mux_mask,
					  priv->lan_led_mux_mask);
	case RTL8196E_LED_BRIGHT:
	default:
		rtl8196e_writel(DIRECTLCR_DEFAULT, DIRECTLCR);
		rtl8196e_writel(LEDMODE_DIRECT, LEDCREG);
		break;
	}

	if (!priv->lan_led_gpio)
		return 0;

	ret = regmap_update_bits(priv->hw.syscon, 0x44,
				 priv->lan_led_mux_mask, 0);
	return ret;
}

/*
 * Schedule NAPI the way the ISR does: mask the device IRQs before publishing
 * NAPI_STATE_SCHED, so an RX hard IRQ arriving in the window cannot re-enter
 * the poll (its napi_schedule_prep sees SCHED set and the ISR would otherwise
 * leave CPUIIMR unmasked). napi_complete_done() in the poll is the single site
 * that re-enables the IRQs. Shared by the ISR and the two timer kick paths.
 */
static void rtl8196e_napi_kick(struct rtl8196e_priv *priv)
{
	/*
	 * Mask first even when NAPI is already scheduled. In that case
	 * napi_schedule_prep() records MISSED and the active poll remains the only
	 * path that may unmask the device after draining the pending event.
	 */
	rtl8196e_hw_disable_irqs(&priv->hw);
	if (unlikely(READ_ONCE(priv->swcore_recovery_failed)))
		return;

	if (napi_schedule_prep(&priv->napi))
		__napi_schedule(&priv->napi);
}

/*
 * Single-instance back-pointer for the panic-time recovery snapshot. There is one
 * eth port on this SoC; set after register_netdev() succeeds in probe and
 * cleared in remove. Read unlocked from the watchdog panic notifier
 * (rtl8196e_eth_panic_snapshot) — safe on this UP platform: a plain pointer
 * load of a value that only the single probe/remove path ever writes.
 */
static struct rtl8196e_priv *rtl8196e_panic_priv;

/*
 * Capture the switch-core recovery fingerprint from a known-good priv. Shared by
 * the watchdog panic snapshot (DRAM record v7) and rtl8196e_eth_diag_log() (the
 * kernel-log fingerprint emitted at each recovery action), so both carry
 * identical fields. Panic/softirq-safe: plain counter reads, the same MMIO
 * loads rtl8196e_poll() already performs, and a lockless copy of the ring diag
 * counters. v7 reframes the snapshot around the unbounded-poll root cause —
 * poll_budget_hit/rx_stall_run say whether the bounded poll saturated with zero
 * delivery; the diag block discriminates A (intrinsic switch desync: wild/skew)
 * from B (real runt flood: bad_len); cpurpdcr0/cpurmdcr0/p6_dcr0 are the
 * switch's own RX descriptor pointers/counter, cross-checked against rx_idx.
 */
static void __rtl8196e_eth_capture(struct rtl8196e_priv *priv,
				   struct rtl8196e_eth_panic *out)
{
	struct rtl8196e_ring_diag diag;

	out->up     = 1;
	out->resync = priv->rx_runout_resync;
	out->kick   = priv->rx_runout_kick;
	out->iisr   = rtl8196e_readl(CPUIISR);
	out->iimr   = rtl8196e_readl(CPUIIMR);
	out->cpuicr = rtl8196e_readl(CPUICR);
	out->sirr   = rtl8196e_readl(SIRR);
	out->rx_packets = (u32)priv->ndev->stats.rx_packets;
	out->tx_packets = (u32)priv->ndev->stats.tx_packets;
	rtl8196e_ring_panic_snapshot(priv->ring, &out->rx_idx, &out->rx_desc,
				     &out->tx_prod, &out->tx_cons,
				     &out->tx_free, &out->tx_desc);
	/* v7: bounded-poll detector state + A/B discriminator + switch desync. */
	out->poll_budget_hit   = priv->poll_budget_hit;
	out->rx_stall_run      = priv->rx_stall_run;
	out->swcore_deep_reset = priv->swcore_deep_reset;
	out->cpurpdcr0 = rtl8196e_readl(CPURPDCR0);
	out->cpurmdcr0 = rtl8196e_readl(CPURMDCR0);
	out->p6_dcr0   = rtl8196e_readl(P6_DCR0);
	rtl8196e_ring_diag_get(priv->ring, &diag);
	out->wild_pkthdr    = diag.rx_wild_pkthdr;
	out->wild_mbuf      = diag.rx_wild_mbuf;
	out->mbuf_no_shadow = diag.rx_mbuf_no_shadow;
	out->skew           = diag.rx_pkthdr_mbuf_skew;
	out->bad_len        = diag.rx_bad_len;
	/*
	 * v8: activity taps + NAPI state + ring bases. The bases are stored in
	 * the same (KSEG1) address form the switch registers hold, so
	 * cpurpdcr0 − rx_ph_base decodes straight to a byte offset / index.
	 */
	out->isr_cnt          = priv->isr_cnt;
	out->isr_last_j       = priv->isr_last_j;
	out->isr_burst_max    = priv->isr_burst_max;
	out->poll_cnt         = priv->poll_cnt;
	out->poll_last_j      = priv->poll_last_j;
	out->poll_delivered   = priv->poll_delivered;
	out->kick_txreclaim   = priv->kick_txreclaim;
	out->kick_txreclaim_j = priv->kick_txreclaim_j;
	out->napi_state       = (u32)READ_ONCE(priv->napi.state);
	out->rx_ph_base       = (u32)(uintptr_t)rtl8196e_ring_rx_pkthdr_base(priv->ring);
	out->rx_mb_base       = (u32)(uintptr_t)rtl8196e_ring_rx_mbuf_base(priv->ring);
}

/*
 * Panic-time snapshot of the switch-core recovery state. Called from the rtl819x
 * watchdog panic notifier to populate record v7 — the fallback capture for the
 * case the box hangs anyway and the watchdog reboots it. Panic context: no
 * locks, only plain reads and MMIO. Declared in the shared header (the watchdog
 * carries a __weak default returning false, so it links without us).
 */
bool rtl8196e_eth_panic_snapshot(struct rtl8196e_eth_panic *out)
{
	struct rtl8196e_priv *priv = rtl8196e_panic_priv;

	if (!priv || !priv->ndev || !netif_running(priv->ndev) || !priv->ring)
		return false;
	__rtl8196e_eth_capture(priv, out);
	return true;
}

/*
 * 1 Hz flight-recorder sample for the watchdog's storm-proof hrtimer
 * (record v8). Deliberately much lighter than the full capture: two MMIO
 * loads plus word reads, no ring-diag copy — it runs every second for the
 * life of the box, in hardirq context. Same __weak linkage as the panic
 * snapshot.
 */
bool rtl8196e_eth_flight_sample(struct rtl8196e_eth_flight *out)
{
	struct rtl8196e_priv *priv = rtl8196e_panic_priv;

	if (!priv || !priv->ndev || !netif_running(priv->ndev) || !priv->ring)
		return false;
	out->iisr           = rtl8196e_readl(CPUIISR);
	out->iimr           = rtl8196e_readl(CPUIIMR);
	out->isr_cnt        = priv->isr_cnt;
	out->poll_cnt       = priv->poll_cnt;
	out->poll_delivered = priv->poll_delivered;
	out->rx_packets     = (u32)priv->ndev->stats.rx_packets;
	out->napi_state     = (u32)READ_ONCE(priv->napi.state);
	return true;
}

/*
 * Emit the same fingerprint to the kernel log at a recovery action. With the
 * bounded poll the box self-heals (deep reset / resync) without panicking,
 * so the DRAM panic record is not written in the common case — this log line is
 * the capture for that case (it survives because there is no reboot). @cause
 * names the recovery site. Rate-limited by its call sites (each fires only at a
 * detector-threshold crossing or a timeout, never per packet).
 */
static void rtl8196e_eth_diag_log(struct rtl8196e_priv *priv, const char *cause)
{
	struct rtl8196e_eth_panic e;

	if (!priv || !priv->ndev || !priv->ring)
		return;
	__rtl8196e_eth_capture(priv, &e);
	netdev_warn(priv->ndev,
		    "eth recovery fingerprint [%s]: iisr=0x%x iimr=0x%x pollhit=%u stallrun=%u deepreset=%u resync=%u kick=%u rxidx=%u rxdesc=0x%x rpdcr0=0x%x rmdcr0=0x%x p6dcr0=0x%x txprod=%u txcons=%u txfree=%u | A: wildph=%u wildmb=%u noshadow=%u skew=%u  B: badlen=%u | rxpkts=%u txpkts=%u\n",
		    cause, e.iisr, e.iimr, e.poll_budget_hit, e.rx_stall_run,
		    e.swcore_deep_reset, e.resync, e.kick, e.rx_idx, e.rx_desc,
		    e.cpurpdcr0, e.cpurmdcr0, e.p6_dcr0, e.tx_prod, e.tx_cons,
		    e.tx_free, e.wild_pkthdr, e.wild_mbuf, e.mbuf_no_shadow,
		    e.skew, e.bad_len, e.rx_packets, e.tx_packets);
}

/* Return the port number (0-5) for the lowest set bit in mask, or -EINVAL. */
static int rtl8196e_port_from_mask(u16 mask)
{
	int port;

	for (port = 0; port < 6; port++) {
		if (mask & (1 << port))
			return port;
	}

	return -EINVAL;
}

/* Periodic link poll timer: update carrier state and reschedule. */
static void rtl8196e_link_timer_fn(struct timer_list *t)
{
	struct rtl8196e_priv *priv = timer_container_of(priv, t, link_timer);
	bool link;

	if (!netif_running(priv->ndev))
		return;

	if (unlikely(READ_ONCE(priv->swcore_recovery_failed))) {
		netif_carrier_off(priv->ndev);
	} else {
		link = rtl8196e_hw_link_up(&priv->hw, priv->phy_port);
		if (link)
			netif_carrier_on(priv->ndev);
		else
			netif_carrier_off(priv->ndev);
	}

	if (priv->link_poll_ms)
		mod_timer(&priv->link_timer, jiffies + msecs_to_jiffies(priv->link_poll_ms));
}

/*
 * Software TX-reclaim path for the no-RX stall. With the TX_ALL_DONE IRQ
 * masked, a stopped TX queue with no RX traffic has nothing to drive reclaim:
 * start_xmit is no longer called (queue stopped) and NAPI is RX-woken, so the
 * queue sits stopped until the 10 s netdev watchdog fires tx_timeout. Arm this
 * timer whenever the queue stops; the callback kicks a NAPI poll (which
 * reclaims, runs netdev_completed_queue and wakes / un-freezes the queue), and
 * the poll re-arms it while the queue stays stopped. Once the queue moves again
 * the timer simply lapses — bounded, never runs on a draining queue. This keeps
 * a purely-TX or low-RX workload from stalling until the netdev watchdog.
 */
static void rtl8196e_arm_tx_reclaim(struct rtl8196e_priv *priv)
{
	/*
	 * Arm only if not already pending. This is called from the TX hot path
	 * (start_xmit and the NAPI-poll re-arm) every time the queue is found
	 * stopped, which under load is most packets; an unconditional mod_timer()
	 * there re-inserts the timer per packet and measurably costs TX
	 * throughput on this CPU. With the guard the timer still fires within one
	 * RTL8196E_TX_RECLAIM_MS window — enough to break a no-RX stall — but is
	 * free once armed. timer_pending() + mod_timer() is not atomic, but this
	 * is a UP platform and a rare double-arm is harmless.
	 */
	if (!READ_ONCE(priv->swcore_recovery_failed) &&
	    !timer_pending(&priv->tx_reclaim_timer))
		mod_timer(&priv->tx_reclaim_timer,
			  jiffies + msecs_to_jiffies(RTL8196E_TX_RECLAIM_MS));
}

static void rtl8196e_tx_reclaim_timer_fn(struct timer_list *t)
{
	struct rtl8196e_priv *priv = timer_container_of(priv, t, tx_reclaim_timer);

	if (netif_running(priv->ndev) &&
	    !READ_ONCE(priv->swcore_recovery_failed)) {
		/*
		 * record-v8 tap: count the tx-reclaim NAPI kicks. This path now
		 * masks the device IRQs before publishing NAPI_STATE_SCHED
		 * (rtl8196e_napi_kick), closing the former SCHED-with-IRQs-
		 * enabled window this tap was watching; the counter is retained
		 * for field-capture continuity.
		 */
		priv->kick_txreclaim++;
		priv->kick_txreclaim_j = (u32)jiffies;
		rtl8196e_napi_kick(priv);
	}
}

/* Run one bounded recovery retry from process context. */
static void rtl8196e_swcore_retry_timer_fn(struct timer_list *t)
{
	struct rtl8196e_priv *priv = timer_container_of(priv, t,
							 swcore_retry_timer);

	if (netif_running(priv->ndev) &&
	    READ_ONCE(priv->swcore_recovery_failed))
		schedule_work(&priv->swcore_reset_work);
}

/*
 * Periodic switch-core watchdog (belt-and-suspenders for the RX-runout storm).
 * If PKTHDR_DESC_RUNOUT stays asserted across RTL8196E_SWCORE_HANG_THRESH
 * consecutive checks, the NAPI path is not clearing it — kick a poll so the
 * poll-side detector runs the resync. During the CPU-pinned storm the poll
 * itself recovers within microseconds and this timer never runs; it exists for
 * a non-pinning stall where NAPI is not being driven. Reading CPUIISR is a
 * single MMIO load once per second. Mirrors the vendor SDK's
 * rtl_check_swCore_tx_hang() -> rtl865x_reinitSwitchCore().
 */
static void rtl8196e_swcore_check_timer_fn(struct timer_list *t)
{
	struct rtl8196e_priv *priv = timer_container_of(priv, t, swcore_check_timer);

	if (!netif_running(priv->ndev))
		return;
	if (unlikely(READ_ONCE(priv->swcore_recovery_failed)))
		goto rearm;

	if (rtl8196e_readl(CPUIISR) & PKTHDR_DESC_RUNOUT_IP_ALL) {
		if (++priv->swcore_runout_seen >= RTL8196E_SWCORE_HANG_THRESH) {
			priv->swcore_runout_seen = 0;
			priv->rx_runout_kick++;
			rtl8196e_napi_kick(priv);
		}
	} else {
		priv->swcore_runout_seen = 0;
	}

	/*
	 * TX-done hang watchdog, mirroring the vendor
	 * rtl_check_swCore_tx_hang. If the TX-done descriptor stays
	 * SWCORE_OWNED at the same index across RTL8196E_TX_HANG_THRESH checks
	 * with the link up, the switch core is wedged on TX — a stall the
	 * RX-runout detectors cannot see — so escalate to a deep reset. The
	 * carrier guard avoids a false positive from a packet legitimately
	 * parked in TX while the link is down.
	 */
	if (netif_carrier_ok(priv->ndev)) {
		u32 idx = 0;

		if (rtl8196e_ring_tx_done_stuck(priv->ring, &idx)) {
			if (idx == priv->tx_hang_last_idx &&
			    ++priv->tx_hang_cnt >= RTL8196E_TX_HANG_THRESH) {
				priv->tx_hang_cnt = 0;
				netdev_warn(priv->ndev,
					    "TX-done descriptor %u stuck\n", idx);
				rtl8196e_eth_diag_log(priv, "tx-done-stuck");
				schedule_work(&priv->swcore_reset_work);
			} else if (idx != priv->tx_hang_last_idx) {
				priv->tx_hang_last_idx = idx;
				priv->tx_hang_cnt = 1;
			}
		} else {
			priv->tx_hang_cnt = 0;
		}
	} else {
		priv->tx_hang_cnt = 0;
	}

	/*
	 * Switch-core PHY-interface watchdog, mirroring the vendor
	 * one_sec_timer() EnablePHYIf branch (rtl_nic.c ->
	 * rtl865x_reinitSwitchCore). For an administratively-up port the switch's
	 * per-port EnablePHYIf bit must stay set; the switch core can silently
	 * clear it on an internal fault, severing the MAC<->PHY interface with
	 * neither a RUNOUT nor a TX-done hang -- a stall both detectors above are
	 * blind to. EnablePHYIf is set at port init and is independent of cable
	 * link state (a carrier loss leaves it set), so no carrier guard is
	 * needed; netif_running() is already enforced at the top of this timer.
	 * Require RTL8196E_PHYIF_LOST_THRESH consecutive clear reads so the brief
	 * clear-then-reenable window of our own deep reset cannot self-trigger,
	 * then escalate to the same FullAndSemiReset the vendor reaches.
	 */
	if (priv->phy_port >= 0 &&
	    !(rtl8196e_readl(PCRP0 + (priv->phy_port << 2)) & EnablePHYIf)) {
		if (++priv->phyif_lost_cnt >= RTL8196E_PHYIF_LOST_THRESH) {
			priv->phyif_lost_cnt = 0;
			netdev_warn(priv->ndev,
				    "switch-core PHY interface lost on port %d\n",
				    priv->phy_port);
			rtl8196e_eth_diag_log(priv, "phyif-lost");
			schedule_work(&priv->swcore_reset_work);
		}
	} else {
		priv->phyif_lost_cnt = 0;
	}

rearm:
	mod_timer(&priv->swcore_check_timer,
		  jiffies + msecs_to_jiffies(RTL8196E_SWCORE_CHECK_MS));
}

/*
 * Reprogram the volatile per-open switch state and start the core: ring
 * bases, PHY, VLAN/NETIF/L2 entries, then rtl8196e_hw_start(). Shared by
 * open() and the deep-reset worker. Does NOT touch NAPI,
 * the eth IRQ mask, or the TX queue — the caller frames those. Returns 0,
 * or a negative errno from PHY/VLAN/NETIF setup (L2-entry failures fall
 * back to trap mode and are not fatal, matching open()'s original flow).
 */
static int rtl8196e_hw_reprogram(struct rtl8196e_priv *priv)
{
	struct net_device *ndev = priv->ndev;
	int ret;

	rtl8196e_hw_set_rx_rings(&priv->hw,
				   rtl8196e_ring_rx_pkthdr_base(priv->ring),
				   rtl8196e_ring_rx_mbuf_base(priv->ring));
	rtl8196e_hw_set_tx_ring(&priv->hw, rtl8196e_ring_tx_desc_base(priv->ring));
	ret = rtl8196e_hw_init_phy(&priv->hw, priv->phy_port, priv->phy_id);
	if (ret)
		return ret;
	ret = rtl8196e_hw_vlan_setup(&priv->hw, priv->vlan_id, 0,
				     priv->portmask | rtl8196e_cpu_port_mask,
				     priv->iface.untag_ports);
	if (ret) {
		netdev_err(ndev, "VLAN setup failed (%d)\n", ret);
		return ret;
	}
	ret = rtl8196e_hw_netif_setup(&priv->hw, ndev->dev_addr,
				      priv->vlan_id, ndev->mtu,
				      priv->portmask | rtl8196e_cpu_port_mask);
	if (ret) {
		netdev_err(ndev, "NETIF setup failed (%d)\n", ret);
		return ret;
	}
	rtl8196e_hw_l2_setup(&priv->hw);
	ret = rtl8196e_hw_l2_add_cpu_entry(&priv->hw, ndev->dev_addr, 0, 0);
	if (ret) {
		priv->l2_check_last = ret;
		priv->l2_check_fail++;
		netdev_warn(ndev, "L2 toCPU entry failed (%d), enabling trap fallback\n", ret);
		rtl8196e_hw_l2_trap_enable(&priv->hw);
	} else {
		ret = rtl8196e_hw_l2_add_bcast_entry(&priv->hw, 0,
						     priv->portmask | rtl8196e_cpu_port_mask);
		if (ret)
			netdev_warn(ndev, "L2 broadcast entry failed (%d)\n", ret);
		ret = rtl8196e_hw_l2_check_cpu_entry(&priv->hw, ndev->dev_addr, 0);
		if (ret) {
			priv->l2_check_last = ret;
			priv->l2_check_fail++;
			netdev_warn(ndev, "L2 toCPU entry verify failed (%d), enabling trap fallback\n", ret);
			rtl8196e_hw_l2_trap_enable(&priv->hw);
		} else {
			priv->l2_check_last = 0;
			priv->l2_check_ok++;
			netdev_dbg(ndev, "L2 toCPU entry verified\n");
		}
	}
	rtl8196e_hw_start(&priv->hw);

	return 0;
}

/*
 * Bring the interface up: program rings, setup VLAN/NETIF/L2, enable IRQs.
 *
 * The one-time SoC bring-up (pinmux, switch-clock toggle, MEMCR,
 * FULL_RST, L2 table clear — ~650 ms of sleeps) runs once at probe
 * (ETH-S03), not here: open() only reprograms the volatile per-open
 * state (ring bases, PHY, VLAN/NETIF/L2 entries) and starts the core,
 * so a down/up cycle costs milliseconds. Note the 0x44 pad write moved
 * with it — nothing re-clears pad muxes at runtime since v2.7, so a
 * single boot-time board-state write is the whole contract.
 */
static int rtl8196e_open(struct net_device *ndev)
{
	struct rtl8196e_priv *priv = netdev_priv(ndev);
	int ret;
	bool link;

	WRITE_ONCE(priv->swcore_recovery_failed, false);
	priv->swcore_retry_count = 0;
	ret = rtl8196e_hw_reprogram(priv);
	if (ret)
		return ret;
	napi_enable(&priv->napi);
	rtl8196e_hw_enable_irqs(&priv->hw);

	netdev_reset_queue(ndev);	/* rings are pristine (stop() reset them) */
	netif_start_queue(ndev);
	link = rtl8196e_hw_link_up(&priv->hw, priv->phy_port);
	if (link)
		netif_carrier_on(ndev);
	else
		netif_carrier_off(ndev);
	if (priv->link_poll_ms)
		mod_timer(&priv->link_timer, jiffies + msecs_to_jiffies(priv->link_poll_ms));

	priv->rx_runout_zero = 0;
	priv->swcore_runout_seen = 0;
	priv->phyif_lost_cnt = 0;
	mod_timer(&priv->swcore_check_timer,
		  jiffies + msecs_to_jiffies(RTL8196E_SWCORE_CHECK_MS));

	return 0;
}

/* Bring the interface down: stop queue, disable IRQs, stop HW, cancel timers. */
static int rtl8196e_stop(struct net_device *ndev)
{
	struct rtl8196e_priv *priv = netdev_priv(ndev);
	unsigned int pkts = 0, bytes = 0;

	netif_stop_queue(ndev);
	timer_delete_sync(&priv->swcore_retry_timer);
	/*
	 * Make sure no deep-reset worker is mid-flight before
	 * we tear down: it touches NAPI, the rings and the queue. The worker
	 * does not take rtnl, so cancel_work_sync() here cannot deadlock.
	 */
	cancel_work_sync(&priv->swcore_reset_work);
	/* A worker that was already running could have armed its retry timer. */
	timer_delete_sync(&priv->swcore_retry_timer);
	/*
	 * A worker that completed inside cancel_work_sync() ends by waking the
	 * queue; re-stop it so no xmit slips into rings we are about to reset.
	 */
	netif_stop_queue(ndev);
	/*
	 * NAPI first: napi_disable() waits out an in-flight
	 * poll, so its napi_complete_done() branch can no longer re-arm
	 * CPUIIMR after we mask below. An RX IRQ landing in the short
	 * window before the mask is acked by the ISR and dropped
	 * (napi_schedule_prep refuses on a disabled NAPI) — harmless on
	 * an interface going down.
	 */
	napi_disable(&priv->napi);
	rtl8196e_hw_disable_irqs(&priv->hw);
	rtl8196e_hw_stop(&priv->hw);
	/* W1C any latched status so a subsequent open() starts clean. */
	rtl8196e_writel(rtl8196e_readl(CPUIISR), CPUIISR);

	/*
	 * Reset both rings before timers go away. Without this, an
	 * `ip link set eth0 down; ip link set eth0 up` cycle under live
	 * traffic would leave TX descriptors with in-flight SKBs and RX
	 * descriptors in arbitrary ownership state — the next open() only
	 * reprograms the ring base addresses and re-enables HW, it does
	 * not rebuild descriptors. NAPI is already quiesced and the HW
	 * IRQ line is masked, so this is the safe window to touch them.
	 */
	if (priv->ring) {
		rtl8196e_ring_tx_reclaim(priv->ring, &pkts, &bytes, 0, NULL);
		if (pkts)
			netdev_completed_queue(ndev, pkts, bytes);
		ndev->stats.tx_dropped += rtl8196e_ring_tx_reset(priv->ring);
		rtl8196e_ring_rx_reset(priv->ring);
	}
	/* NAPI is quiesced — nothing will drain the defer list until open(). */
	skb_queue_purge(&priv->tx_defer_list);
	netdev_reset_queue(ndev);

	timer_delete_sync(&priv->link_timer);
	timer_delete_sync(&priv->tx_reclaim_timer);
	timer_delete_sync(&priv->swcore_check_timer);
	WRITE_ONCE(priv->swcore_recovery_failed, false);
	priv->swcore_retry_count = 0;
	netif_carrier_off(ndev);

	return 0;
}

/* Transmit a packet: linearize if needed, flush data cache, submit to TX ring. */
static __iram netdev_tx_t rtl8196e_start_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct rtl8196e_priv *priv = netdev_priv(ndev);
	bool was_empty = false;
	int ret;
	int free_count;

	if (unlikely(!priv->ring || !priv->portmask)) {
		dev_kfree_skb_any(skb);
		ndev->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}

	if (unlikely(skb_is_nonlinear(skb))) {
		if (skb_linearize(skb)) {
			dev_kfree_skb_any(skb);
			ndev->stats.tx_dropped++;
			return NETDEV_TX_OK;
		}
	}

	/*
	 * Pad short frames to ETH_ZLEN (60 B) before any cache flush or
	 * DMA submission. The TX ring also raises len to ETH_ZLEN as a
	 * defence in depth, but without skb_put_padto() the bytes between
	 * skb->len and ETH_ZLEN come from skb tailroom — uninitialised
	 * slab memory that the switch DMA would happily transmit, leaking
	 * kernel data on the wire.
	 *
	 * skb_put_padto() may consume the skb on -ENOMEM (insufficient
	 * tailroom and reallocation failed); on success skb->len is updated.
	 */
	if (unlikely(skb->len < ETH_ZLEN)) {
		if (skb_put_padto(skb, ETH_ZLEN)) {
			ndev->stats.tx_dropped++;
			return NETDEV_TX_OK;
		}
	}

	/*
	 * Reclaim completed TX descriptors on every xmit call.
	 * With TX_ALL_DONE IRQ disabled, this is the only reclaim path
	 * for pure TX traffic (no RX IRQ to trigger NAPI reclaim).
	 * When RX traffic is present, NAPI poll handles most reclaim
	 * in batch; this call is then a fast no-op (cons == prod).
	 */
	{
		unsigned int rpkts = 0, rbytes = 0;
		rtl8196e_ring_tx_reclaim(priv->ring, &rpkts, &rbytes, 0,
					 &priv->tx_defer_list);
		if (rpkts)
			netdev_completed_queue(ndev, rpkts, rbytes);
	}

	/* Flush packet data — skb->len already covers the (possibly padded) frame. */
	dma_cache_wback_inv((unsigned long)skb->data, skb->len);

	ret = rtl8196e_ring_tx_submit(priv->ring, skb, skb->data, skb->len,
					     priv->vlan_id, priv->portmask,
					     &was_empty);

	if (unlikely(ret < 0)) {
		unsigned int pkts = 0, bytes = 0;

		if (net_ratelimit())
			netdev_warn(ndev, "xmit submit failed (%d), reclaiming\n", ret);
		rtl8196e_ring_tx_reclaim(priv->ring, &pkts, &bytes, 0,
					 &priv->tx_defer_list);
		if (pkts)
			netdev_completed_queue(ndev, pkts, bytes);
		ret = rtl8196e_ring_tx_submit(priv->ring, skb, skb->data, skb->len,
					     priv->vlan_id, priv->portmask,
					     &was_empty);
		if (ret < 0) {
			netif_stop_queue(ndev);
			rtl8196e_arm_tx_reclaim(priv);
			return NETDEV_TX_BUSY;
		}
	}

	/* BQL (ETH-S05): account after the submit is final (a BUSY return
	 * above hands the skb back to the stack unaccounted). Completion
	 * sides: the three reclaim sites and the reset paths. */
	netdev_sent_queue(ndev, skb->len);

	rtl8196e_ring_kick_tx(priv->ring, was_empty);

	/*
	 * Drain any coalesced-but-unpulsed kick at the end of a dequeue batch
	 * (xmit_more clear). Coalescing can otherwise leave a descriptor flipped
	 * SWCORE_OWNED after the TX engine already parked, with no pulse to wake
	 * it; on an RX-silent link nothing else drains it and the tx-done
	 * watchdog eventually deep-resets the switch on a phantom "stuck TX".
	 * Draining at the batch boundary bounds this to the current burst while
	 * still coalescing within a batch under load.
	 */
	if (!netdev_xmit_more())
		rtl8196e_ring_kick_drain(priv->ring);

	ndev->stats.tx_packets++;
	ndev->stats.tx_bytes += skb->len;
	u64_stats_update_begin(&priv->stats64.syncp);
	u64_stats_inc(&priv->stats64.tx_packets);
	u64_stats_add(&priv->stats64.tx_bytes, skb->len);
	u64_stats_update_end(&priv->stats64.syncp);

	free_count = rtl8196e_ring_tx_free_count(priv->ring);
	if (unlikely(free_count < RTL8196E_TX_STOP_THRESH))
		netif_stop_queue(ndev);

	/*
	 * If this xmit left the queue stopped — driver ring-full XOFF above or
	 * BQL byte-limit XOFF from netdev_sent_queue() — arm the software
	 * reclaim timer so a no-RX stall cannot wait for the 10 s watchdog.
	 */
	if (unlikely(netif_xmit_stopped(netdev_get_tx_queue(ndev, 0)))) {
		priv->tx_xoff_seen_xmit++;
		rtl8196e_arm_tx_reclaim(priv);
	}

	return NETDEV_TX_OK;
}

/*
 * Full HW + ring resync: stop the switch engines, reclaim and reset the TX ring,
 * reset the RX ring (re-arm every descriptor SWCORE_OWNED, rx_idx = 0), reprogram
 * both ring bases and restart. hw_stop()/hw_start() cycles the switch RX engine
 * (TRXRDY) back to descriptor 0, so the RX ring MUST be resynced in lockstep:
 * rebuilding only TX leaves rx_idx desynced from the switch's RX pointer, the
 * switch then sees no usable descriptors and asserts PKTHDR_DESC_RUNOUT
 * continuously — the self-sustaining interrupt storm. Shared recovery body used
 * by the poll-side RUNOUT-storm detector (the TX watchdog now escalates through
 * the deep-reset worker instead). The caller owns NAPI quiescing and the IRQ
 * mask/enable framing;
 * this routine touches only the rings and the switch engine and leaves the eth
 * IRQ in the (masked) state the caller set.
 */
static void rtl8196e_hw_ring_resync(struct rtl8196e_priv *priv)
{
	unsigned int pkts = 0, bytes = 0;

	rtl8196e_hw_stop(&priv->hw);
	rtl8196e_ring_tx_reclaim(priv->ring, &pkts, &bytes, 0, NULL);
	priv->ndev->stats.tx_dropped += rtl8196e_ring_tx_reset(priv->ring);
	rtl8196e_ring_rx_reset(priv->ring);
	/* Rings rebuilt from scratch — restart BQL accounting with them. */
	netdev_reset_queue(priv->ndev);
	rtl8196e_hw_set_tx_ring(&priv->hw, rtl8196e_ring_tx_desc_base(priv->ring));
	rtl8196e_hw_set_rx_rings(&priv->hw,
				 rtl8196e_ring_rx_pkthdr_base(priv->ring),
				 rtl8196e_ring_rx_mbuf_base(priv->ring));
	rtl8196e_hw_start(&priv->hw);
}

/*
 * TX watchdog handler. dev_watchdog() calls this from a timer softirq while
 * holding dev->tx_global_lock, so it MUST NOT sleep. The previous inline body
 * called napi_disable(), which in 6.x takes the netdev mutex and then
 * usleep_range()s while NAPI_STATE_SCHED is set — a sleeping-in-atomic bug that
 * fires precisely under load (napi_defer_hard_irqs keeps SCHED set across the
 * gro_flush_timeout window), i.e. exactly when a TX timeout is most likely.
 *
 * Keep this callback atomic: stop the queue, record the fingerprint, and hand
 * the ring/switch recovery to the deep-reset worker. Reusing swcore_reset_work
 * (rather than a second work item) means the workqueue serializes it against
 * the other recovery sites — only one reset can run at a time. The worker
 * reclaims and resets both rings, reprograms the switch, and wakes the queue;
 * the 10 s watchdog gives it ample margin over its ~650 ms cost.
 */
static void rtl8196e_tx_timeout(struct net_device *ndev, unsigned int txqueue)
{
	struct rtl8196e_priv *priv = netdev_priv(ndev);

	netdev_warn(ndev, "TX timeout\n");

	if (!priv->ring)
		return;
	if (READ_ONCE(priv->swcore_recovery_failed))
		return;

	rtl8196e_eth_diag_log(priv, "tx-timeout");
	netif_stop_queue(ndev);
	schedule_work(&priv->swcore_reset_work);
}

/*
 * Deep switch-core reset, run from a
 * workqueue because rtl8196e_hw_swcore_reset() sleeps ~650 ms (clock cycle +
 * FULL_RST) — illegal in the NAPI poll / timer softirq contexts that trigger
 * it. This is the depth of the vendor rtl865x_reinitSwitchCore: a full
 * FullAndSemiReset of the switch silicon, not just the RX/TX ring-pointer
 * resync that rtl8196e_hw_ring_resync() does. Escalated to when the shallow
 * in-poll resync repeatedly fails to clear the storm (rx_runout_resync_run),
 * or the periodic TX-done watchdog finds the switch core wedged (tx_hang_cnt).
 * stop() runs cancel_work_sync() before it resets/frees the rings, so the ring
 * is valid here; netif_running() guards against a down that began first.
 */
static void rtl8196e_swcore_reset_work_fn(struct work_struct *work)
{
	struct rtl8196e_priv *priv =
		container_of(work, struct rtl8196e_priv, swcore_reset_work);
	struct net_device *ndev = priv->ndev;
	unsigned int pkts = 0, bytes = 0;
	int ret;

	if (!netif_running(ndev) || !priv->ring)
		return;

	netif_stop_queue(ndev);
	napi_disable(&priv->napi);
	rtl8196e_hw_disable_irqs(&priv->hw);
	rtl8196e_hw_stop(&priv->hw);

	rtl8196e_ring_tx_reclaim(priv->ring, &pkts, &bytes, 0, NULL);
	ndev->stats.tx_dropped += rtl8196e_ring_tx_reset(priv->ring);
	rtl8196e_ring_rx_reset(priv->ring);
	netdev_reset_queue(ndev);

	/* The deep part our ring resync lacks: reset the switch silicon. A
	 * best-effort L2-clear failure here is warned, not fatal — the real
	 * "did the reset take" check is hw_reprogram (PHY/VLAN/NETIF/L2 entry). */
	ret = rtl8196e_hw_swcore_reset(&priv->hw);
	if (ret)
		netdev_warn(ndev, "switch core reset: L2 clear failed (%d), continuing\n", ret);
	/* The reset rewrote LEDCREG; restore the operator's led_mode. */
	ret = rtl8196e_apply_led_mode(priv, READ_ONCE(priv->led_mode));
	if (ret)
		netdev_warn(ndev, "switch core reset: LED mode replay failed (%d)\n",
			    ret);
	ret = rtl8196e_hw_reprogram(priv);

	if (ret) {
		unsigned int delay_ms;

		/*
		 * Publish the hold-down state before re-enabling NAPI. Timer kicks,
		 * link polling and NAPI completion all consult this latch, so none of
		 * them can re-enable carrier or device IRQs on a partial setup.
		 */
		WRITE_ONCE(priv->swcore_recovery_failed, true);
		napi_enable(&priv->napi);
		netif_carrier_off(ndev);
		priv->swcore_reprogram_fail++;
		if (priv->swcore_retry_count < RTL8196E_RESET_RETRY_MAX) {
			delay_ms = RTL8196E_RESET_RETRY_BASE_MS <<
				   priv->swcore_retry_count;
			priv->swcore_retry_count++;
			mod_timer(&priv->swcore_retry_timer,
				  jiffies + msecs_to_jiffies(delay_ms));
			netdev_err(ndev,
				   "switch core reset: reprogram failed (%d), retry %u/%u in %u ms (#%u)\n",
				   ret, priv->swcore_retry_count,
				   RTL8196E_RESET_RETRY_MAX, delay_ms,
				   priv->swcore_reprogram_fail);
		} else {
			netdev_err(ndev,
				   "switch core reset: reprogram failed (%d), retry budget exhausted, interface held down (#%u)\n",
				   ret, priv->swcore_reprogram_fail);
		}
		return;
	}

	WRITE_ONCE(priv->swcore_recovery_failed, false);
	priv->swcore_retry_count = 0;
	timer_delete_sync(&priv->swcore_retry_timer);
	/* Balance napi_disable() only after the recovery state is final. */
	napi_enable(&priv->napi);
	rtl8196e_hw_enable_irqs(&priv->hw);
	netif_wake_queue(ndev);

	priv->swcore_deep_reset++;
	priv->rx_runout_resync_run = 0;
	priv->tx_hang_cnt = 0;
	priv->rx_stall_run = 0;
	priv->phyif_lost_cnt = 0;

	netdev_warn(ndev, "switch core reset done (#%u)\n", priv->swcore_deep_reset);
}

/* NAPI poll: drain RX ring up to budget, reclaim completed TX, wake queue if stalled. */
static __iram int rtl8196e_poll(struct napi_struct *napi, int budget)
{
	struct rtl8196e_priv *priv = container_of(napi, struct rtl8196e_priv, napi);
	unsigned int pkts = 0, bytes = 0;
	int delivered = 0;
	unsigned int delivered_bytes = 0;
	/*
	 * work_done now counts descriptors PROCESSED (the bounded-loop return),
	 * not packets delivered; `delivered` carries the packet count. The
	 * budget/complete/return gates below want the processed count, so they
	 * keep using work_done unchanged.
	 */
	int work_done;

	if (unlikely(READ_ONCE(priv->swcore_recovery_failed))) {
		napi_complete_done(napi, 0);
		return 0;
	}

	/* record-v8 activity taps — see the ISR-side comment. */
	priv->poll_cnt++;
	priv->poll_last_j = (u32)jiffies;

	work_done = rtl8196e_ring_rx_poll(priv->ring, budget, napi, priv->ndev,
					  &delivered, &delivered_bytes);
	priv->poll_delivered += (u32)delivered;
	/* 64-bit RX totals for get_stats64 (aggregated once per poll). */
	u64_stats_update_begin(&priv->stats64.syncp);
	u64_stats_add(&priv->stats64.rx_packets, delivered);
	u64_stats_add(&priv->stats64.rx_bytes, delivered_bytes);
	u64_stats_update_end(&priv->stats64.syncp);

	rtl8196e_ring_tx_reclaim(priv->ring, &pkts, &bytes, budget, NULL);
	if (pkts)
		netdev_completed_queue(priv->ndev, pkts, bytes);

	/*
	 * Free the skbs start_xmit's reclaim parked on the defer list —
	 * here they get the real-budget napi_consume_skb() path instead of
	 * the dev_consume_skb_any() they would have taken inline in xmit.
	 */
	if (!skb_queue_empty(&priv->tx_defer_list)) {
		struct sk_buff *dskb;

		while ((dskb = __skb_dequeue(&priv->tx_defer_list)))
			napi_consume_skb(dskb, budget);
	}

	/* Flush any deferred kick_tx pulses before going idle. */
	rtl8196e_ring_kick_drain(priv->ring);

	if (unlikely(pkts && netif_queue_stopped(priv->ndev))) {
		int free_count = rtl8196e_ring_tx_free_count(priv->ring);

		if (free_count >= RTL8196E_TX_WAKE_THRESH)
			netif_wake_queue(priv->ndev);
	}

	/*
	 * If the queue is still stopped after this reclaim (DRV ring-full XOFF
	 * or BQL byte-limit XOFF), nothing reclaims the rest without RX — keep
	 * the software timer going. Once the queue is woken / un-frozen,
	 * netif_xmit_stopped() is false and the timer lapses.
	 */
	if (unlikely(netif_xmit_stopped(netdev_get_tx_queue(priv->ndev, 0)))) {
		priv->tx_xoff_seen_poll++;
		rtl8196e_arm_tx_reclaim(priv);
	}

	/*
	 * A poll that did zero RX work while
	 * PKTHDR_DESC_RUNOUT is (re)asserted means the switch RX pointer and
	 * rx_idx have desynced — the ISR/poll/re-enable handshake would spin
	 * forever (a CPU-pinning interrupt storm). The poll is the one context
	 * that runs on every storm iteration, so after a few consecutive such
	 * polls resync the ring from here; the napi_complete_done() tail below
	 * then re-enables IRQs against an armed, in-sync ring and the storm
	 * cannot restart. CPUIISR is read only on a zero-work poll (the &&
	 * short-circuits), so the normal RX path pays nothing.
	 */
	/*
	 * Vendor parity: the vendor swNic
	 * receive path W1Cs PKTHDR/MBUF_DESC_RUNOUT as it refills descriptors,
	 * so a transient runout self-clears as the driver makes progress. Our
	 * napi_complete tail already W1Cs runout when the poll completes
	 * (work_done < budget); the only case a latched runout survives a
	 * *productive* poll is a budget-saturating poll (work_done == budget),
	 * which returns without completing and gets re-polled. Clear runout
	 * exactly there. Gating on == budget (not > 0) is deliberate: it skips
	 * the light TCP-ACK polls during a TX flow, whose extra per-poll MMIO
	 * write measurably cost ~2 Mbit/s TX on this Lexra CPU, while still
	 * covering the heavy-RX case the clear is for.
	 */
	/*
	 * budget==0 (netpoll) must not be mistaken for saturation. A poll that
	 * filled the whole budget gets the RUNOUT W1C (vendor-parity clear-on-
	 * progress) AND drives the drop-flood stall detector: a saturating poll
	 * that delivered NOTHING is the signature of the soft-lockup (the bounded
	 * loop in rtl8196e_ring_rx_poll now lets it return instead of pinning the
	 * CPU). After RTL8196E_RX_STALL_THRESH such polls in a row — independent of
	 * PKTHDR_DESC_RUNOUT — escalate to the switch-core deep reset. Any
	 * productive poll
	 * resets the run; the non-saturating common case touches rx_stall_run
	 * only when it is actually non-zero, so the hot path adds no store.
	 */
	if (budget && work_done == budget) {
		priv->poll_budget_hit++;
		rtl8196e_writel(PKTHDR_DESC_RUNOUT_IP_ALL | MBUF_DESC_RUNOUT_IP_ALL, CPUIISR);
		if (delivered == 0) {
			struct rtl8196e_ring_diag d;

			/*
			 * Snapshot the structural-desync counters at the start
			 * of a stall run. These (wild/no-shadow/rearm-badidx
			 * descriptor pointers) grow only on a genuine switch
			 * desync — the one thing a deep reset can fix. A runt/
			 * bad-length or OOM flood instead advances the cursor
			 * and drops frames; a reset there just adds a 650 ms
			 * outage without stopping the flood.
			 */
			if (priv->rx_stall_run == 0) {
				rtl8196e_ring_diag_get(priv->ring, &d);
				priv->rx_stall_a_base = d.rx_wild_pkthdr +
					d.rx_wild_mbuf + d.rx_mbuf_no_shadow +
					d.rx_no_skb + d.rx_rearm_badidx;
			}
			if (rtl8196e_rx_stall_thresh &&
			    ++priv->rx_stall_run >= rtl8196e_rx_stall_thresh) {
				u32 a_now;

				rtl8196e_ring_diag_get(priv->ring, &d);
				a_now = d.rx_wild_pkthdr + d.rx_wild_mbuf +
					d.rx_mbuf_no_shadow + d.rx_no_skb +
					d.rx_rearm_badidx;
				if (a_now != priv->rx_stall_a_base) {
					netdev_warn(priv->ndev,
						    "RX poll saturating with no delivery, switch desync — resetting switch core\n");
					rtl8196e_eth_diag_log(priv, "rx-poll-stall");
					schedule_work(&priv->swcore_reset_work);
				} else if (net_ratelimit()) {
					netdev_warn(priv->ndev,
						    "RX poll saturating with no delivery (drop flood) — not resetting\n");
				}
				priv->rx_stall_run = 0;
			}
		} else {
			priv->rx_stall_run = 0;
		}
	} else if (priv->rx_stall_run) {
		priv->rx_stall_run = 0;
	}

	if (unlikely(delivered == 0 &&
		     (rtl8196e_readl(CPUIISR) & PKTHDR_DESC_RUNOUT_IP_ALL))) {
		if (++priv->rx_runout_zero >= RTL8196E_RUNOUT_RESYNC_THRESH) {
			netif_stop_queue(priv->ndev);
			rtl8196e_hw_ring_resync(priv);
			netif_wake_queue(priv->ndev);
			priv->rx_runout_resync++;
			priv->rx_runout_zero = 0;
			/*
			 * A ring resync that keeps firing
			 * without a productive poll in between means the storm is
			 * not a mere ring-pointer desync the shallow resync can
			 * fix — escalate to a full switch-core deep reset (from
			 * process context: it sleeps ~650 ms). Announce the cause
			 * once, exactly at the threshold crossing (== not >=), so
			 * the storm does not spam the log before the worker runs.
			 */
			if (++priv->rx_runout_resync_run == RTL8196E_DEEP_RESET_THRESH) {
				netdev_warn(priv->ndev,
					    "RX runout not clearing after %u resyncs\n",
					    priv->rx_runout_resync_run);
				rtl8196e_eth_diag_log(priv, "runout-deep-reset");
				schedule_work(&priv->swcore_reset_work);
			}
		}
	} else {
		priv->rx_runout_zero = 0;
		priv->rx_runout_resync_run = 0;
	}

	if (work_done < budget) {
		if (napi_complete_done(napi, work_done)) {
			rtl8196e_writel(PKTHDR_DESC_RUNOUT_IP_ALL | MBUF_DESC_RUNOUT_IP_ALL, CPUIISR);
			if (likely(!READ_ONCE(priv->swcore_recovery_failed)))
				rtl8196e_hw_enable_irqs(&priv->hw);
		}
	}

	return work_done;
}

/* Interrupt handler: read and clear CPUIISR, update link state, schedule NAPI. */
static __iram irqreturn_t rtl8196e_isr(int irq, void *dev_id)
{
	struct net_device *ndev = dev_id;
	struct rtl8196e_priv *priv = netdev_priv(ndev);
	u32 status;
	u32 mask;
	u32 now_j;
	bool link;

	/*
	 * record-v8 activity taps: invocation count, last-entry stamp, and the
	 * per-jiffy burst high-water. A legitimate ISR rate is bounded by NAPI
	 * masking after the first RX IRQ (well under one entry per jiffy under
	 * load); a level-line storm re-entering per eret would drive
	 * isr_burst_max to the thousands — a witness it cannot dodge. Word
	 * stores only, next to the two MMIO reads below.
	 */
	now_j = (u32)jiffies;
	priv->isr_cnt++;
	priv->isr_last_j = now_j;
	if (priv->isr_burst_j == now_j) {
		if (++priv->isr_burst > priv->isr_burst_max)
			priv->isr_burst_max = priv->isr_burst;
	} else {
		priv->isr_burst_j = now_j;
		priv->isr_burst = 1;
	}

	status = rtl8196e_readl(CPUIISR);
	mask = rtl8196e_readl(CPUIIMR);
	status &= mask;
	if (unlikely(!status))
		return IRQ_NONE;
	/* W1C only the bits we actually own, so masked-but-latched bits stay
	 * pending for whoever armed them (or stay sticky until someone does). */
	rtl8196e_writel(status, CPUIISR);

	if (unlikely(status & LINK_CHANGE_IP)) {
		link = rtl8196e_hw_link_up(&priv->hw, priv->phy_port);
		if (link)
			netif_carrier_on(ndev);
		else
			netif_carrier_off(ndev);
	}

	if (likely(status & (RX_DONE_IP_ALL | PKTHDR_DESC_RUNOUT_IP_ALL)))
		rtl8196e_napi_kick(priv);

	return IRQ_HANDLED;
}

/*
 * Refuse a MAC change while the interface is UP: the new address would not
 * propagate to the NETIF table nor to the hashed L2 "toCPU" entry, silently
 * breaking unicast delivery. The next open() reprograms both from dev_addr.
 */
static int rtl8196e_set_mac_address(struct net_device *ndev, void *p)
{
	if (netif_running(ndev))
		return -EBUSY;
	return eth_mac_addr(ndev, p);
}

/*
 * Same F2 pattern for the MTU: the NETIF table holds the
 * open-time value, so a live change would update ndev->mtu without the
 * hardware following — a silent inconsistency. Refuse while UP; the
 * core has already clamped new_mtu to [min_mtu, max_mtu], and the next
 * open() programs the NETIF table from ndev->mtu.
 */
static int rtl8196e_change_mtu(struct net_device *ndev, int new_mtu)
{
	if (netif_running(ndev))
		return -EBUSY;
	WRITE_ONCE(ndev->mtu, new_mtu);
	return 0;
}

/*
 * Report 64-bit packet/byte totals from the priv accumulators so userspace
 * (`ip -s link`) does not see the ndev->stats 32-bit fields wrap in days at
 * line rate. Error/drop counters stay in ndev->stats (rare, wrap is a
 * non-issue). u64_stats_sync provides a coherent snapshot on this 32-bit CPU.
 */
static void rtl8196e_get_stats64(struct net_device *ndev,
				 struct rtnl_link_stats64 *s)
{
	struct rtl8196e_priv *priv = netdev_priv(ndev);
	unsigned int start;

	do {
		start = u64_stats_fetch_begin(&priv->stats64.syncp);
		s->rx_packets = u64_stats_read(&priv->stats64.rx_packets);
		s->rx_bytes   = u64_stats_read(&priv->stats64.rx_bytes);
		s->tx_packets = u64_stats_read(&priv->stats64.tx_packets);
		s->tx_bytes   = u64_stats_read(&priv->stats64.tx_bytes);
	} while (u64_stats_fetch_retry(&priv->stats64.syncp, start));
	s->rx_errors        = ndev->stats.rx_errors;
	s->tx_errors        = ndev->stats.tx_errors;
	s->rx_dropped       = ndev->stats.rx_dropped;
	s->tx_dropped       = ndev->stats.tx_dropped;
	s->rx_length_errors = ndev->stats.rx_length_errors;
}

static const struct net_device_ops rtl8196e_netdev_ops = {
	.ndo_open = rtl8196e_open,
	.ndo_stop = rtl8196e_stop,
	.ndo_get_stats64 = rtl8196e_get_stats64,
	.ndo_start_xmit = rtl8196e_start_xmit,
	.ndo_tx_timeout = rtl8196e_tx_timeout,
	.ndo_set_mac_address = rtl8196e_set_mac_address,
	.ndo_change_mtu = rtl8196e_change_mtu,
};

/* ethtool: identify the driver and the underlying platform device. */
static void rtl8196e_get_drvinfo(struct net_device *ndev,
				 struct ethtool_drvinfo *info)
{
	strscpy(info->driver, RTL8196E_DRV_NAME, sizeof(info->driver));
	strscpy(info->version, RTL8196E_DRV_VERSION, sizeof(info->version));
	strscpy(info->fw_version, "n/a", sizeof(info->fw_version));
	if (ndev->dev.parent)
		strscpy(info->bus_info, dev_name(ndev->dev.parent),
			sizeof(info->bus_info));
	else
		strscpy(info->bus_info, "platform",
			sizeof(info->bus_info));
}

/*
 * ethtool: report the actual link state from the switch port status register.
 * The 4 RTL8196E PHYs are managed internally by the switch ASIC; we expose
 * what PSRPx says (10/100, half/full) rather than pretending it is a vanilla
 * MII PHY. Autoneg is always on at the PHY level — the driver never disables
 * it — so we advertise/report TP medium with autoneg enabled.
 */
static int rtl8196e_get_link_ksettings(struct net_device *ndev,
				       struct ethtool_link_ksettings *cmd)
{
	struct rtl8196e_priv *priv = netdev_priv(ndev);
	bool link = false, full_duplex = false;
	int speed = -1;

	rtl8196e_hw_link_status(&priv->hw, priv->phy_port,
				&link, &speed, &full_duplex);

	ethtool_link_ksettings_zero_link_mode(cmd, supported);
	ethtool_link_ksettings_add_link_mode(cmd, supported, 10baseT_Half);
	ethtool_link_ksettings_add_link_mode(cmd, supported, 10baseT_Full);
	ethtool_link_ksettings_add_link_mode(cmd, supported, 100baseT_Half);
	ethtool_link_ksettings_add_link_mode(cmd, supported, 100baseT_Full);
	ethtool_link_ksettings_add_link_mode(cmd, supported, Autoneg);
	ethtool_link_ksettings_add_link_mode(cmd, supported, TP);

	ethtool_link_ksettings_zero_link_mode(cmd, advertising);
	ethtool_link_ksettings_add_link_mode(cmd, advertising, 10baseT_Half);
	ethtool_link_ksettings_add_link_mode(cmd, advertising, 10baseT_Full);
	ethtool_link_ksettings_add_link_mode(cmd, advertising, 100baseT_Half);
	ethtool_link_ksettings_add_link_mode(cmd, advertising, 100baseT_Full);
	ethtool_link_ksettings_add_link_mode(cmd, advertising, Autoneg);
	ethtool_link_ksettings_add_link_mode(cmd, advertising, TP);

	cmd->base.port = PORT_TP;
	cmd->base.phy_address = priv->phy_id;
	cmd->base.autoneg = AUTONEG_ENABLE;
	cmd->base.speed = link && speed > 0 ? speed : SPEED_UNKNOWN;
	cmd->base.duplex = link ? (full_duplex ? DUPLEX_FULL : DUPLEX_HALF)
				: DUPLEX_UNKNOWN;
	return 0;
}

#define RTL8196E_ETHTOOL_STATS_COUNT 36

/* ethtool: return the number of driver-specific statistics. */
static int rtl8196e_get_sset_count(struct net_device *ndev, int sset)
{
	(void)ndev;
	if (sset == ETH_SS_STATS)
		return RTL8196E_ETHTOOL_STATS_COUNT;
	return -EOPNOTSUPP;
}

/* ethtool: fill the statistics name strings array. */
static void rtl8196e_get_strings(struct net_device *ndev, u32 sset, u8 *data)
{
	static const char stats[RTL8196E_ETHTOOL_STATS_COUNT][ETH_GSTRING_LEN] = {
		"rtl8196e_l2_check_ok",
		"rtl8196e_l2_check_fail",
		"rtl8196e_l2_check_last_result",
		"rtl8196e_tx_kicks_total",
		"rtl8196e_tx_kicks_cold",
		"rtl8196e_tx_kicks_threshold",
		"rtl8196e_tx_kicks_drain",
		/* Ring anomaly counters — must stay 0 in nominal flow. */
		"rtl8196e_rx_wild_pkthdr",
		"rtl8196e_rx_wild_mbuf",
		"rtl8196e_rx_bad_len",
		"rtl8196e_rx_no_skb",
		"rtl8196e_rx_alloc_fail",
		"rtl8196e_rx_rearm_badidx",
		"rtl8196e_tx_bad_args",
		"rtl8196e_tx_bad_len",
		"rtl8196e_tx_ring_full",
		"rtl8196e_tx_reclaim_no_skb",
		"rtl8196e_tx_bad_pkthdr",
		"rtl8196e_tx_bad_mbuf",
		"rtl8196e_rx_mbuf_no_shadow",
		/* Switch-core recovery counters — stay 0 unless a storm hit. */
		"rtl8196e_rx_runout_resync",
		"rtl8196e_rx_runout_kick",
		/* Deep switch-core resets — stay 0 unless escalated. */
		"rtl8196e_swcore_deep_reset",
		/* RX pkthdr/mbuf index skew probe (saturation-only). */
		"rtl8196e_rx_pkthdr_mbuf_skew",
		/* Drop-flood stall detector observability. */
		"rtl8196e_poll_budget_hit",
		"rtl8196e_rx_stall_run",
		/* xmit_stopped hint observability (likely/unlikely measurement). */
		"rtl8196e_tx_xoff_seen_xmit",
		"rtl8196e_tx_xoff_seen_poll",
		/* Deferred TX skb frees (xmit-context reclaim -> poll drain). */
		"rtl8196e_tx_defer_queued",
		"rtl8196e_tx_defer_direct",
		/* record-v8 activity taps (issue #99 storm forensics). */
		"rtl8196e_isr_cnt",
		"rtl8196e_isr_burst_max",
		"rtl8196e_poll_cnt",
		"rtl8196e_poll_delivered",
		"rtl8196e_kick_txreclaim",
		/* Deep resets whose hardware reprogram failed (interface held down). */
		"rtl8196e_swcore_reprogram_fail",
	};

	(void)ndev;
	if (sset != ETH_SS_STATS)
		return;

	memcpy(data, stats, sizeof(stats));
}

/* ethtool: fill the statistics values array. */
static void rtl8196e_get_ethtool_stats(struct net_device *ndev,
				       struct ethtool_stats *stats, u64 *data)
{
	struct rtl8196e_priv *priv = netdev_priv(ndev);
	u32 cold = 0, thresh = 0, drain = 0, total = 0;
	struct rtl8196e_ring_diag diag = {};

	(void)stats;
	data[0] = priv->l2_check_ok;
	data[1] = priv->l2_check_fail;
	data[2] = priv->l2_check_last;
	if (priv->ring) {
		rtl8196e_ring_kick_stats_get(priv->ring, &cold, &thresh, &drain, &total);
		rtl8196e_ring_diag_get(priv->ring, &diag);
	}
	data[3] = total;
	data[4] = cold;
	data[5] = thresh;
	data[6] = drain;
	data[7] = diag.rx_wild_pkthdr;
	data[8] = diag.rx_wild_mbuf;
	data[9] = diag.rx_bad_len;
	data[10] = diag.rx_no_skb;
	data[11] = diag.rx_alloc_fail;
	data[12] = diag.rx_rearm_badidx;
	data[13] = diag.tx_bad_args;
	data[14] = diag.tx_bad_len;
	data[15] = diag.tx_ring_full;
	data[16] = diag.tx_reclaim_no_skb;
	data[17] = diag.tx_bad_pkthdr;
	data[18] = diag.tx_bad_mbuf;
	data[19] = diag.rx_mbuf_no_shadow;
	data[20] = priv->rx_runout_resync;
	data[21] = priv->rx_runout_kick;
	data[22] = priv->swcore_deep_reset;
	data[23] = diag.rx_pkthdr_mbuf_skew;
	data[24] = priv->poll_budget_hit;
	data[25] = priv->rx_stall_run;
	data[26] = priv->tx_xoff_seen_xmit;
	data[27] = priv->tx_xoff_seen_poll;
	data[28] = diag.tx_defer_queued;
	data[29] = diag.tx_defer_direct;
	data[30] = priv->isr_cnt;
	data[31] = priv->isr_burst_max;
	data[32] = priv->poll_cnt;
	data[33] = priv->poll_delivered;
	data[34] = priv->kick_txreclaim;
	data[35] = priv->swcore_reprogram_fail;
}

static const struct ethtool_ops rtl8196e_ethtool_ops = {
	.get_drvinfo = rtl8196e_get_drvinfo,
	.get_link = ethtool_op_get_link,
	.get_link_ksettings = rtl8196e_get_link_ksettings,
	.get_sset_count = rtl8196e_get_sset_count,
	.get_strings = rtl8196e_get_strings,
	.get_ethtool_stats = rtl8196e_get_ethtool_stats,
};

/*
 * led_mode sysfs attribute: "bright" (default), "dim", or "off".
 *
 * "bright": LEDCREG = LEDMODE_DIRECT, DIRECTLCR = default (full brightness).
 * "dim":    LEDCREG = 0 (scan mode, ~25% brightness).
 * "off":    LED pad muxed to GPIO and held inactive (physical high).
 *
 * The LAN LED uses an ASIC LED_PORTn output for BRIGHT/DIM (B6 on the
 * Lidl, B2 on the Sengled G4). OFF disconnects that pad from the ASIC
 * because DIRECTLCR=0 is only the minimum duty scale, not an output disable.
 * The STATUS LED (GPIO-driven) should be set to 255, 60, or 0 to match.
 */
static ssize_t led_mode_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct rtl8196e_priv *priv = netdev_priv(to_net_dev(dev));

	switch (READ_ONCE(priv->led_mode)) {
	case RTL8196E_LED_OFF:
		return sysfs_emit(buf, "off\n");
	case RTL8196E_LED_DIM:
		return sysfs_emit(buf, "dim\n");
	case RTL8196E_LED_BRIGHT:
	default:
		return sysfs_emit(buf, "bright\n");
	}
}

static ssize_t led_mode_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct rtl8196e_priv *priv = netdev_priv(to_net_dev(dev));
	u8 mode;
	int ret;

	if (sysfs_streq(buf, "bright"))
		mode = RTL8196E_LED_BRIGHT;
	else if (sysfs_streq(buf, "dim"))
		mode = RTL8196E_LED_DIM;
	else if (sysfs_streq(buf, "off"))
		mode = RTL8196E_LED_OFF;
	else
		return -EINVAL;

	ret = rtl8196e_apply_led_mode(priv, mode);
	if (ret)
		return ret;

	/* Record only successfully applied intent for deep-reset replay. */
	WRITE_ONCE(priv->led_mode, mode);

	return count;
}

static DEVICE_ATTR_RW(led_mode);

/*
 * kick_threshold sysfs attribute: number of TX submits to coalesce before
 * pulsing CPUICR.TXFD. Mirrors the rtl8196e_kick_threshold module-param
 * but is writable at runtime — operators can sweep values under live
 * traffic without rebooting. Range 1..64; 1 disables coalescing.
 */
static ssize_t kick_threshold_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", READ_ONCE(rtl8196e_kick_threshold));
}

static ssize_t kick_threshold_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	unsigned int v;
	int ret;

	ret = kstrtouint(buf, 0, &v);
	if (ret)
		return ret;
	if (v < 1 || v > 64)
		return -EINVAL;

	WRITE_ONCE(rtl8196e_kick_threshold, v);
	return count;
}

static DEVICE_ATTR_RW(kick_threshold);

static struct attribute *rtl8196e_sysfs_attrs[] = {
	&dev_attr_led_mode.attr,
	&dev_attr_kick_threshold.attr,
	NULL,
};

static const struct attribute_group rtl8196e_sysfs_group = {
	.attrs = rtl8196e_sysfs_attrs,
};

/*
 * Claim the board-declared active-low LAN LED pad as a GPIO and derive its
 * PIN_MUX_SEL_2 field. The descriptor leaves the pad at logical zero
 * (physical high/off) whenever the ASIC is disconnected. BRIGHT and DIM
 * temporarily route the requested pad back to LED_PORTn.
 */
static int rtl8196e_lan_led_init(struct device *dev,
				 struct rtl8196e_priv *priv)
{
	struct of_phandle_args args;
	const char *name = NULL;
	bool listed = false;
	u32 line, u;
	int ret;

	if (!of_property_present(dev->of_node, "lan-led-gpios")) {
		dev_warn(dev, "lan-led-gpios absent; LED off mode unavailable\n");
		return 0;
	}

	ret = of_parse_phandle_with_args(dev->of_node, "lan-led-gpios",
					 "#gpio-cells", 0, &args);
	if (ret)
		return dev_err_probe(dev, ret, "cannot parse lan-led-gpios\n");
	if (args.args_count < 1) {
		of_node_put(args.np);
		return dev_err_probe(dev, -EINVAL,
				     "lan-led-gpios has no line number\n");
	}
	line = args.args[0];
	if (line < 10 || line > 14) {
		of_node_put(args.np);
		return dev_err_probe(dev, -EINVAL,
				     "LAN LED GPIO %u is not a B2-B6 LED pad\n",
				     line);
	}

	of_property_for_each_u32(args.np, "realtek,led-pads", u)
		if (u == line)
			listed = true;
	of_property_read_string_index(args.np, "gpio-line-names", line, &name);
	of_node_put(args.np);

	if (!listed)
		return dev_err_probe(dev, -EINVAL,
				     "LAN LED GPIO %u is absent from realtek,led-pads\n",
				     line);
	if (name && *name)
		return dev_err_probe(dev, -EINVAL,
				     "LAN LED GPIO %u is also named '%s'\n",
				     line, name);

	priv->lan_led_gpio = devm_gpiod_get(dev, "lan-led", GPIOD_OUT_LOW);
	if (IS_ERR(priv->lan_led_gpio))
		return dev_err_probe(dev, PTR_ERR(priv->lan_led_gpio),
				     "cannot claim LAN LED GPIO\n");
	if (!gpiod_is_active_low(priv->lan_led_gpio))
		return dev_err_probe(dev, -EINVAL,
				     "lan-led-gpios must be active-low\n");

	priv->lan_led_mux_mask = 3U << ((line - 10) * 3);
	return 0;
}

/* Platform probe: allocate netdev, parse DT, create ring, request IRQ, register. */
static int rtl8196e_probe(struct platform_device *pdev)
{
	struct net_device *ndev;
	struct rtl8196e_priv *priv;
	int ret;
	int irq;

	ndev = alloc_etherdev(sizeof(*priv));
	if (!ndev)
		return -ENOMEM;

	platform_set_drvdata(pdev, ndev);
	priv = netdev_priv(ndev);
	priv->ndev = ndev;
	u64_stats_init(&priv->stats64.syncp);

	ret = rtl8196e_dt_parse(&pdev->dev, &priv->iface);
	if (ret)
		goto err_free;

	if (priv->iface.mac_set)
		eth_hw_addr_set(ndev, priv->iface.mac);
	else
		eth_hw_addr_random(ndev);

	strscpy(ndev->name, priv->iface.ifname, IFNAMSIZ);
	priv->vlan_id = priv->iface.vlan_id;
	priv->portmask = priv->iface.member_ports;
	priv->phy_port = rtl8196e_port_from_mask(priv->portmask);
	priv->phy_id = priv->iface.phy_id_set ? priv->iface.phy_id : priv->phy_port;
	priv->link_poll_ms = priv->iface.link_poll_ms_set ? priv->iface.link_poll_ms : link_poll_ms;
	if (priv->phy_port < 0) {
		ret = -EINVAL;
		goto err_free;
	}

	{
		struct regmap *syscon;

		syscon = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
							 "realtek,syscon");
		if (IS_ERR(syscon)) {
			ret = PTR_ERR(syscon);
			if (ret != -EPROBE_DEFER)
				dev_err(&pdev->dev,
					"syscon lookup failed (%d), cannot configure Ethernet pinmux\n",
					ret);
			goto err_free;
		}
		priv->hw.syscon = syscon;
	}

	ret = rtl8196e_lan_led_init(&pdev->dev, priv);
	if (ret)
		goto err_free;

	priv->ring = rtl8196e_ring_create(RTL8196E_TX_DESC,
					 RTL8196E_RX_DESC,
					 RTL8196E_RX_MBUF_DESC,
					 RTL8196E_CLUSTER_SIZE);
	if (!priv->ring) {
		ret = -ENOMEM;
		goto err_free;
	}

	timer_setup(&priv->link_timer, rtl8196e_link_timer_fn, 0);
	timer_setup(&priv->tx_reclaim_timer, rtl8196e_tx_reclaim_timer_fn, 0);
	timer_setup(&priv->swcore_check_timer, rtl8196e_swcore_check_timer_fn, 0);
	timer_setup(&priv->swcore_retry_timer, rtl8196e_swcore_retry_timer_fn, 0);
	INIT_WORK(&priv->swcore_reset_work, rtl8196e_swcore_reset_work_fn);
	skb_queue_head_init(&priv->tx_defer_list);

	/* NAPI deferral tuning: on this slow CPU (Lexra RLX4181 @ 380 MHz),
	 * the driver drains the RX ring faster than packets arrive (~3 pkt/poll).
	 * Each NAPI cycle then pays the fixed cost of napi_complete_done()
	 * which in 6.x flushes GRO + walks up the TCP stack (~180 µs/cycle).
	 * At 2100 cycles/s this dominates CPU (~40 % wall time).
	 *
	 * napi_defer_hard_irqs=1 + gro_flush_timeout=2 ms turns the flush into
	 * an hrtimer-scheduled re-poll, batching more packets per cycle and
	 * avoiding the per-cycle IRQ ping-pong. Measured on this platform:
	 *   RX 70 → 93 Mbit/s (+33 %), TX 52 → 71 Mbit/s (+36 %).
	 *
	 * MUST be set BEFORE netif_napi_add(): in 6.x the framework copies
	 * ndev->napi_defer_hard_irqs into napi->defer_hard_irqs at add time,
	 * so setting it later has no effect on the already-added NAPI.
	 *
	 * These fields are still tunable via sysfs (/sys/class/net/eth0/...)
	 * so a userspace override remains possible without touching the driver.
	 */
	ndev->napi_defer_hard_irqs = 1;
	ndev->gro_flush_timeout = 2000000;  /* 2 ms */

	/* weight parameter removed from netif_napi_add in 6.1 */
	netif_napi_add(ndev, &priv->napi, rtl8196e_poll);
	ndev->netdev_ops = &rtl8196e_netdev_ops;
	ndev->ethtool_ops = &rtl8196e_ethtool_ops;
	ndev->watchdog_timeo = 10 * HZ;
	/*
	 * Hardware floor: rtl8196e_hw_netif_setup() rejects mtu < 576 (NETIF
	 * table encoding), and the DT parser enforces the same. A lower
	 * min_mtu would let `ip link set mtu <576` succeed while down, then
	 * fail the next open(). Keep the netdev floor in step with the HW.
	 */
	ndev->min_mtu = 576;
	ndev->max_mtu = priv->iface.mtu;
	ndev->mtu = priv->iface.mtu;
	ndev->features |= NETIF_F_RXCSUM;
	/* RX checksum is a real, gated policy now (rtl8196e_rx_set_csum), so let
	 * ethtool toggle it; off forces CHECKSUM_NONE for every frame. */
	ndev->hw_features |= NETIF_F_RXCSUM;
	ndev->sysfs_groups[0] = &rtl8196e_sysfs_group;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		ret = irq;
		goto err_ring;
	}

	/*
	 * Claim and map the three DT register windows (ETH-S01). The
	 * accessors keep the compile-time KSEG1 constants from
	 * rtl8196e_regs.h — on this in-order core they fold to lui+lw/sw
	 * with no pointer load, and ioremap() of a low-512MB physical
	 * address returns the very same KSEG1 alias anyway — so instead
	 * of carrying the mapping around, probe *verifies* it: a DT or
	 * platform change that moved a window fails loudly here rather
	 * than silently splitting MMIO between two addresses. The claim
	 * also makes /proc/iomem honest and detects window conflicts.
	 */
	{
		static const struct {
			unsigned long kseg1;
			const char *name;
		} win[] = {
			{ 0xB8010000UL, "cpu-interface" },
			{ 0xBB000000UL, "asic-table" },
			{ 0xBB800000UL, "switch-core" },
		};
		int i;

		for (i = 0; i < ARRAY_SIZE(win); i++) {
			void __iomem *base;

			base = devm_platform_ioremap_resource(pdev, i);
			if (IS_ERR(base)) {
				ret = PTR_ERR(base);
				goto err_ring;
			}
			if ((unsigned long)base != win[i].kseg1) {
				dev_err(&pdev->dev,
					"%s window mapped at %p, expected KSEG1 0x%08lx - constant-address accessors are stale\n",
					win[i].name, base, win[i].kseg1);
				ret = -ENXIO;
				goto err_ring;
			}
		}
	}

	/*
	 * One-time SoC bring-up (ETH-S03): pinmux + board 0x44 pad state,
	 * switch-clock toggle, MEMCR, FULL_RST, LED controller, queue
	 * mapping, L2 table clear. ~650 ms of msleeps — paid once here in
	 * process context instead of on every open().
	 */
	ret = rtl8196e_hw_init(&priv->hw);
	if (ret)
		goto err_ring;

	/*
	 * Quiesce before unmasking the line: the bootloader
	 * uses this NIC for TFTP and is not guaranteed to leave CPUIIMR
	 * masked or CPUIISR clean, so the ISR could otherwise run against
	 * a not-yet-registered netdev (a latched LINK_CHANGE_IP would
	 * drive netif_carrier_* on it). Mirrors what stop() does. Sits
	 * after hw_init so nothing FULL_RST latches survives either.
	 */
	rtl8196e_hw_disable_irqs(&priv->hw);
	rtl8196e_writel(rtl8196e_readl(CPUIISR), CPUIISR);

	ret = request_irq(irq, rtl8196e_isr, 0, RTL8196E_DRV_NAME, ndev);
	if (ret)
		goto err_ring;

	ret = register_netdev(ndev);
	if (ret)
		goto err_irq;

	/* Publish for the watchdog panic-record snapshot. */
	rtl8196e_panic_priv = priv;

	dev_info(&pdev->dev,
		 "v" RTL8196E_DRV_VERSION " (J. Nilo) - port:%d, vid:%u, mtu:%u\n",
		 priv->phy_port, priv->vlan_id, ndev->mtu);
	return 0;

err_irq:
	free_irq(irq, ndev);
err_ring:
	rtl8196e_ring_destroy(priv->ring);
err_free:
	free_netdev(ndev);
	return ret;
}

/* Platform remove: unregister netdev, free IRQ, destroy ring, free netdev. */
static void rtl8196e_remove(struct platform_device *pdev)
{
	struct net_device *ndev = platform_get_drvdata(pdev);
	struct rtl8196e_priv *priv;
	int irq;

	if (!ndev)
		return;

	priv = netdev_priv(ndev);

	/* Stop the panic notifier from dereferencing a freed instance. */
	rtl8196e_panic_priv = NULL;

	unregister_netdev(ndev);

	irq = platform_get_irq(pdev, 0);
	if (irq >= 0)
		free_irq(irq, ndev);

	if (priv->ring)
		rtl8196e_ring_destroy(priv->ring);

	free_netdev(ndev);
}

static const struct of_device_id rtl8196e_of_match[] = {
	{ .compatible = "realtek,rtl8196e-mac" },
	{ }
};
MODULE_DEVICE_TABLE(of, rtl8196e_of_match);

static struct platform_driver rtl8196e_driver = {
	.probe = rtl8196e_probe,
	.remove = rtl8196e_remove,
	.driver = {
		.name = RTL8196E_DRV_NAME,
		.of_match_table = rtl8196e_of_match,
	},
};

module_platform_driver(rtl8196e_driver);

MODULE_AUTHOR("Jacques Nilo");
MODULE_DESCRIPTION("RTL8196E minimal Ethernet driver");
MODULE_VERSION(RTL8196E_DRV_VERSION);
MODULE_LICENSE("GPL");
