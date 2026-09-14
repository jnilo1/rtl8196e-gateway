# RTL8196E Ethernet driver (`rtl8196e-eth`) — specification

| | |
|---|---|
| **Document date** | 2026-07-30 |
| **Driver version** | 2.24 (`RTL8196E_DRV_VERSION` in `rtl8196e_main.c`). v2.24 fixes true LAN LED off by routing the board-declared active-low pad to an inactive GPIO output; the v2.23 datapath and recovery baseline is unchanged |
| **Active release** | v4.5.0 (kernels `6.18.51` and `7.2.5`, `-rtl8196e-v4.5.0`) |

Goals/non-goals contract and externally observable behaviour of the
driver. The architecture rationale lives in `DESIGN.md`, findings and
audit history in `AUDIT.md`, per-release throughput in `PERFORMANCE.md`
(all in this directory).

---

## 1. Goals

- Clean, single-purpose driver targeting the RTL8196E SoC only.
- Single physical Ethernet port (port 4 on the Lidl Silvercrest gateway).
- Maximum performance (zero-copy RX via `napi_alloc_skb`, direct TX).
- Compatible with existing devicetree (`&ethernet` + `interface@0`).
- IPv4 and IPv6 handled entirely by the Linux network stack.
- NAPI polling, hardware interrupts, basic ethtool stats.
- No kernel patches required (pure in-tree driver, no external dependencies).

## 2. Non-goals

- QoS / multiple queues / netfilter offload / L3-L4 hardware acceleration.
- Multiple hardware VLANs (single netdev only).
- Scatter-gather (`NETIF_F_SG` not advertised on the production driver — the
  HW supports it, see "Levers explored" in the archived performance record (`PERFORMANCE.md`, "History") and the
  `feat/tx-throughput` archive branch for the rejected SG implementation).
- XDP.

## 3. Hardware constraints (isolated in `rtl8196e_hw.*` / `rtl8196e_ring.c`)

- DMA registers and descriptors are programmed with **virtual** addresses
  (KSEG1 for the ring arrays, KSEG0 for pools and buffers), not
  `dma_addr_t` — the ASIC ignores the segment bits (AUDIT F17/ETH-S01).
- Non-coherent write-back L1: TX data requires `dma_cache_wback_inv()`
  before submit; HW-written descriptors require `dma_cache_inv()` before
  reading (full model in `DESIGN.md` §3).
- Two RX rings required: pkthdr (descriptors) + mbuf (buffers).
- Mandatory init sequence: pinmux (via syscon, shared registers —
  AUDIT ETHDRV-007), switch clock cycle, MEMCR, full reset, PHY init,
  TRXRDY.
- L2 toCPU entry required for CPU packet reception (trap-all fallback
  when it cannot be installed or verified).
- IRQ routed through SoC interrupt controller (GIMR bit 15).
- BIST skipped (must not block init).

## 4. RX buffer management (`napi_alloc_skb`)

- Hot path uses `napi_alloc_skb(napi, buf_size)` — NAPI-optimized allocation
  using a per-CPU page frag cache.  Avoids locks, maximizes cache locality.
  Internally adds `NET_SKB_PAD` headroom and calls `skb_reserve`.
- Ring init uses `netdev_alloc_skb_ip_align(NULL, ...)` (no NAPI context at
  probe time).
- Pre-allocated SKBs stored in shadow array `rx_bufs[]`
  (`struct rtl8196e_rx_buf { struct sk_buff *skb }`), one per RX
  descriptor. Since driver 2.6 the poll path looks the shadow up by
  the **hardware mbuf index** (pool-bounds guarded; misses counted in
  `rtl8196e_rx_mbuf_no_shadow`) instead of trusting ring-position
  correspondence.
- On each RX: the old SKB is handed to the stack, a new SKB is allocated
  with `napi_alloc_skb(napi, buf_size)`, and its `data` pointer is installed
  in the hardware descriptor.
- On destroy: `dev_kfree_skb_any()` for each shadow entry.
- No `page_pool`, no `build_skb()`, no PAGE_POOL Kconfig dependency.

## 5. Devicetree compatibility

- Parent node: `&ethernet` (compatible: `realtek,rtl8196e-mac`).
- Reads the first child `interface@0` (matched by `reg = <0>`):
  - `ifname` — interface name (default: `eth0`)
  - `local-mac-address` — MAC address (random if absent; the Lidl DTS
    deliberately omits it — S10network persists the random MAC to
    `/userdata/etc/mac_address` and restores it on every boot)
  - `vlan-id` — VLAN ID (default: 1)
  - `member-ports` — port bitmask (port 4 = `0x10`); must fit in the
    9-port HW window (`0x1ff`), driver rejects anything outside (since v2.4)
  - `untag-ports` — untag bitmask; must be a subset of `member-ports`
    (since v2.4)
  - `mtu` — MTU (default: 1500, accepted range 576–1500)
  - `phy-id` — PHY address for MDIO (default: same as port number)
  - `link-poll-ms` — link status polling interval (also on parent node)
- Additional interface nodes are silently ignored (the first child
  with `reg = <0>` wins; a warning is emitted only when no
  `interface@0` node is found at all).
- The `realtek,syscon` phandle is mandatory: probe defers on
  `-EPROBE_DEFER` and fails on any other lookup error (ETHDRV-001).
- `lan-led-gpios` on the parent Ethernet node identifies the active-low shared
  pad used by the front-panel LAN LED. It must be B2–B6 (GPIO 10–14): Lidl
  declares B6/LED_PORT4 and Sengled G4 overrides it with B2/LED_PORT0.
  `bright`/`dim` route this pad to the ASIC; `off` routes it to the preloaded
  inactive GPIO output. Without the property, Ethernet remains available but
  a request for `led_mode=off` fails with `-EOPNOTSUPP`.

## 6. File architecture

| File              | Role                                                                          | Pure LOC |
|-------------------|-------------------------------------------------------------------------------|---------:|
| `rtl8196e_main.c` | net_device, NAPI poll, ISR, TX xmit, ethtool, sysfs, probe/remove             |      692 |
| `rtl8196e_hw.c`   | MMIO registers, init sequence, KSEG1 helpers, PHY/MDIO, VLAN/NETIF/L2 tables  |      584 |
| `rtl8196e_ring.c` | TX/RX descriptor rings, kick coalescing, napi_alloc_skb RX buffers, cache ops |      627 |
| `rtl8196e_dt.c`   | Devicetree parsing (`interface@0` properties)                                 |       92 |
| `rtl8196e_regs.h` | Register definitions (trimmed to what's used)                                 |      140 |
| `rtl8196e_desc.h` | Hardware descriptor structures (`rtl_pktHdr`, `rtl_mBuf`)                     |       93 |
| `rtl8196e_ring.h` | Ring API                                                                      |       60 |
| `rtl8196e_hw.h`   | HW API                                                                        |       31 |
| `rtl8196e_dt.h`   | DT API                                                                        |       19 |
| **Total**         |                                                                               | **2 338** |

Pure LOC = non-blank, non-comment lines (`gcc -fpreprocessed -dD -E -P`),
re-verified exact at driver 2.6 on 2026-06-12. For comparison, the
legacy `rtl819x` driver (17 files) totalled ~9 660 pure LOC — a ~4×
reduction.

## 7. RX path

- Two RX rings:
  - pkthdr ring (descriptors) — `RTL8196E_RX_DESC` (128) entries
  - mbuf ring (buffers) — `RTL8196E_RX_MBUF_DESC` (128) entries
- Buffer allocation via `napi_alloc_skb(napi, buf_size)` (NAPI-optimized).
- Data placed at `skb->data` (after `NET_SKB_PAD` headroom, added internally).
- NAPI poll (`rtl8196e_ring_rx_poll()`), per RISC-owned entry up to budget:
  1. Check descriptor ownership bit.
  2. Validate that the pkthdr pointer starts exactly on an element of the RX
     pkthdr sub-pool (fallback to the static index mapping,
     `rx_wild_pkthdr`); invalidate + read it.
  3. Validate `ph_mbuf` against the exact RX mbuf sub-pool and element stride
     (`rx_wild_mbuf`); invalidate + read.
  4. Look the shadow SKB up by **hardware mbuf index**, guarded
     `< rx_cnt` (`rx_mbuf_no_shadow`).
  5. Bound-check `ph_len` to `[ETH_ZLEN, min(MTU + VLAN/L2 headers + FCS, buf_size)]` (`rx_bad_len`).
  6. Invalidate cache on packet data (only `len` bytes).
  7. `napi_alloc_skb()` — allocate a fresh SKB for the descriptor.
  8. `skb_put()` on old SKB, `eth_type_trans()`, `napi_gro_receive()`.
  9. Rearm from a **canonical descriptor state on every exit path**
     (nominal / drop / bad-length — ETHDRV-002): reset ph/mb fields,
     install the fresh SKB's `data`, flush buffer + descriptors,
     `wmb()`, flip both ring entries to SWCORE_OWNED (preserving WRAP).
- RX checksum is safe by default (`rtl8196e_rx_set_csum`): only an
  unfragmented IPv4 UDP datagram with both `CSUM_IP_OK` and
  `CSUM_TCPUDP_OK` is marked `CHECKSUM_UNNECESSARY`. TCP, IPv6, VLAN,
  fragmented and malformed traffic stays `CHECKSUM_NONE` until its hardware
  flags are characterized with deliberately corrupted frames.
  `NETIF_F_RXCSUM` remains in `hw_features`, so ethtool can force
  `CHECKSUM_NONE` globally. This policy intentionally accepts the historical
  software-TCP checksum cost in exchange for checksum integrity.
- Descriptor geometry: each `rtl_pktHdr` occupies its own cache line
  (`struct rtl8196e_pkthdr_slot`, `__aligned(L1_CACHE_BYTES)`) so an RX rearm
  `wback_inv` cannot clobber a switch DMA update to a neighbouring pkthdr.
  The 20-byte HW descriptor ABI is unchanged; only the pool stride grows.

## 8. TX path

- Single TX ring: `RTL8196E_TX_DESC` (128) entries.
- `rtl8196e_start_xmit()` → `rtl8196e_ring_tx_submit()`:
  - Non-linear SKBs linearized via `skb_linearize()`.
  - Short packets padded to `ETH_ZLEN` via `skb_put_padto()` **before**
    the data flush (info-leak guard, ETH-001); oversized (>1518) rejected.
  - Packet data flushed before submit (`dma_cache_wback_inv` on `skb->data`).
  - Descriptor flushes (pkthdr + mbuf) inside `tx_submit`; descriptor
    and mbuf pointers pool-bounds-validated (`tx_bad_pkthdr`/`tx_bad_mbuf`).
  - No spinlock: uniprocessor SoC, `start_xmit` runs with BH disabled.
  - Atomic ownership transfer (single write preserving WRAP bit).
- TX kick (`rtl8196e_ring_kick_tx(ring, was_empty)`):
  - Coalesced — pulses the `TXFD` bit on `CPUICR` at most once per
    `rtl8196e_kick_threshold` submits (default 4), except on cold-start
    (`was_empty == true`) where it always pulses immediately so the ASIC
    TX DMA engine can wake.
  - `rtl8196e_ring_kick_drain(ring)` is invoked at the end of every NAPI
    poll **and** at each dequeue-batch boundary (`!netdev_xmit_more()` in
    `start_xmit`), so a descriptor flipped after the TX engine parked is
    never left unkicked past the current burst — even on an RX-silent link.
  - Coalescing introduced in v3.4.1; cf. "Levers explored" in
    the archived performance record (`PERFORMANCE.md`, "History").
- TX reclaim (`rtl8196e_ring_tx_reclaim()`):
  - Called from NAPI poll with `napi_budget > 0` (uses `napi_consume_skb`
    for batched SKB freeing).
  - Called unconditionally from `start_xmit` (no TX completion IRQ).
  - **Software TX-reclaim timer** (`RTL8196E_TX_RECLAIM_MS`, 4 ms): armed
    whenever the queue stops, it kicks a NAPI poll so a stopped queue with no
    RX traffic reclaims its in-flight descriptors before the 10 s netdev
    watchdog. Lapses once the queue moves again.
- TX completion: `TX_ALL_DONE` interrupt is **not** unmasked in
  `CPUIIMR`.  Reclaim is entirely software, driven by NAPI poll
  (RX traffic side) and by the unconditional reclaim at the head of
  every `start_xmit` (process side).
- Flow control:
  - `netif_stop_queue()` when free count `< 4` (`RTL8196E_TX_STOP_THRESH`).
  - `netif_wake_queue()` when free count `>= 16` (`RTL8196E_TX_WAKE_THRESH`),
    checked in NAPI poll after TX reclaim.
- **BQL** (byte queue limits): `netdev_sent_queue` on submit,
  `netdev_completed_queue` at the three reclaim sites, `netdev_reset_queue`
  on every ring rebuild — bounds TX buffering under the qdisc.
- TX timeout: **atomic** callback (`dev_watchdog` runs it under
  `tx_global_lock`, so it must not sleep) — it only stops the queue and logs
  the recovery fingerprint, then defers the ring/switch reset to the
  deep-reset worker (shared, serialized against the other recovery sites).
- Switch-core recovery: poll-side RUNOUT-storm resync + a periodic
  watchdog (TX-done-stuck / PHY-interface-lost / drop-flood-with-desync)
  that escalates to a full switch-core deep reset from a workqueue.
- ISR and timer NAPI kicks share one helper that masks device IRQs before
  publishing `NAPI_STATE_SCHED`. If NAPI is already active, the mask remains
  closed and the current poll records the missed event before completing.
- A failed reset reprogram latches a hold-down state before NAPI is re-enabled:
  queue and carrier stay down, device IRQs stay masked, and link/reclaim/watchdog
  timers cannot undo that state. Three process-context retries run after
  1/2/4 seconds; exhaustion leaves the interface down until an administrative
  down/up cycle. `stop()` synchronously deletes the retry timer on both sides
  of `cancel_work_sync()` so an exiting worker cannot leave a timer behind.

## 9. PHY / Link

- Minimal PHY init sequence extracted from legacy driver, isolated in
  `rtl8196e_hw.c`.
- Link status read from port registers (PSRPx).
- `netif_carrier_on/off` updated on link change IRQ and poll timer.
- Link poll timer interval configurable via DT (`link-poll-ms`) or module
  param.
- MAC address change is refused while the interface is UP (F2): the
  NETIF table and the hashed L2 toCPU entry are reprogrammed from
  `dev_addr` only at `open()`.

## 10. Constants

| Constant                  | Value | Location          |
|---------------------------|------:|-------------------|
| `RTL8196E_TX_DESC`        |   128 | `rtl8196e_main.c` |
| `RTL8196E_RX_DESC`        |   128 | `rtl8196e_main.c` |
| `RTL8196E_RX_MBUF_DESC`   |   128 | `rtl8196e_main.c` |
| `RTL8196E_CLUSTER_SIZE`   |  1700 | `rtl8196e_main.c` (`buf_size` passed to ring) |
| `RTL8196E_TX_STOP_THRESH` |     4 | `rtl8196e_main.c` |
| `RTL8196E_TX_WAKE_THRESH` |    16 | `rtl8196e_main.c` |
| `rtl8196e_kick_threshold` |     4 | `rtl8196e_ring.c` (extern; runtime-tunable via the `kick_threshold` sysfs attribute, §13) |
| `RTL8196E_TX_RECLAIM_MS`  |     4 | `rtl8196e_main.c` (software TX-reclaim timer period) |
| `RTL8196E_RESET_RETRY_MAX` |     3 | `rtl8196e_main.c` (bounded reset reprogram retries) |
| `RTL8196E_RESET_RETRY_BASE_MS` | 1000 | `rtl8196e_main.c` (1/2/4 second retry backoff) |
| `RTL8196E_DRV_VERSION`    | "2.24" | `rtl8196e_main.c` |

## 11. Init sequence (in `rtl8196e_open()`)

1. `rtl8196e_hw_init()`: pinmux via syscon, switch clock cycle, MEMCR
   (0 then 0x7f), FULL_RST + delay, LED direct mode, RX queue mapping,
   L2 table clear, W1C pending IRQs.
2. Set RX rings (pkthdr + mbuf base addresses) and TX ring base address.
3. `rtl8196e_hw_init_phy()`: PHY reset + autoneg for the configured port.
4. `rtl8196e_hw_vlan_setup()`: VLAN table entry + PVIDs.
5. `rtl8196e_hw_netif_setup()`: NETIF table entry (MAC, VLAN, MTU, port mask).
6. `rtl8196e_hw_l2_setup()`: L2 forwarding mode, flood control, STP
   forwarding (+ optional `rtl8196e_force_trap` debug mode).
7. `rtl8196e_hw_l2_add_cpu_entry()`: toCPU L2 entry for driver MAC —
   on failure, trap-to-CPU fallback and skip to step 9.
8. `rtl8196e_hw_l2_add_bcast_entry()` + `rtl8196e_hw_l2_check_cpu_entry()`
   readback verify — trap fallback on verify failure.
9. `rtl8196e_hw_start()`: CPUICR (`TXCMD | RXCMD | BUSBURST_32WORDS |
   MBUF_2048BYTES | EXCLUDE_CRC`), TRXRDY.
10. `napi_enable()`.
11. `rtl8196e_hw_enable_irqs()`: CPUIIMR (`RX_DONE_IE_ALL | LINK_CHANGE_IE
    | PKTHDR_DESC_RUNOUT_IE_ALL`).  TX completion is **not** unmasked
    here (software reclaim).
12. Start queue, check link, start link poll timer.

`stop()` runs the reverse direction, reclaims completed TX descriptors after
the hardware stops, counts only the remaining SKBs as `tx_dropped`, and
**resets both rings** so the next `open()` starts from a canonical descriptor
state (ETH-002).
Step 1 (`rtl8196e_hw_init`, the ~650 ms one-time SoC bring-up) has been
hoisted to **probe** (ETH-S03 done); `open()` now only reprograms the
volatile per-open state (`rtl8196e_hw_reprogram`: ring bases, PHY,
VLAN/NETIF/L2 entries) and starts the core, so a down/up costs milliseconds.
A best-effort L2-clear failure in `hw_init` is warned, not fatal — probe
must still bring the interface up.

Every SWTCR/TLU BUSY timeout aborts the current table operation with
`-ETIMEDOUT` after restoring `SWTCR0` and `TLU_CTRL`. VLAN and NETIF clears
are mandatory and propagate failure to `open()` or the recovery hold-down.
The probe-time L2 clear remains best-effort, while runtime L2 entry failures
use the existing trap-to-CPU fallback.

## 12. Module parameters

All exposed under `/sys/module/rtl8196e_eth/parameters/` once the
driver is loaded.  All read/write at runtime (mode 0644, root-only
debug knobs — ETHDRV-005).

| Parameter                | Type          | Default               | Purpose                                                  |
|--------------------------|---------------|----------------------:|----------------------------------------------------------|
| `link_poll_ms`           | unsigned int  | 0 (disabled)          | Link poll interval in ms; 0 disables the poll timer      |
| `rtl8196e_debug`         | unsigned int  | 0                     | Extra debug logging (descriptor dumps via `dbg_timer`)   |
| `rtl8196e_force_trap`    | unsigned int  | 0                     | Force all unknown traffic to CPU (debug)                 |
| `rtl8196e_cpu_port_mask` | unsigned int  | `RTL8196E_CPU_PORT_MASK` (0x20) | CPU port mask for VLAN / L2                |

`link_poll_ms` and `rtl8196e_cpu_port_mask` are consumed at `open()`
time only; a runtime write takes effect on the next down/up cycle.

## 13. Sysfs attributes (under `/sys/class/net/eth0/`)

| Attribute        | Mode | Purpose                                                                |
|------------------|------|------------------------------------------------------------------------|
| `led_mode`       | RW   | Front-panel LAN LED mode: `bright` / `dim` use the ASIC (`LEDCREG`/`DIRECTLCR`); `off` remuxes the board-declared pad to an inactive GPIO output |
| `kick_threshold` | RW   | TX kick coalescing factor, range 1..64 (1 disables coalescing); mirrors `rtl8196e_kick_threshold`, sweepable under live traffic |

## 14. Ethtool stats (`ethtool -S eth0`)

36 driver-private stats (`RTL8196E_ETHTOOL_STATS_COUNT`, returned by
`get_sset_count(ETH_SS_STATS)`). The bring-up `tx_dbg_*` scaffolding was
retired; the groups are now:

L2 toCPU checks:

- `rtl8196e_l2_check_ok` / `_fail` / `_last_result`

TX kick coalescing (TXFD pulses split by trigger):

- `rtl8196e_tx_kicks_total` / `_cold` / `_threshold` / `_drain`

Ring anomaly counters (must stay 0 in nominal flow):

- `rtl8196e_rx_wild_pkthdr`, `rtl8196e_rx_wild_mbuf` — descriptor
  pointers outside their pool
- `rtl8196e_rx_bad_len`, `rtl8196e_rx_no_skb`, `rtl8196e_rx_alloc_fail`,
  `rtl8196e_rx_rearm_badidx`, `rtl8196e_rx_mbuf_no_shadow` — RX drop
  / rearm anomalies
- `rtl8196e_tx_bad_args`, `rtl8196e_tx_bad_len`, `rtl8196e_tx_ring_full`,
  `rtl8196e_tx_reclaim_no_skb`, `rtl8196e_tx_bad_pkthdr`,
  `rtl8196e_tx_bad_mbuf` — TX submit / reclaim anomalies

Switch-core recovery (stay 0 unless a storm/wedge hit):

- `rtl8196e_rx_runout_resync`, `rtl8196e_rx_runout_kick`,
  `rtl8196e_swcore_deep_reset`, `rtl8196e_swcore_reprogram_fail`
- `rtl8196e_rx_pkthdr_mbuf_skew` — switch-desync probe (saturation-only)
- `rtl8196e_poll_budget_hit`, `rtl8196e_rx_stall_run` — drop-flood stall
  detector observability

TX flow / deferral:

- `rtl8196e_tx_xoff_seen_xmit`, `rtl8196e_tx_xoff_seen_poll`
- `rtl8196e_tx_defer_queued`, `rtl8196e_tx_defer_direct`

Activity taps (storm forensics):

- `rtl8196e_isr_cnt`, `rtl8196e_isr_burst_max`, `rtl8196e_poll_cnt`,
  `rtl8196e_poll_delivered`, `rtl8196e_kick_txreclaim`

Non-wrapping packet/byte totals are reported via `ndo_get_stats64`
(`ip -s link`), not as ethtool-private stats. `u64_stats_sync` protects the
four accumulators against torn reads on the 32-bit CPU; the UP/BH concurrency
model provides the required single writer.

## 15. Verification

Current baselines (6.18 head, gcc 15.2 toolchain — see
`32-Kernel/CLAUDE.md` and the release tables in `PERFORMANCE.md`):

- TCP RX (host → gateway): **~93.5–94 Mbit/s** (line-rate), retrans ~0
- TCP TX (gateway → host): median **~70 Mbit/s** (run-to-run 69.3–72.8),
  retrans ~0 — use a 5-rep median
- Regression threshold: sustained TX below ~69 or RX below ~93, or any
  non-zero TCP retransmission rate (`scripts/test_rtl8196e_eth.sh`; stop
  OTBR first)
- Pre-hardening v2.22 bench (2026-07-17): TCP RX 93.9 / TX 69.6,
  RetransSegs 0.0000%, all anomaly counters 0. The safe-default checksum
  follow-up requires a fresh performance run before release.

Historical reference (v3.4.1, kernel 6.18.24, where the §8 kick
coalescing was introduced): TCP RX 93.5 / TCP TX 70.1 / UDP TX 100M
37.9 / UDP storm 64-byte 1.87 Mbit/s (iperf 2.x, 5 × 60 s medians,
direct cable).

Functional checks:

- `ping` IPv4 / IPv6.
- Stable SSH session.
- 0 driver TX/RX errors, 0 TCP retransmissions on the SoC side over a
  300 s stress.
- `ethtool -S eth0` shows the driver-private stats (see §14) with all
  anomaly counters at 0.
- No warnings in dmesg.
- `/proc/irq/<eth>/spurious` stays at 0 unhandled (F7/F14 gate).

See the archived performance record (`PERFORMANCE.md`, "History") for the full per-phase TX path decomposition,
asymptote analysis, and the four orthogonal-levers tracks measured in
the v3.4.1 perf session.
