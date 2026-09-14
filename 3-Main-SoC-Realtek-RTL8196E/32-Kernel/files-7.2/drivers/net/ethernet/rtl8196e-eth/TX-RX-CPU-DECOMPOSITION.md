# TX/RX per-packet CPU decomposition — methodology and results

**Status:** DONE, driver v2.20.
**Date:** 2026-07-02.
**Audience:** anyone re-running or extending this measurement, or wanting to
know exactly what "the driver is ~32 % of total per-packet CPU" means and
where that number comes from.
**Branch:** `perf/tx-rx-decomposition` (off `optim_tx` tip `15f1b51`;
round 1 commit `becf657`, round 2 commit `e974949`) — **archived on
`private`, never merged.** The instrumentation
described here does not ship on `main`/`optim_tx`; the production driver
carries no `ktime_get()` calls in the hot path. Only this document is
permanent (the summary `PERFORMANCE.md` carried is in its archived history,
see that file's "History").

---

## 1. TL;DR

The driver's own code — `start_xmit()` plus `poll()` combined — accounts for
**~32 % of total per-packet CPU** during a sustained TCP TX transfer, not the
**~6 %** previously assumed. The old figure (the archived performance record (`PERFORMANCE.md`, "History"), "TX path
per-packet decomposition (driver v2.4 + Track A)") only ever measured
`start_xmit()` in isolation and *inferred* the rest was generic TCP/IP stack;
`rtl8196e_poll()` — the NAPI RX/reclaim/ACK-processing path — had never been
instrumented, and turns out to cost **more** per TX packet (40.1 µs) than
`start_xmit()` itself (15.5 µs).

This does not overturn the driver's earlier conclusion that further
driver-side tuning has no measurable throughput effect (that was tested
empirically, not inferred, in the 2026-05-02 orthogonal-levers session). It
does correct *why*, and it shifts where a future lever would plausibly land:
`poll()`'s NAPI-batching behavior, not `start_xmit()`'s already-lean
~15.5 µs/pkt.

---

## 2. Motivation

The archived performance record's only prior driver-vs-stack breakdown was a single
`ktime_get()` measurement of `start_xmit()`'s own internals (cache-flush /
kick / "other" split, 10,852 ns total), captured 2026-05-02 on driver v2.4.
From that single number, the document extrapolated: at the observed packet
rate and an assumed ~132 µs/packet CPU budget (itself derived from packet
rate under an assumed 100 % CPU-busy condition), `start_xmit()` was said to
consume "~6 % of total CPU time per packet — the rest ... sits in the TCP/IP
send-side stack and the soft-IRQ NAPI poll that processes incoming TCP ACKs."

That sentence bundles two different things under "the rest": the generic
network stack (not driver code) and `rtl8196e_poll()` (driver code that had
simply never been measured). No number ever separated them. This document
does.

---

## 3. Why manual `ktime_get()` probes, not ftrace / perf / kprobes

This question was already answered, empirically, by a *different*
investigation already in this tree (`POST-MORTEM-driver-perf.md` §7, "The
deep question — why is 6.18 faster per packet?"), which needed the same kind
of fine-grained timing and tried the standard kernel tools first:

- **`function_graph` tracer is broken on this Lexra/MIPS port** — entries are
  not captured (the return-trampoline mechanism does not match the Lexra
  exception path).
- **An unfiltered/deep ftrace trace crashed the gateway under load** at
  380 MHz within seconds.
- The only mode that worked was the plain **`function` tracer with a tight
  `set_ftrace_filter`** and an 8 MB trace buffer — and even then, **functions
  placed in `__iram` are excluded from mcount instrumentation** (the `.iram`
  section is incompatible with ftrace's instrumentation mechanism). Both
  `rtl8196e_start_xmit()` and `rtl8196e_poll()` are `__iram`.

On top of that prior finding, this session confirmed the current kernel
`.config` has `CONFIG_FTRACE`, `CONFIG_PERF_EVENTS`, `CONFIG_KPROBES`, and
`CONFIG_TRACEPOINTS` all **off**. `CONFIG_MODULES` is off too (the kernel is
fully monolithic — `rtl8196e-eth` is built-in, not a module), so there is no
way to load a tracepoint consumer without a full rebuild+reflash regardless
of which mechanism is used. `CONFIG_HAVE_PERF_EVENTS=y` is set at the arch
level, but `CONFIG_HW_PERF_EVENTS` requires MIPS CPU families this Lexra
core does not belong to (no CP0 `Count` register) — `perf` would be
software-counter-only at best, with the same rebuild cost and no advantage
over a manual bracket.

Given all of that, extending the driver's own existing manual-probe pattern
(see §4) was the only approach with a track record of actually working, and
already-known overhead characteristics, on this specific hardware.

---

## 4. Instrumentation design

Four independently-gated probes, each a `ktime_get()`/`ktime_sub()` bracket
around one call site, recorded into a per-device 16-bucket log2 histogram
(`<128 ns` doubling up to `>=2.1 ms`) plus running `count`/`sum_ns`/`max_ns`.
Three (`xmit_probe`, `cache_probe`, `kick_probe`) are a straight port of the
archived `feat/tx-throughput` branch design (commits `382c837`, `33fdac2`,
driver v2.4) onto the current `start_xmit()` — which has grown substantially
since (BQL accounting, a retry-on-failure path, the `tx_xoff_seen_*`
counters), none of which existed when that archive branch was written. The
fourth, `poll_probe`, is new — `rtl8196e_poll()` had never been probed.

| Probe | Brackets | File:line (this branch) |
|---|---|---|
| `xmit_probe` | The whole `start_xmit()` body, success path only (function entry to just before `return NETDEV_TX_OK`; an early-drop return above the bracket records nothing) | `rtl8196e_main.c:564-666` |
| `cache_probe` | Only the single `dma_cache_wback_inv(skb->data, skb->len)` call (not the two descriptor-writeback flushes inside `rtl8196e_ring_tx_submit()`) | `rtl8196e_main.c:618` |
| `kick_probe` | Only the `rtl8196e_ring_kick_tx()` call | `rtl8196e_main.c:647` |
| `poll_probe` | The whole `rtl8196e_poll()` body (single return path, no branch-exclusion needed) | `rtl8196e_main.c:772-906` |

Design choices carried over unchanged from the archive (all already proven
safe there):

- `module_param(..., bool, 0644)` gate per probe, all default off — matches
  this driver's existing convention (`rtl8196e_rx_stall_thresh`,
  `link_poll_ms`) rather than a new Kconfig symbol.
- The recorder (`rtl8196e_perf_record()`) is `noinline` and deliberately kept
  **outside `.iram`** — both `start_xmit()` and `poll()` are `__iram`
  (16 KB on-chip zero-wait-state SRAM, hard budget enforced at link time),
  and a probe recorder inlined into them would eat into that budget for
  code that only exists on this disposable branch. Verified before flashing:
  `.iram` grew from ~6 KB baseline to 7,648 B with all four probes present,
  and `nm` confirmed all four recorder symbols fall outside the `.iram` VMA
  range.
- Exposed via sysfs (`/sys/class/net/eth0/{xmit,cache,kick,poll}_probe_stats`
  RO + `..._reset` WO), not ethtool — the BusyBox rootfs on this gateway has
  no `ethtool` binary by default (a static one is deployable for the
  correctness gate in §5, but sysfs is what the probes themselves use).

---

## 5. Bench rig and procedure

- Kernel `6.18.35-rtl8196e-v4.0.0-rc4`, driver `2.20-tx-rx-probe` (the
  `-tx-rx-probe` `DRV_VERSION` suffix makes a probe build unmistakable in
  `dmesg`/`uname -r` — it must never reach `main`/`optim_tx`).
- Gateway 192.168.1.88, host 192.168.1.200, direct Cat-6 cable on `enp2s0`
  (verified via `ip route get 192.168.1.88` before trusting any rep).
- OTBR fully stopped (`/userdata/etc/init.d/S70otbr stop`) before every
  bench — otherwise `otbr-agent`'s mDNS/border-routing traffic on `eth0`
  contaminates the TX measurement.
- Workload: `iperf3 -c 192.168.1.88 -p 5201 -R -t 30` (gateway → host, TX
  direction — reverse mode so the *server*, i.e. the gateway, is the
  sender), 5 reps per set, `sleep 2` between reps (back-to-back reps can
  race the daemonized `iperf3 -s -D` server otherwise).
- Correctness gate independent of the timing measurement: `ethtool -S eth0`
  ring-anomaly counters (`tx_ring_full`, `rx_wild_pkthdr`, `rx_bad_len`,
  etc.) checked before and after every rep — all stayed 0 across all 20 reps
  run this session.
- Sanity check per rep: `xmit_count`/`cache_count`/`kick_count` all landed
  within a few packets of `tx_packets` delta (one `start_xmit()` call = one
  successful TX packet) — confirms no probe was toggled mid-rep and no
  packet accounting drift.
- Gateway restored to the clean production `optim_tx` build (no probe
  suffix, all probes' source code entirely absent) and OTBR restarted
  immediately after data collection — this instrumentation never stayed on
  the shared bench unit longer than the measurement itself required.

---

## 6. A methodological pitfall found and fixed: combined-probe overhead

The first run enabled all four probes simultaneously (the natural first
attempt, since it lets every number come from literally the same window).
It produced a real, non-negligible artifact: a calibration rep with **only**
`xmit_probe` enabled measured `start_xmit()` at 11,901 ns/pkt; with
`cache_probe` and `kick_probe` also running, the same quantity measured
15,488 ns/pkt — a ~3,600 ns inflation. Throughput also dropped measurably
(65.3 vs 66.9 Mbit/s median).

The cause is structural, not noise: `cache_probe` and `kick_probe` bracket
call sites that sit **inside** `xmit_probe`'s own bracket. Each adds its own
`ktime_get()` reads, and `ktime_get()` is not cheap on this core — there is
no CP0 `Count` register, so the clocksource falls back to
`timer-rtl819x`, an MMIO-backed hardware timer. Four extra MMIO round-trips
per packet (two probes, each reading the clock on entry and via the
recorder) is a real, measurable cost when it happens ~170,000 times over a
30 s window.

**Fix:** rather than trust a single all-four-probes run, the final numbers
come from **two separately-probed 5-rep sets**:

- **Set A** — `xmit_probe` + `cache_probe` + `kick_probe` together, `poll_probe`
  off. This matches the archive branch's original intent (cache/kick are
  legitimately nested inside xmit's own bracket by design — they are
  sub-phases of the same function, not unrelated code), so no further
  separation was needed here.
- **Set B** — `poll_probe` alone, the other three off. `poll()` is an
  independent call site (not nested inside `start_xmit()`), so isolating it
  removes any cross-contamination between the two driver entry points.

The two sets' own `total_cpu_ns_per_pkt` (see §7) differed by only ~2.3 %
(171,430 vs 175,461 ns/pkt) — small enough that the headline result is
reported as a tight sensitivity range rather than a single, false-precision
number (§8). A same-window four-probe run was kept as a cross-check and
landed within about one percentage point of the separated result (31.4 % vs
~32 %) — reassuring, but the separated measurement is the one trusted for
the exact figures in §8.

A second, smaller platform gotcha surfaced during this same work: **BusyBox
`date` on this rootfs has no sub-second resolution** — `date +%s%N` silently
returns garbage (observed `212`, `243`), not epoch nanoseconds, and
`date --help` confirms only whole-second formats are supported. The fix was
to use each rep's own `iperf3` sender-interval report (`0.00-30.01 sec`,
10 ms precision) as the wall-clock duration instead of a gateway-side
timestamp — it brackets the gateway's actual TX-active window directly and
needs no unsupported format specifier.

---

## 7. Calculation methodology

All quantities for a given rep come from that rep's own before/after
snapshot: `tx_packets` deltas (`/sys/class/net/eth0/statistics/tx_packets`),
the `cpu` line of `/proc/stat` (for CPU-busy fraction — not `top`/`vmstat`;
this BusyBox build has no `vmstat` and `top`'s batch-mode reliability over a
scripted SSH capture was not established), and each active probe's sysfs
dump, reset between reps.

```
busy_frac              = 1 − Δidle / Δtotal                     (from /proc/stat, CONFIG_HZ-invariant)
wall_ns                = iperf3 sender-interval seconds × 1e9   (§6 — not gateway-side date)
total_cpu_ns_per_pkt    = busy_frac × wall_ns / tx_delta

xmit_ns_per_pkt         = xmit_sum_ns  / xmit_count      (Set A)
cache_ns_per_pkt        = cache_sum_ns / cache_count     (Set A — nested sub-component of xmit, not additive)
kick_ns_per_pkt         = kick_sum_ns  / kick_count      (Set A — nested sub-component of xmit, not additive)
xmit_other_ns_per_pkt   = xmit_ns_per_pkt − cache_ns_per_pkt − kick_ns_per_pkt

poll_ns_per_tx_pkt      = poll_sum_ns / tx_delta         (Set B — NOT / poll_count, see below)

driver_own_ns_per_pkt   = xmit_ns_per_pkt + poll_ns_per_tx_pkt          [DIRECT — both terms measured]
residual_ns_per_pkt     = total_cpu_ns_per_pkt − driver_own_ns_per_pkt   [DERIVED]
driver_own_pct          = driver_own_ns_per_pkt / total_cpu_ns_per_pkt × 100
```

`poll_probe`'s cost is deliberately divided by `tx_delta` (TX packets), not
by `poll_count` (the number of `poll()` invocations, which is far smaller —
NAPI batches many ACKs per poll call). In a TX-direction bench, essentially
every `poll()` invocation exists to service the ACK stream that the gateway's
own TX traffic generated, so "total poll cost in the window ÷ TX packets
that produced it" is the correct per-TX-packet attribution, not "cost per
poll call."

`cache_ns_per_pkt` and `kick_ns_per_pkt` are **not** added a second time on
top of `xmit_ns_per_pkt` — they already execute inside `start_xmit()`'s own
bracket, so they are sub-components used only for the internal breakdown in
Table B (§8), not independent terms in the headline split.

Per-rep figures are computed for all 5 reps in each set; the values reported
in §8 are the **median of 5**, matching this driver's existing
noise-floor-aware bench convention (a single run can land anywhere in the
documented spread).

---

## 8. Results

**Table A — headline split** (Set A for `start_xmit`, Set B for `poll`,
5-rep medians each):

| Component | ns/pkt | % of total | Basis |
|---|---:|---:|---|
| `start_xmit()` (TX submit path) | 15,528 | ~9 % | **DIRECT** |
| `poll()` attributable to the TX-direction ACK stream | 40,098 | ~23 % | **DIRECT** |
| **Driver-own total** | **55,626** | **~32 %** | DIRECT (sum) |
| Generic TCP/IP stack + softirq (residual) | 115,800 – 119,835 | ~68 % | **DERIVED** |
| **Total CPU per TX packet** | 171,430 – 175,461 | 100 % | DIRECT (Set B vs Set A — different reps, see §6) |

`driver_own_pct` sensitivity range: **31.7 %** (using Set A's own
`total_cpu`) to **32.5 %** (using Set B's own `total_cpu`) — reported as a
range rather than a single point figure precisely because `start_xmit` and
`poll` were measured in separate reps.

**Table B — internal `start_xmit()` decomposition** (Set A, cache/kick
nested inside xmit by design — directly comparable to the 2026-05-02 v2.4
table):

| Phase | ns/pkt | % of `start_xmit` | v2.4 (2026-05-02) |
|---|---:|---:|---|
| `dma_cache_wback_inv(skb->data, skb->len)` | 1,597 | 10.3 % | 15.4 % (1,675 ns) |
| `rtl8196e_ring_kick_tx` (CPUICR pulse) | 970 | 6.2 % | 13.3 % (1,444 ns) |
| Other (submit + reclaim + BQL + retry path + xoff counters + stats) | 12,962 | 83.5 % | 71.3 % (7,733 ns) |
| **Total `start_xmit`** | **15,528** | **100 %** | 10,852 ns |

Cache-flush and kick both shrank as a *share* of `start_xmit`; kick also
shrank in absolute terms (970 vs 1,444 ns) — consistent with Track A's
kick-coalescing now amortizing most pulses across several submits (`ethtool
-S eth0` showed the large majority of `tx_kicks_total` landing in
`tx_kicks_threshold`, not `tx_kicks_cold`, in every rep). "Other" grew both
as a share and in absolute terms (+5,229 ns) — expected, since
`start_xmit()` now does real work the v2.4 measurement predates: BQL
accounting, the retry-on-failure path, and the `tx_xoff_seen_xmit` counter
check.

**Table C — round 2 (2026-07-02, same day): splitting the "other" bucket.**
Table B left 83.5 % of `start_xmit` undifferentiated, so a second probe
round (commit `e974949` on the same branch, driver `2.20-tx-rx-probe2`)
added `submit_probe` (brackets the first `rtl8196e_ring_tx_submit()` call)
and `reclaim_probe` (brackets the unconditional reclaim block including its
BQL `netdev_completed_queue()`), run as their own separated set (**Set C**:
submit+reclaim only, 5 reps, median 66.5 Mbit/s, anomaly counters 0):

| Phase | ns/pkt | % of `start_xmit` (15,528) | Basis |
|---|---:|---:|---|
| `dma_cache_wback_inv(skb->data, skb->len)` | 1,597 | 10.3 % | Set A |
| `rtl8196e_ring_kick_tx` | 970 | 6.2 % | Set A |
| `rtl8196e_ring_tx_submit` | 2,684 | 17.3 % | **Set C** |
| **TX-reclaim block** (reclaim + BQL completion) | **7,867** | **50.7 %** | **Set C** |
| Rest (checks + BQL `sent_queue` + stats + probe self-overhead) | 2,411 | 15.5 % | derived |

**The reclaim block is the dominant phase of `start_xmit` — roughly half of
it on its own.** Cross-set caveat: the components sum against Set A's
probe-inflated 15,528 ns total; against the cleaner single-probe
calibration figure (11,901 ns), the four measured components (13,118 ns)
slightly over-subscribe it, consistent with each bracket including ~0.3 µs
of its own overhead — the *ranking* and orders of magnitude are unaffected.

Round 2 also added two always-on context counters
(`tx_reclaimed_xmit`/`tx_reclaimed_poll`, sysfs `tx_reclaim_split`)
answering *where* TX packets actually get reclaimed:

| Reclaim context | Packets (5 reps pooled) | Share | skb-free path taken |
|---|---:|---:|---|
| `start_xmit()` (`napi_budget=0`) | 636,422 | **73.5 %** | `dev_consume_skb_any()` — **bypasses the NAPI bulk freelist** |
| `poll()` (`napi_budget>0`) | 229,036 | 26.5 % | `napi_consume_skb()` freelist recycling |

This confirms the hypothesis that motivated round 2: in a sustained TX
flow the inter-packet interval (~176 µs at 66 Mbit/s) exceeds a frame's
transmission time (~120 µs), so most descriptors complete *between* two
`start_xmit()` calls and get reclaimed there — through the slow,
non-recycled skb-free path. `POST-MORTEM-driver-perf.md` credits the NAPI
bulk skb freelist as one of the mechanisms behind the 5.10→6.18 per-packet
gains; three quarters of this driver's TX frees don't use it.

**Actionable ceiling:** deferring xmit-context skb frees to `poll()`'s
freelist path (or batching the xmit-side reclaim) can recover at most
~7.9 µs/pkt — ~4.5 % of the 175 µs/pkt total budget — realistically less,
since part of the reclaim block (descriptor invalidates, cursor walk, BQL)
remains wherever the free happens. Still the largest untested driver-side
lever identified since Track A (+1.2 %).

Histogram note (applies to every probe in both rounds): the clocksource
ticks at 40 µs granularity on this SoC (25 kHz hardware timer), so
individual samples quantize to 0 / 40 µs / 80 µs... buckets — the
histograms show tick-crossing counts, not a real latency distribution.
The *means* (`sum_ns/count`) remain statistically sound: tick crossings
are uniformly distributed, so over ~170 k samples the summed tick count
converges to the true total duration (sampling error well under 1 % at
these counts).

---

## 9. Interpretation

The driver's own code is **~32 % of total per-packet CPU**, roughly **5×**
the old ~6 % inference. This is not primarily because `start_xmit()` itself
got much more expensive — 10,852 → 15,528 ns is real growth (BQL, the retry
path, the xoff counters), but the same order of magnitude. It is because the
old figure never measured `poll()` at all and silently folded 100 % of
"everything that isn't `start_xmit()`" into "generic stack." `poll()`'s
TX-attributable cost (40,098 ns/pkt) is actually **larger** than
`start_xmit()`'s own (15,528 ns/pkt) — plausibly dominated by the GRO/stack-
delivery work this driver's own source comments already flagged elsewhere
as costing "~180 µs/cycle" inside `napi_complete_done()`, plus RX-ring
housekeeping, TX reclaim, `kick_drain`, and the RUNOUT/stall-detector checks
that all execute inside the same `poll()` call frame.

This does **not** overturn "tuning the driver beyond Track A has no
measurable effect on throughput" (the archived performance record (`PERFORMANCE.md`, "History"), orthogonal-levers
session) — that conclusion came from empirically testing several driver-side
changes and finding ≤1.2 % throughput deltas, and this measurement doesn't
contradict it: operating on a larger *share* of the CPU budget doesn't imply
a proportionally larger *throughput* lever exists there, especially when
per-packet cost is dominated by cache-flush/MMIO latency that driver logic
changes don't reduce. What this measurement does change is *where a future
lever would most plausibly land* if one were sought: `poll()`'s NAPI-batching
behavior (`napi_defer_hard_irqs`, `gro_flush_timeout` — both already tuned,
see the comment in `rtl8196e_probe()`), not `start_xmit()`'s already-lean
~15.5 µs/pkt.

---

## 10. Caveats and limitations

- **Probe self-overhead is folded into the residual, not hidden.** The
  ~3,600 ns/pkt inflation from `cache_probe`/`kick_probe` nesting inside
  `xmit_probe` (§6) is real, but since both `driver_own_ns_per_pkt` and
  `total_cpu_ns_per_pkt` in Set A come from the *same* overhead-inclusive
  window, the *ratio* (`driver_own_pct`) stays internally consistent — it is
  not a hidden bias in the headline percentage, though Set A's absolute
  ns/pkt figures run a few percent hot relative to an unprobed production
  build.
- **`total_cpu_ns_per_pkt` is reported as a range, not a point figure**,
  because `start_xmit` (Set A) and `poll` (Set B) were measured in different
  reps rather than literally the same window (§6's fix for cross-probe
  overhead necessarily gives up strict same-window measurement of every
  quantity at once).
- The retry-on-failure path in `start_xmit()` (added since v2.4, no
  special-casing in the probe design — see the branch's commit message)
  never actually fired during this bench: `tx_ring_full` was 0 in every rep.
  Its handling is a design choice, not empirically exercised here.
- Throughput during every rep (64.9 – 67.5 Mbit/s across both sets) sits in
  the "settled-low" band seen elsewhere in this session (66–67), not the
  historical 69.3–72.8 Mbit/s band documented in the archived performance record (`PERFORMANCE.md`, "History") — a
  session/build variance consideration noted for completeness, not evidence
  that the probes themselves caused a regression (the production, unprobed
  build was separately re-benched and reflashed at the end of the session).
- This document and the underlying probes describe the **6.18 kernel line
  only** — the archive branch this instrumentation lives on does not mirror
  the changes to `files-7.1/`, unlike this write-up itself (kept in sync
  across both overlays as usual, since it's documentation, not source).

---

## 11. Reproduction

```bash
git fetch private perf/tx-rx-decomposition
git checkout private/perf/tx-rx-decomposition
./build_kernel.sh
# IRAM budget gate — must pass before flashing:
mips-lexra-linux-musl-readelf -S linux-6.18-rtl8196e/vmlinux | grep -A1 '\.iram\b'
mips-lexra-linux-musl-nm linux-6.18-rtl8196e/vmlinux | grep rtl8196e_perf_record
./flash_remote.sh -y kernel <gateway-ip>

# On the gateway, enable a probe set and reset counters, e.g. Set B:
ssh root@<gateway-ip> '
    echo N > /sys/module/rtl8196e_eth/parameters/rtl8196e_xmit_probe
    echo N > /sys/module/rtl8196e_eth/parameters/rtl8196e_cache_probe
    echo N > /sys/module/rtl8196e_eth/parameters/rtl8196e_kick_probe
    echo Y > /sys/module/rtl8196e_eth/parameters/rtl8196e_poll_probe
    echo 1 > /sys/class/net/eth0/poll_probe_reset
'
# Run the workload, then read back:
ssh root@<gateway-ip> 'cat /sys/class/net/eth0/poll_probe_stats'
```

Restore the gateway to the actual `optim_tx`/`main` production build
afterward — this branch is never meant to stay flashed on shared hardware.

---

## 12. Follow-up (2026-07-02): the defer-frees experiment — outcome

The "actionable ceiling" identified in §8/Table C was implemented the same
day on `optim_tx` (not on this probe branch — it is a production-candidate
change, no `ktime_get()` involved): `rtl8196e_ring_tx_reclaim()` gained a
`defer` list parameter; the xmit-context reclaim parks completed skbs on
`priv->tx_defer_list` (cap 64, direct-free fallback past it) and
`rtl8196e_poll()` drains them through `napi_consume_skb(skb, budget)`.
Descriptor-slot release and BQL completion timing are unchanged — only the
skb free moves out of `start_xmit()`.

**Mechanism caveat identified before benching:** TCP TX skbs reaching the
driver are fast clones (`tcp_transmit_skb()` transmits a clone of the
write-queue skb), and `napi_consume_skb()` explicitly routes clones to
`__kfree_skb()` — the per-NAPI skb-cache recycling does **not** apply to
them. So the ~7.9 µs ceiling was never fully reachable for TCP: the
realistic gain is batching locality (~14 back-to-back frees per poll,
slab structures hot) plus a shorter `start_xmit()`, not free-path
elimination. This also recontextualizes `POST-MORTEM-driver-perf.md`'s
"bulk skb pool" credit: it applies to RX-path and non-clone skbs, not to
TCP TX completions.

**A/B/A result** (same session, 5/10/5 reps, drift-controlled by
re-flashing the baseline afterward): baseline medians 66.8 / 66.9
(A1/A2 — no drift), defer build median **67.4** (mean 67.9) —
**+0.55 Mbit/s (+0.8 %), suggestive but not conclusive** (t ≈ 1.7,
p ≈ 0.11; B's central cluster sits consistently ~0.55 above A's).
Correctness clean across all 21 reps (0 retrans, ring-anomaly counters 0,
120 s stress, no leak over 1.5 M packets, `tx_defer_queued` 63 % of
packets, cap never hit). **Kept on `optim_tx`** with the honest
"suggestive" label — full gate table in the archived performance record (`PERFORMANCE.md`, "History"),
"Driver v2.20 defer-frees gate run".
