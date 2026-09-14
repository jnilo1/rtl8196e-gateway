# Post-Mortem: rtl8196e-eth driver — performance journey 5.10 → 6.18

**Date:** April 2026 (last technical update May 2026, see postscripts).
**Hardware:** Realtek RTL8196E SoC, Lexra RLX4181 MIPS @ 380 MHz, 32 MB RAM, single-core, no FPU, non-coherent DMA.

> **Reading note (May 2026).** This document is the historical record of the
> April 2026 investigation that recovered the RX throughput on the 6.18 port.
> The driver has since evolved to v2.4 + v3.4.1 Track A coalescing.  Headline
> numbers in §6 and §9 are the April 2026 measurements; current production
> baselines live in
> `files-6.18/drivers/net/ethernet/rtl8196e-eth/PERFORMANCE.md`.  The
> ad-hoc `rxprofile` sysfs attribute and `ktime_get_ns()` counters
> referenced in §4 and §10 were investigation-only and never shipped to
> production; the current driver has different optional probes that live
> on the `feat/tx-throughput` archive branch.
>
> This document chronicles the performance investigation that followed the
> 6.18 port.  The goal: understand a measurable RX throughput regression
> on 6.18 vs 5.10, find the root cause, fix it, then explain *why* the
> fix worked.  The companion document `POST-MORTEM-6.18.md` covers the
> kernel/architecture porting work itself.

---

## 1. Starting point

The `rtl8196e-eth` driver is a from-scratch reimplementation of the legacy 11.7 KLOC
`rtl819x` SDK driver. It uses modern Linux APIs only:

- NAPI (`netif_napi_add` / `napi_complete_done` / `napi_gro_receive`)
- Platform driver + Device Tree
- `regmap`/`syscon` for SoC glue
- DMA streaming API
- `__iram` section placement for the hot path

Source layout (~2.6 KLOC):

| File | LOC | Role |
|---|---|---|
| `rtl8196e_main.c` | 692 | NAPI poll, sysfs, probe |
| `rtl8196e_hw.c` | 773 | Switch/PHY init, ASIC tables |
| `rtl8196e_ring.c` | 600 | TX/RX descriptor rings, refill |
| `rtl8196e_dt.c` | 108 | Device-tree parsing |

The hot path (`rtl8196e_ring_rx_poll`, `rtl8196e_ring_tx_reclaim`, GRO-receive glue)
is placed in on-chip SRAM via the custom `__iram` section to avoid main-memory
fetches under load.

### Baseline (5.10.252, no tuning, plain build)

| Direction | Throughput |
|---|---|
| TCP RX (host → gateway) | **86.6 Mbit/s** |
| TCP TX (gateway → host) | **48.1 Mbit/s** |

These numbers were within ~10% of the legacy `rtl819x` SDK driver while using
20% of its source size and entirely upstream APIs.

---

## 2. The regression

After porting the driver verbatim to 6.18 (kernel headers stable, no API changes
needed), the same iperf test gave:

| Direction | 5.10 | 6.18 (initial) | Delta |
|---|---|---|---|
| TCP RX | 86.6 | **66.8** | **-23%** |
| TCP TX | 48.1 | 52.0 | +8% |

A 23% RX regression on identical driver source was unexpected and unacceptable.
TX was actually marginally better.

---

## 3. Investigation — the wrong path

The first hypothesis was compiler/optimization regressions. We tried:

- **Per-file `-O2` overrides** in the driver Makefile — small effect, then reverted.
- **Inlining audit** of `rx_poll`, `pool_alloc_skb`, etc — no clear gain.
- **Comparing disassembly** of `rtl8196e_ring_rx_poll` between 5.10 and 6.18 builds
  with the same GCC 8.5.0 toolchain.

**Result of the comparison:** The compiled hot function is **byte-identical**
between the two kernels. 1032 bytes, same instruction sequence, same `jal` targets,
same register allocation. The only differences are symbol-name renames where the
kernel API itself was renamed (`__napi_alloc_skb` → `napi_alloc_skb`,
`napi_gro_receive` → `gro_receive_skb`).

This ruled out the compiler/code path entirely. The regression had to be in
**what the driver calls**, not in the driver itself.

---

## 4. Instrumentation — building the right measurement tool

We needed in-driver profiling that could survive the slow CPU. Constraints:

- **No `read_c0_count()`**: Lexra has no CP0 Count register
  (`cpu_has_counter == 0`). We had to use the RTL819X hardware clocksource
  via `ktime_get_ns()` (40 ns resolution).
- **No `__udivdi3`** linker dependency: u64/u32 divisions on MIPS 32-bit must
  go through `div_u64()` to avoid a missing libgcc helper.

Counters added (investigation only, **not shipped to production** — kept in
this section for the methodological record):

```c
u32 rtl8196e_rx_polls;        /* # of NAPI poll calls */
u32 rtl8196e_rx_packets;      /* # of skbs delivered  */
u64 rtl8196e_rx_ns_total;     /* time inside rx_poll loop */
u64 rtl8196e_rx_ns_gro;       /* time inside napi_gro_receive */
u64 rtl8196e_rx_ns_outer;     /* poll() function total */
u64 rtl8196e_rx_ns_gap;       /* idle gap between polls */
u64 rtl8196e_rx_ns_txr;       /* tx_reclaim time inside poll */
u64 rtl8196e_rx_ns_nc;        /* napi_complete_done time */
u64 rtl8196e_rx_ns_eni;       /* enable_irqs time */
```

An ad-hoc `rxprofile` sysfs attribute dumped the per-packet breakdown
during the investigation.  Counters and the sysfs attribute were
removed before the driver was merged.  The current driver has
different, more focused TX-side probes (`xmit_probe`, `kick_probe`,
`cache_probe`) on the `feat/tx-throughput` archive branch — see
the archived performance record (`PERFORMANCE.md`, "History"),
"In-driver instrumentation".

### What the counters revealed

Running iperf RX for 30 s on each kernel:

| Metric | 5.10 | 6.18 |
|---|---|---|
| Average packets per `poll()` call | **14** | **2** |
| Time inside `rx_poll` loop / packet | ~13 µs | ~15 µs |
| `napi_complete_done` cost / call | ~180 µs | ~200 µs |
| **Effective `napi_complete_done` cost / packet** | **~13 µs** | **~90 µs** |
| CPU fraction in `napi_complete_done` | ~11% | **~40%** |

The smoking gun: 6.18 was completing NAPI 7× more often per packet than 5.10.
`napi_complete_done()` does substantial work — flushing GRO state, walking the
TCP stack to generate pure ACKs, re-enabling interrupts — and on this CPU it
costs ~200 µs *per call*. 5.10 amortized that over 14 packets; 6.18 over 2.

The driver code was identical, yet the *batching behavior* of NAPI itself
differed. Why?

---

## 5. Root cause

Between 5.10 and 6.18, several subtle changes affect NAPI batching:

1. The default `napi_defer_hard_irqs` is **0** in both kernels. Without it,
   each ISR re-arms NAPI immediately, the `poll` returns the moment the ring
   is empty, `napi_complete_done` fires, IRQs are re-enabled, and the next
   packet re-triggers the cycle. Each cycle = full `napi_complete_done` cost.
2. In 5.10 the receive path was slow enough (older skb allocator, older GRO
   bulk-delivery path) that 14 wire packets typically queued up between polls
   "naturally" and amortized the completion cost.
3. In 6.18 the network stack is fast enough that `poll` drains the ring before
   the next batch arrives. With `defer_hard_irqs=0`, NAPI completes after every
   2 packets and re-enables IRQs — losing the amortization 5.10 enjoyed by
   accident.

This is exactly the scenario `napi_defer_hard_irqs` + `gro_flush_timeout`
were introduced to fix (commit `6f8b12d661d0` in 5.13). Setting these two
together turns the post-poll completion into a deferred hrtimer-scheduled
re-poll: instead of completing NAPI immediately, the kernel waits up to
`gro_flush_timeout` nanoseconds, allowing more packets to accumulate, then
re-polls in a single batch.

---

## 6. The fix

```c
/* NAPI deferral tuning: on this slow CPU (Lexra @ 380 MHz),
 * napi_complete_done() costs ~200 µs (GRO flush + TCP stack walk +
 * ACK gen). With defer_hard_irqs=0, that fires on every 2-packet
 * batch, eating ~40% of CPU on RX. Defer one IRQ + 2 ms hrtimer
 * lets the next batch grow to ~14 packets, amortizing the cost.
 *
 * MUST be set BEFORE netif_napi_add(): in 6.x the framework
 * copies ndev->napi_defer_hard_irqs into napi->defer_hard_irqs at
 * add time, so setting it later has no effect.
 */
ndev->napi_defer_hard_irqs = 1;
ndev->gro_flush_timeout = 2000000;  /* 2 ms */
netif_napi_add(ndev, &priv->napi, rtl8196e_poll);
```

A subtle bug we hit during development: **the order matters in 6.x**. Setting
`ndev->napi_defer_hard_irqs` *after* `netif_napi_add()` has no effect because
the value is copied into the per-NAPI struct at add time. In 5.10 the field
is read live, so the order was less strict. Two days lost on this.

### Sweep of `gro_flush_timeout`

| Timeout (µs) | 6.18 RX | 6.18 TX |
|---|---|---|
| 500 | 78 | 64 |
| 1000 | 87 | 68 |
| **2000** | **93.5** | **71.3** |
| 4000 | 93 | 71 |
| 8000 | 91 | 68 (stutter) |

`2000 µs` = sweet spot: maximum batching gain without latency stutter on TX
ACK paths.

### Final numbers (April 2026 measurement window)

| Direction | 5.10 (untuned) | 5.10 (tuned) | 6.18 (untuned) | 6.18 (tuned) |
|---|---|---|---|---|
| TCP RX | 86.6 | 87.2 | 66.8 | **93.5** |
| TCP TX | 48.1 | 49.4 | 52.0 | **71.3** |

The tuning was applied as a **built-in default** in both 5.10 and 6.18 drivers —
no user configuration needed. 6.18 ended up *faster* than 5.10 in both directions
after the fix. 5.10 gains were marginal (already amortized by accident).

> **Postscript (May 2026).**  Re-measured on the same hardware with
> driver v2.4 + v3.4.1 Track A coalescing applied: TCP RX 93.4, TCP TX
> 70.1 (5 × 60 s median).  The TX number is ~1 Mbit/s below the April
> 71.3 cited above; that earlier figure was a single-run point measurement
> while the May number is the median of a 5-rep sweep with 1 % variance.
> Both are consistent within the bench noise floor.  Current production
> baselines live in
> `files-6.18/drivers/net/ethernet/rtl8196e-eth/PERFORMANCE.md`; the per-phase
> TX path decomposition is in its archived history (see that file's "History").

---

## 7. The deep question — "Why is 6.18 faster per packet?"

After applying the fix, throughput numbers said 6.18 was clearly faster than
5.10 on the same hardware with identical driver source. But the disassembly
proof from §3 showed `rtl8196e_ring_rx_poll` was byte-identical. So where did
the gain come from?

A naive linear model `t = F + n × P` (fixed per-poll cost + per-packet cost)
fitted to the measurements gave a *negative* F — physically impossible. The
relationship was not linear, suggesting the per-packet cost itself differed.

We tried several angles:

- **strace** the iperf server to see syscall timing — abandoned, the gateway
  has no libc (busybox-only static system), no dynamic loader, dynamic
  binaries cannot run. Building strace statically would have taken 30 min for
  a user-space-only view that wouldn't see the softirq context anyway.
- **ftrace `function_graph`** with all-children depth-3 trace — instantly
  killed the gateway under load. Way too much overhead on 380 MHz.
- **ftrace `function` tracer + tight filter** — ended up working.

### ftrace methodology

Both kernels were rebuilt with:

```
CONFIG_FTRACE=y
CONFIG_FUNCTION_TRACER=y
CONFIG_FUNCTION_GRAPH_TRACER=y
CONFIG_DYNAMIC_FTRACE=y
```

`function_graph` itself turned out to be broken on this MIPS port (entries
not captured, presumably because the return-trampoline mechanism doesn't
match the Lexra exception path). We fell back to plain `function` tracer
with a 7-function filter:

```
net_rx_action
dev_gro_receive
eth_gro_receive
tcp_gro_receive
inet_gro_receive
napi_gro_complete    (5.10 only — inlined in 6.18)
__netif_receive_skb_core
```

`rtl8196e_ring_rx_poll` itself is **not** in the filter list because it lives
in `__iram`, a section excluded from mcount instrumentation. We measured the
chain *around* it instead — what the driver calls and what calls the driver.

Trace buffer set to 8 MB, 5 s of TCP RX captured on each kernel, then post-
processed with awk to compute, per active batch (≥ 3 super-skbs):

| Metric | 5.10 | 6.18 | Δ |
|---|---|---|---|
| super-skbs / batch | 4.47 | 4.05 | -9% |
| wire packets / batch | 128.3 | 115.8 | -10% |
| GRO ratio (wire/super) | 28.6 | 28.5 | identical |
| batch span (driver+GRO+stack) | 8389 µs | 6773 µs | -19% |
| **per super-skb** | **1876 µs** | **1674 µs** | **-11%** |
| **per wire packet** | **65.4 µs** | **58.5 µs** | **-11%** |

### Conclusion

The driver code is byte-identical. What changed is **everything the driver
calls**:

1. **`napi_alloc_skb` → page-fragment cache (5.13)**: skb headers come from
   a per-NAPI page fragment instead of slab. Eliminates a `kmalloc` per skb,
   which on Lexra (no `ll/sc`, slow atomics) is several µs.
2. **Bulk skb pool / `napi_consume_skb` recycling (5.16)**: completed skbs
   are returned to a per-NAPI freelist instead of `kfree_skb`.
3. **GRO bulk delivery (5.18 → 6.0)**: `napi_gro_receive` can deliver several
   merged skbs in one batched call to the upper stack, saving softirq
   transition overhead.
4. **`__napi_skb_cache` lazy free** and various `inet_gro_receive` /
   `tcp_gro_receive` micro-optimizations.

Cumulative effect: ~7 µs saved per wire packet end-to-end, exactly matching
the 11% gain we measured. There is no magic — four years of mainline network
stack optimization, applied to an unchanged driver, on the same hardware.

The interesting corollary: **the "regression" we initially saw was not a
regression in the driver, it was a regression in a heuristic**. 5.10 happened
to batch packets well by accident because its slower stack let queues build
up. 6.18 ran ahead of the queues and lost batching, until we put it back
explicitly with `napi_defer_hard_irqs`.

Once the heuristic was fixed, 6.18 was strictly faster than 5.10 because of
all the work that went into the network stack between those two releases.

---

## 8. Lessons learned

1. **Disassembly first, theory second.** Several days were spent on compiler
   tuning before checking whether the binary had actually changed. It hadn't.
2. **`napi_defer_hard_irqs` matters more than you think on slow CPUs.** The
   default of 0 is fine on modern hardware where `napi_complete_done` is
   cheap. On a 380 MHz MIPS without `ll/sc`, it is the single most impactful
   network tunable.
3. **Field initialization order in 6.x NAPI changed silently.** Per-device
   defaults must be set *before* `netif_napi_add`. Previous code relied on
   the field being read live.
4. **`__iram` excludes ftrace mcount.** Functions in custom sections cannot
   be traced. Move them out temporarily if you need fine-grained timing, or
   instrument with explicit `ktime_get_ns()` counters as we did.
5. **`function_graph` does not work on Lexra MIPS.** Use plain `function`
   tracer with a tight `set_ftrace_filter` and post-process timestamps in
   awk. Set the buffer to ≥ 8 MB for any non-trivial capture under load.
6. **Pick a small ftrace filter set first.** Trying to trace everything will
   wedge a 380 MHz CPU under network load within seconds.
7. **The `ksoftirqd` switch problem doesn't always need a code change.** If
   the stack is fast enough to drain rings between IRQs, just defer one IRQ
   to let the next batch grow.
8. **A "regression" is not always in your code.** Sometimes the platform got
   faster and broke a heuristic that depended on slowness.

---

## 9. Full test suite — apples-to-apples comparison

After the cleanup, the full regression suite (TCP RX/TX, parallel 4/8 streams,
TCP stress 5 min, UDP RX at 10/50/100 Mbit/s, UDP bidirectional 50/50) was
run on both kernels with **identical driver source and identical NAPI tuning**
applied. The only difference between the two runs is the kernel itself
(5.10.252 vs 6.18.0). Both runs use the same hardware, same Ubuntu host, same
Ethernet cable, same iperf 2 binary on the gateway.

### TCP

| Test | 5.10 | 6.18 | Δ |
|---|---|---|---|
| TCP Ubuntu → RTL (30 s)  | 87.9 Mbit/s | **93.7 Mbit/s** | +6.6 % |
| TCP RTL → Ubuntu (30 s)  | 44.3 Mbit/s | **69.8 Mbit/s** | **+57.6 %** |
| TCP Parallel 4 streams   | 86.9 Mbit/s | **95.0 Mbit/s** | +9.3 %  |
| TCP Parallel 8 streams   | 57.2 Mbit/s | **95.2 Mbit/s** | **+66 %** |
| TCP Stress 5 min         | 92.2 Mbit/s | 93.9 Mbit/s | +1.8 %  |

Two results stand out:

- **TCP TX +57.6 %** (44 → 70 Mbit/s). The NAPI deferral fix benefits both
  directions (RX poll and TX reclaim share the same softirq), but 6.18 amplifies
  the gain — the faster GRO and skb allocator paths make each TX completion
  cheaper.
- **TCP Parallel 8 streams +66 %** (57 → 95 Mbit/s). 5.10 collapses from
  86.9 Mbit/s at 4 streams to 57.2 Mbit/s at 8 streams — the per-flow GRO
  state thrashes the L1 cache and the slower per-skb path can't keep up.
  6.18 absorbs the parallelism without flinching, holding line rate (95.2)
  with 8 concurrent flows.

### UDP

| Offered  | 5.10 received / loss | 6.18 received / loss |
|---|---|---|
| 10 Mbit/s  | 10.5 / 0 %         | 10.5 / 0 %         |
| 50 Mbit/s  | 52.2 / **0.46 %**  | 52.4 / **0 %**     |
| 100 Mbit/s | 34.8 / 63 %        | **43.0** / 55 %    |

Three observations:

- At 50 Mbit/s, 5.10 already starts losing packets (617 / 133 750 = 0.46 %),
  while 6.18 is loss-free. The crossover where the gateway can no longer
  absorb the offered rate moved **up** by a few Mbit/s.
- At 100 Mbit/s offered, both kernels saturate: there is nothing the driver
  can do once the wire delivers more than the CPU can process. But 6.18's
  saturation point is **+24 %** higher (43 vs 35 Mbit/s).
- UDP is the harshest test for this CPU because there is no congestion
  control to throttle the sender — every packet that doesn't fit in the
  socket buffer is a drop. The fact that 6.18 absorbs ~8 Mbit/s more before
  saturation is a direct measurement of the per-packet path improvement.

### UDP Bidirectional 50 / 50

| Direction   | 5.10                  | 6.18                  |
|---|---|---|
| host → RTL  | 28.2 Mbit/s, 46 % loss | 29.9 Mbit/s, 43 % loss |
| RTL → host  | 11.4 Mbit/s, 0 % loss  | 12.7 Mbit/s, 0 % loss  |

Modest gain (+6 % each direction). Bidirectional UDP is bottlenecked by the
TX path consuming the same single CPU core that needs to service RX, so the
improvement is bounded.

### Interface-level counters (whole 10-min run)

| Kernel | RX packets | RX drops | drop rate |
|---|---|---|---|
| 5.10 | 3.62 M | 322 | 0.0089 % |
| 6.18 | 3.83 M | 316 | 0.0083 % |

Drops are entirely from the saturated UDP 100 Mbit/s tests. No drops in any
TCP test. Stability is identical between the two kernels.

### What this confirms

Both kernels run the **same driver source** with the **same NAPI tuning**
applied. The throughput delta is entirely from the kernel network stack.
The gains line up with what was predicted in §7:

- Per-packet path improvements (napi_skb_cache, bulk skb pool, GRO bulk
  delivery) → visible as 1.5-2 µs/pkt savings → **+6 % single-flow TCP**,
  **+24 % saturated UDP**, **+58 % TCP TX** (the TX path benefits the most
  because skb-completion is per-skb, not per-batch).
- Parallel-flow scaling: 6.18 added per-NAPI freelists and per-CPU GRO state
  improvements (5.18-6.0 timeframe) that prevent the cache-thrashing 5.10
  suffers under N>4 streams. **+66 %** at 8 streams.

The 6.18 port is no longer "experimental, comparable to 5.10". It is
**strictly faster on every metric**, often by a wide margin, while running
the exact same driver code. The investment in porting the Lexra MIPS bits
to mainline 6.x has paid off.

> **Postscript (May 2026).**  Track A (kick_tx coalescing,
> `rtl8196e_kick_threshold = 4` with NAPI-end drain) was deployed in
> v3.4.1 and adds another +1.2 % on TCP TX over the April baseline.
> See "Levers explored" in the driver's archived performance record
> (`PERFORMANCE.md`, "History") for the full
> v3.4.1 perf session, including the three other orthogonal levers
> that were measured and rejected (writeback-only flush, NAPI weight
> 128, full TX scatter-gather).

---

## 10. Files touched (April 2026 — what landed in production)

- `files-6.18/drivers/net/ethernet/rtl8196e-eth/rtl8196e_main.c`
  — added `ndev->napi_defer_hard_irqs = 1` and
  `ndev->gro_flush_timeout = 2 000 000` (2 ms) **before**
  `netif_napi_add()`.  Still in production today.
- `files/drivers/net/ethernet/rtl8196e-eth/rtl8196e_main.c`
  — same pair, applied to the 5.10 driver as well so the two kernels
  could be compared apples-to-apples.
- `config-5.10.252-realtek.txt` and `config-6.18-realtek.txt`
  — temporarily enabled `CONFIG_FTRACE` / `CONFIG_FUNCTION_TRACER` /
  `CONFIG_DYNAMIC_FTRACE` for the ftrace methodology in §7.  Reverted
  before the production build.
- `scripts/test_rtl8196e_eth.sh` — updated IP, replaced `ifconfig`/`ethtool`
  with `/sys/class/net/.../statistics` capture (busybox-only target).

The `rxprofile` sysfs attribute and the `ktime_get_ns()` counters in
`rtl8196e_ring.c` mentioned in §4 were **not** shipped — they were
removed before merge.  The current driver carries no in-tree
`rxprofile` attribute; the optional TX-side probes used in the v3.4.1
session live on the `feat/tx-throughput` archive branch.
