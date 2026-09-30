# RTL8196E Ethernet driver (`rtl8196e-eth`) — cumulative audit

> **Cumulative audit ledger.** The current-state table and the §4 finding
> registry are authoritative. The per-pass sections keep the technical
> conclusions of each pass; they do not override a later disposition.

| Current state | Authoritative value |
|---|---|
| **Audited code** | driver 2.24 |
| **Last audit pass** | 2026-07-30 — A4, LED functional validation/fix |
| **Last fully audited baseline** | v2.23 |
| **Post-baseline changes** | v2.24: board-aware true LAN LED off (ETHDRV-016); no datapath change |
| **Validation state** | A2 hardening bench/field evidence plus A3 static revalidation; A4 true-off mechanism visually validated on Lidl and compiled for both boards and both kernel lines; Sengled G4 not yet visually checked |
| **Maintained kernels** | Linux 6.18 and 7.2, identical driver source |
| **Current finding registry** | §4 |
| **Audited surface** | driver sources/headers, Kconfig/Makefile, DT/config, DMA/cache/ring lifecycle and privileged controls |

## Audit-pass ledger

| Pass | Date | Baseline | Result | Validation |
|---|---|---|---|---|
| A0 | 2026-04-23…05-03 | driver 2.2 → 2.5 | F1–F17, ETH-001…008 and ETHDRV-001…006 | historical; detail in `AUDIT.md` at tag v3.10.0 |
| A1 | 2026-06-12 | 2.6 → 2.13 | ETHDRV-007…012 and simplification batch ETH-S01…S07 | build plus target testing |
| C1 | 2026-06-15…07-08 | 2.13 → 2.18 | issue #99 recovery and diagnostics: ETHDRV-013…015 | fault injection and field evidence |
| A2 | 2026-07-17 | 2.18 → 2.23 | independent hardening audit (§2b) | build, target networking and recovery checks |
| A3 | 2026-07-30 | 2.23 | no new defect; security/performance contracts revalidated (§2c) | static source review |
| A4 | 2026-07-30 | 2.23 → 2.24 | ETHDRV-016: `off` was physically DIM; pad-mux/GPIO fix (§2d) | register readback + visual Lidl validation; four builds |

Pass A1 replaced the earlier cumulative log (A0). All A0 finding IDs remain
resolvable in §4 because the other driver documents and the GPIO audit
reference them. The bench tables of the rejected A0 experiments (F6,
F11/F13/F15, the three ETHDRV-003 patch variants) are in the superseded
`AUDIT.md`, readable in the public release tag v3.10.0
(`git show v3.10.0:<path of this file>`). Per-release throughput is in
`PERFORMANCE.md`; the per-version bench entries cited below are in
PERFORMANCE.md, History (tag v4.4.0).

Scope: single netdev (`eth0`, port 4 on the Lidl board), NAPI + GRO-defer
tuning, KSEG1 (uncached) descriptor rings over KSEG0 (cached) descriptor
pools with explicit `dma_cache_*` discipline, SP/SC TX ring, `__iram` hot
paths. The release bench (`scripts/bench_release_iperf3.sh`) flags a TCP TX
median below `THR_TX_FLOOR` (69 Mbit/s) or a TCP RX median below
`THR_RX_FLOOR` (88 Mbit/s); a TX median 2 Mbit/s or more below the previous
release's median is a regression. The safe-default RX checksum costs ~2–4 %
(§1.3).

---

## 1. Security analysis

### 1.1 Attack surface

| Surface | Reachable by | Driver exposure |
|---|---|---|
| RX frames + ASIC-written descriptors | anyone on the wire / DMA | `rx_poll` trusts nothing: pkthdr and mbuf pointers pool-bounds-checked with index fallback (`rx_wild_*` counters), `ph_len` clamped to `[ETH_ZLEN, buf_size]` before `skb_put`, shadow skb looked up by **hardware** mbuf index with `< rx_cnt` guard (driver 2.6) |
| TX path (skb from stack) | local stack | nonlinear skbs linearised; short frames `skb_put_padto(ETH_ZLEN)` **before** cache flush — no slab-tail leak on the wire (ETH-001); len > 1518 rejected (`tx_bad_len`) |
| Devicetree | build-time (trusted) | `vlan-id` 1–4095, `member-ports` ⊆ 0x1ff and ≠ 0, `untag ⊆ member`, `mtu` 576–1500, `ifname` via `strscpy` (ETH-006) |
| sysfs (`led_mode`, `kick_threshold`) | root only (0644 dev attrs) | `sysfs_streq` whitelist / `kstrtouint` + range 1..64; no parsing of attacker data |
| module params (3 × 0644) | root only | operational knobs by design (ETHDRV-005): `rtl8196e_rx_stall_thresh`, `link_poll_ms`, `rtl8196e_cpu_port_mask`; the port mask is consumed by `open()` and recovery reprogramming, stray high bits are masked by the table packers |
| ethtool ops | `CAP_NET_ADMIN` | fixed-size string table, `data[]` filled to exactly `RTL8196E_ETHTOOL_STATS_COUNT`; no variable-length copy |

### 1.2 Verified correct in the current code

- **TX zero-padding before DMA** (`skb_put_padto` ahead of
  `dma_cache_wback_inv`) — the ETH-001 information-leak fix is present
  and ordered correctly; the ring-level `len < ETH_ZLEN → len = ETH_ZLEN`
  raise is only defence in depth.
- **TX handover ordering**: descriptor fields → `dma_cache_wback_inv` →
  `wmb()` → ownership flip (WRAP-preserving) → `wmb()`. RX rearm mirrors
  it (skb buffer + ph + mb wback before the SWCORE_OWNED store).
- **SP/SC TX ring**: `start_xmit` runs with BH disabled and NAPI is a
  softirq on the same single CPU — no true concurrency;
  `READ_ONCE`/`WRITE_ONCE` on `tx_prod`/`tx_cons` prevent compiler tearing.
  Ring-full check keeps one slot open (`next == tx_cons` → `-ENOSPC`).
- **ISR discipline** (F7): reads `CPUIISR ∧ CPUIIMR`, returns `IRQ_NONE`
  without acking when nothing is owned, W1Cs only the owned bits.
- **`tx_timeout` is atomic** (§2b F1): the watchdog callback runs in a
  timer softirq under `dev->tx_global_lock` and only stops the queue +
  `schedule_work(&swcore_reset_work)`. The sleeping recovery
  (`napi_disable`, ~650 ms switch reset, both-ring reset per ETHDRV-013,
  `led_mode` replay) runs in the worker; a failed reprogram latches a
  bounded-retry hold-down (§2b).
- **MAC change refused while UP** (F2): prevents a silent NETIF/L2
  desync; next `open()` reprograms both tables from `dev_addr`.
- **Descriptor ABI guards**: `BUILD_BUG_ON` on sizes/offsets (ETH-004)
  and the `__BIG_ENDIAN_BITFIELD` `#error` (ETHDRV-004).
- **Table-engine failure paths** (F10): TLU timeout aborts the write
  with `-EIO`; `open()` falls back to trap-to-CPU mode when the L2
  toCPU entry cannot be installed or verified.
- **Sleeping waits in process context** (F3/F4): `msleep` /
  `usleep_range` throughout `hw_init` and the poll-ready loops.
- **DT refcounts**: `for_each_child_of_node` early-return hands an
  elevated ref to the caller, which `of_node_put`s on every exit.
- **PIN_MUX_SEL (0x40) writes** match the documented v1.2 UART1 fix:
  bits [4:3] = 01 preserved, MII bits cleared; bits 1/6 are owned by
  `8250_rtl819x` and untouched here. The **PIN_MUX_SEL_2 (0x44)** write
  derives each B2–B6 pad field from the GPIO controller node (ETHDRV-007):
  named in `gpio-line-names` → `0b11` (GPIO), listed in
  `realtek,led-pads` → `0b00` (LED_PORTn), neither → `0b11` left as
  unclaimed GPIO (Hi-Z — Table 36 has no disable encoding); `[17:15]`
  (MII) always cleared. Property absent → every unnamed pad falls back to
  `0b00` (third-party DTB compatibility).

### 1.3 ETHDRV-003 — RX checksum integrity (resolved v2.23, trade reversed)

Through v2.17 the RX path applied **blanket** `CHECKSUM_UNNECESSARY`: the
A0 characterisation showed bad-UDP-checksum frames reaching userland
sockets, but all three *gated* variants then tried regressed the bench
13–33 Mbit/s, so blanket trust was kept (accepted as "case D").

The A2 re-audit (§2b F5) re-weighted this: silently handing bad-checksum
frames to sockets is the wrong default for a border-router NIC, and the
cost of the *safe* direction had never been isolated from the flawed
gating variants. **v2.23 reverses the trade.** `rtl8196e_rx_set_csum()`
defaults to `CHECKSUM_NONE` and grants `CHECKSUM_UNNECESSARY` only to the
one characterised case — unfragmented IPv4 **UDP** with **both**
`CSUM_IP_OK` and `CSUM_TCPUDP_OK` set; every other protocol/encapsulation
(TCP, VLAN, IPv6, fragments) is validated by the stack. `NETIF_F_RXCSUM`
is advertised and honoured, so the policy is togglable with `ethtool -K`.
The isolated cost, from a same-build runtime A/B (blanket vs gated) at P8,
is ~4.4 % under parallel load and ~2 % single-stream (PERFORMANCE.md,
History (tag v4.4.0), driver v2.23). CSCR has no "reject bad L3/L4 toward
CPU" bit (SDK V3.4.7.3, `rtl8196e_regs.h`) — hence a software policy, not
a hardware gate. This section is the reference for ETHDRV-003.

### 1.4 Verdict

No remotely exploitable memory-safety flaw found in the current code. The
RX hot path validates every hardware-influenced value before use, and the
TX path does not leak slab memory. The A1 medium findings (ETHDRV-007,
ETHDRV-008) and the A2 findings (§2b) are robustness/correctness items,
plus the one integrity reversal in §1.3.

---

## 2. Findings ETHDRV-007…016 (A1, C1, A4)

| ID | Type | Severity | Status | One-liner |
|----|------|----------|--------|-----------|
| ETHDRV-007 | ROBUSTNESS (cross-driver) | **medium** | fixed v2.7/v2.8 | `hw_init()` (then in `ndo_open`) cleared PIN_MUX_SEL_2 fields owned by held GPIO lines — broke `efr32-nrst` control after any `eth0` down/up |
| ETHDRV-008 | ROBUSTNESS (DMA) | **medium** | fixed v2.9 | ring arrays written via KSEG1 without flushing the cached alias first — stale dirty lines could later evict over live descriptors |
| ETHDRV-009 | ROBUSTNESS | low | fixed v2.9 | `stop()` masked IRQs before `napi_disable()`; a racing NAPI completion re-armed `CPUIIMR` on a downed interface |
| ETHDRV-010 | HARDENING | low | fixed v2.9 | probe `request_irq`d before quiescing `CPUIIMR`/`CPUIISR`; bootloader-latched state could fire the ISR on a not-yet-registered netdev |
| ETHDRV-011 | ROBUSTNESS (debug) | info | closed v2.10 (deleted) | `dbg_timer_fn` dereferenced ring-entry pointers without pool-bounds checks |
| ETHDRV-012 | API | info | fixed v2.9 | no `ndo_change_mtu`: a live MTU change did not reach the NETIF table until the next `open()` |
| ETHDRV-013 | ROBUSTNESS (correctness) | **high** | mitigated v2.14 — insufficient alone, see ETHDRV-015 | `tx_timeout()` rebuilt only the TX ring, desyncing `rx_idx` → `PKTHDR_DESC_RUNOUT` IRQ storm (the issue #99 soft-lockup) |
| ETHDRV-014 | ROBUSTNESS | low | fixed v2.14 | with TX_ALL_DONE masked, a TX queue stopped while no RX arrives had no reclaim path until the 10 s netdev watchdog |
| ETHDRV-015 | ROBUSTNESS (correctness) | **high** | fixed v2.15 | the RUNOUT storm is self-sustaining regardless of how the desync is entered, and the NAPI poll had no escape |
| ETHDRV-016 | FUNCTIONAL | low | fixed v2.24 | `led_mode=off` left the LAN LED visibly DIM |

### ETHDRV-007 — eth re-cleared GPIO-owned mux fields on every open (medium)

`rtl8196e_hw_init()` cleared the B4/B5/B6 fields ([7:6]/[10:9]/[13:12])
and MII bits of PIN_MUX_SEL_2 (0x44), and ran from `rtl8196e_open()` — on
every interface up. `gpio-rtl819x` sets those fields to `0b11` (GPIO) only
at line-request time, so after `ip link set eth0 down/up` a held line
(`efr32-nrst` on B4 on the Lidl board; the button on B5/B6 on the Sengled
G4) silently left GPIO mode and every later nRST pulse was a no-op until
reboot. This is the driver-side counterpart of **GPIO-007**
(`drivers/gpio/AUDIT.md` §2), which defers the fix to this driver.

**Resolution.** v2.7: ownership comes from the **DT contract**, not the
current register value — `hw_init()` reads the GPIO controller's
`gpio-line-names`; named pad → `0b11`, unnamed → `0b00`, `[17:15]` always
cleared. A held line now gets the same `0b11` re-asserted, and named
on-demand lines (nRST, blmode) get a deterministic GPIO-input state from
boot. v2.8 (#126) closed the residual exposure of an anonymous cdev claim:
with `realtek,led-pads` present, only listed pads get `0b00`, everything
else unnamed stays `0b11` (Hi-Z). The LED pad is declared, not derived
from `member-ports`, because only a visual check proves which pad drives
the LED — Lidl port 4 → B6/LED_PORT4, G4 port 0 → B2/LED_PORT0, both
verified by eye after a wrong B2 value for Lidl killed the LAN LED
(register readback cannot catch a wrong LED pad). Since v2.11 (ETH-S03)
the write runs once at probe.

### ETHDRV-008 — no cache flush of ring-array memory before KSEG1 aliasing (medium)

`rtl8196e_alloc_uncached()` kmallocs the three descriptor ring arrays
(`tx_ring`, `rx_pkthdr_ring`, `rx_mbuf_ring`) and returns the KSEG1 alias.
Lines from the memory's previous lifetime may still sit dirty in the
write-back L1 (`kfree` does not flush); a later eviction would write them
over ring entries written via KSEG1. The descriptor pools already got a
full `dma_cache_wback_inv` after init; the ring arrays did not. The
`dma_cache_inv` on a KSEG1 address in `tx_reclaim` (F11 heritage) happened
to cover the TX ring only.

**Resolution (v2.9).** `dma_cache_wback_inv()` on the cached pointer inside
`rtl8196e_alloc_uncached()` before returning the alias; one-time, probe
only. Full iperf gate passed.

### ETHDRV-009 — stop() ordering let a racing NAPI re-arm IRQs (low)

`stop()` ran `netif_stop_queue` → `disable_irqs` → `hw_stop` → W1C →
`napi_disable`; an in-flight poll finishing with `work_done < budget`
re-enabled `CPUIIMR` on a downing interface. Harmless while `hw_stop` had
already cleared TXCMD/RXCMD/TRXRDY, but the masking was illusory.

**Resolution (v2.9).** `napi_disable()` first. An RX IRQ landing between
`napi_disable` and the mask is benign: the ISR W1C-acks and
`napi_schedule_prep` refuses on a disabled NAPI.

### ETHDRV-010 — probe-time IRQ window before netdev registration (low)

Probe called `request_irq()` before quiescing `CPUIIMR`/`CPUIISR` and
before `register_netdev()`. The bootloader uses this NIC for TFTP, so a
latched `LINK_CHANGE_IP` could drive `netif_carrier_on/off` on a
not-yet-registered netdev. **Resolution (v2.9):**
`rtl8196e_hw_disable_irqs()` + W1C `CPUIISR` in probe before
`request_irq`.

### ETHDRV-011 — debug timer skipped the pool-bounds discipline (info)

`rtl8196e_dbg_timer_fn()` dereferenced raw ring entries without
`rtl8196e_ptr_in_pool` checks — able to oops exactly when an operator
enabled `rtl8196e_debug=1` to observe a corrupt entry. **Resolution
(v2.10):** closed by deletion with ETH-S02.

### ETHDRV-012 — live MTU change not propagated to hardware (info)

Without `ndo_change_mtu`, the core updated `ndev->mtu` only; the NETIF
table kept the open-time value. **Resolution (v2.9):** F2 pattern —
`-EBUSY` while UP, accepted while down (the next `open()` programs the
NETIF table from `ndev->mtu` in `rtl8196e_hw_netif_setup()`).

### ETHDRV-013 — tx_timeout rebuilt only TX, desyncing RX into a RUNOUT storm (high)

`rtl8196e_tx_timeout()` rebuilt only the TX ring, then cycled the switch
with `hw_stop()`/`hw_start()`. `hw_start()` re-asserts TRXRDY, which
rewinds the switch RX engine to descriptor 0, while `rx_idx` and the RX
ring bases were left untouched. The switch finds no RISC-owned descriptor
where it expects one and latches `PKTHDR_DESC_RUNOUT` (`CPUIISR` bit 17);
`napi_complete` W1C-clears it each poll and the switch re-asserts it — an
interrupt storm (~100 k/s measured) with zero progress, pinning the single
CPU in `__napi_poll` until the hardware watchdog resets the SoC. This
matches the issue #99 field signature (`napi=[rtl8196e_poll]`, RUNOUT
latched, `rx_packets` frozen).

**Resolution (v2.14).** `rtl8196e_ring_rx_reset()` +
`rtl8196e_hw_set_rx_rings()` added to the recovery, symmetric with
`open()`/`stop()` (`DESIGN.md` invariant 9). A bench TX timeout that
previously stormed recovers cleanly. A field unit carrying this fix still
hit #99, which led to ETHDRV-015; ETHDRV-013 stays as defence in depth.

### ETHDRV-014 — no-RX TX stall waited out the netdev watchdog (low)

With TX_ALL_DONE masked (`DESIGN.md` invariant 5), TX reclaim runs only in
`start_xmit` and the RX-woken NAPI poll. A queue stopped (ring-full or BQL
XOFF) while no RX arrives had no reclaim path until the 10 s netdev
watchdog fired `tx_timeout` — which then tripped ETHDRV-013. BQL (v2.13)
lowered the stop threshold and made this a routine trigger on an idle
border router.

**Resolution (v2.14).** A per-device software timer
(`RTL8196E_TX_RECLAIM_MS`, 4 ms), armed whenever `start_xmit` or the poll
leaves the queue stopped; its callback schedules NAPI (reclaim +
`netdev_completed_queue` + wake) and it lapses once the queue drains. The
arm is `timer_pending()`-guarded so the TX hot path pays nothing once
armed (an unconditional `mod_timer` per packet had cost ~5 % TX).

### ETHDRV-015 — RUNOUT storm had no poll-side escape (high)

The storm is self-sustaining regardless of how the desync is entered: a
poll that finds `rx_pkthdr_ring[rx_idx]` `SWCORE_OWNED` does zero work and
does not advance `rx_idx`, then `napi_complete_done` re-W1Cs RUNOUT and
re-enables IRQs against the unchanged starved ring. ETHDRV-013 closed only
the `tx_timeout` entry. The Realtek SDK has the same RX architecture but
ships a runtime stuck-detector, `rtl_check_swCore_tx_hang()` →
`rtl865x_reinitSwitchCore()`, which the from-scratch rewrite had dropped.

**Resolution (v2.15).** Two detectors feed the shared full resync
`rtl8196e_hw_ring_resync()`: poll-side, after
`RTL8196E_RUNOUT_RESYNC_THRESH` (3) consecutive zero-work polls with RUNOUT
asserted (CPUIISR read only on a zero-work poll, so no hot-path cost); and
a periodic ~1 s watchdog (`RTL8196E_SWCORE_CHECK_MS`) that schedules a poll
if RUNOUT stays asserted across `RTL8196E_SWCORE_HANG_THRESH` (3) checks.
Ethtool counters `rtl8196e_rx_runout_resync` / `rtl8196e_rx_runout_kick`
stay 0 unless a storm hit. After a further field recurrence, the watchdog
panic record gained an eth snapshot (`rtl8196e_eth_panic_snapshot()`,
contract in `include/linux/rtl8196e_eth_panic.h`; diagnostic-only, v2.16/
v2.17), and v2.18 added the deep switch-core reset escalation and TX-done
hang detection (see `CHANGELOG.md`, whose entry for that change is labelled
ETHDRV-016 — a different item from ETHDRV-016 in this ledger). Full
analysis: `issue99.md` in the public release tag v4.6.0.

### ETHDRV-016 — `led_mode=off` was physically DIM (low)

The A4 visual gate found LAN reading back `off` but visibly DIM with
`LEDCREG=0` and `DIRECTLCR=0`. Per the SDK definitions, topology zero is
scan mode and DIRECTLCR is a duty-scale register, not an output disable.

**Resolution (v2.24).** The driver claims the active-low `lan-led-gpios`
pad, preloads its inactive GPIO level, and changes only that pad's
PIN_MUX_SEL_2 field: GPIO for OFF, LED_PORTn for BRIGHT/DIM. The DT
supplies Lidl B6 and Sengled G4 B2; no pad is hard-coded. Visually
confirmed dark on Lidl; both overlays build the same source and both board
DTBs compile. Slow-path sysfs/recovery change only.

---

## 2b. Independent re-audit A2 (2026-07-17, driver 2.18 → 2.23)

A from-scratch audit (code only, this file not consulted) merged with an
external audit of the same date. Every hunk passed the full iperf gate; the
two perf-sensitive ones (cache-line pkthdr, safe-default checksum) were
A/B-measured. Fault injection exercised the recovery FSM: a transient
reprogram failure auto-recovers after one retry; a persistent one does 3
retries at 1/2/4 s, then holds the interface down with device IRQs never
re-opened, and an admin `ip link` down/up recovers.

The F-numbers in this table are local to A2 and distinct from the A0
F-series in §4.

| Finding | Type | Sev | Resolution (driver) |
|---|---|---|---|
| **F1** — `tx_timeout` sleeps in atomic ctx | correctness | high | atomic callback → `swcore_reset_work` (2.22) |
| **F2** — RX pkthdr false-sharing on 32-B lines | DMA geometry | medium | cache-line-strided pkthdr slots (2.22) |
| **F3** — coalesced kick strands a `SWCORE_OWNED` TX descriptor on an RX-silent link → phantom deep reset | robustness | medium | batch-end (`xmit_more` clear) `kick_drain` (2.22) |
| **F4** — 1700-B RX buffers vs `MBUF_2048BYTES` | — | — | rejected: ingress max-len reset default is 1536, no overflow path (below) |
| **F5** — blanket `CHECKSUM_UNNECESSARY` | integrity | medium | safe-default gated policy + `NETIF_F_RXCSUM` (2.23, §1.3); F5-A register variant rejected (below) |
| **F6** — `min_mtu` 68 < HW floor 576 | correctness | low | `min_mtu = 576` (2.22) |
| **F7** — worker wakes queue on a downing iface | robustness | low | re-test `netif_running` before the final wake (2.22) |
| **F8** — deep reset rewrites `LEDCREG`, clobbering `led_mode` | robustness | low | replay operator mode after reset (2.22) |
| **N8** — recovery hold-down | robustness | medium | failed-reprogram latch + bounded 3× 1/2/4 s retry, then admin-recover only (2.22) |
| Shared `napi_kick` (mask-first) | hardening | low | one helper for ISR + both timer kicks; closes a SCHED-with-IRQs-enabled window (2.22) |
| Table-engine timeouts | robustness | low | bounded `TLU_BUSY` → `-ETIMEDOUT` + SWTCR/TLU restore; `hw_swcore_reset` returns int (2.22) |
| Pool-bounds stride | hardening | low | `ptr_in_pool` requires element alignment; exact TX/RX sub-pools (2.22) |
| 64-bit stats | stats | low | `u64_stats_sync` + `ndo_get_stats64` (2.23) |
| `Kconfig` UP-only | hardening | info | `depends on ... && !SMP` — the lockless model is UP-only (2.22) |
| **N2** / **N6** | doc / cosmetic | info | stale `rx_reset` comment; `l2_check_last` errno sign in ethtool (2.22) |

### Rejected on-target — two switch-register writes

Both were built, flashed to the bench gateway and **took eth0 down**
(recovered via serial console + a known-good reflash). The silicon reset
defaults are kept (`DESIGN.md` invariant 12):

- **AcptMaxLen (PCR bits [2:1]), from F4.** SDK V3.4.7.3 shows the reset
  default is 1536 and the driver's PCR RMWs preserve [2:1], so the switch
  drops >1536 at ingress, well under the 1700-B buffer — there was never a
  remote-overflow path. Writing the field explicitly (even to 1552)
  wedged the port. `rtl8196e_regs.h` keeps the `AcptMaxLen_*` defines as
  reference only.
- **AcceptL2Err (CSCR bit 3), F5-A.** The driver sets `EXCLUDE_CRC`, so the
  CPU injects FCS-less frames on TX; `AcceptL2Err=1` (reset default) is what
  lets the CPU port accept them. Clearing it left TX dead (setting bit 3
  again restored it). `rtl8196e_hw_l2_setup()` clears only CSCR bits 0–2.

In the same cycle `hw_init`'s L2-table clear became warn-not-fatal.

---

## 2c. Static revalidation A3 (2026-07-30, driver 2.23)

A static re-read of the v2.23 sources and their DT/Kconfig contract (the
driver sources were byte-identical across the two kernel overlays, then
6.18 and 7.1). No image was built or flashed and no bench was run.

**Security: no new finding.** Still holding: every RX ring pointer is
stripped of ownership bits, checked against the exact cache-line-strided
pkthdr or mbuf sub-pool and required to land on an element boundary;
`ph_len` must fit the Ethernet minimum, `mtu + VLAN_ETH_HLEN +
ETH_FCS_LEN` and the 1700-byte buffer; every consumed RX descriptor
(including drop/OOM/corruption paths) is charged against the NAPI budget;
short TX frames are zero-padded before writeback, frames above 1518 are
rejected; descriptor contents are flushed before the ownership flip, with
barriers on both RX rearm and TX submit; recovery paths do not sleep in
atomic context, are serialized through one work item and keep carrier,
queue and IRQs down after a failed reprogram; the lockless model is
restricted to UP by `Kconfig: depends on !SMP`.

Residual trust boundary: this is a non-IOMMU SoC and the switch ASIC can
DMA into the memory assigned to it. The pool checks protect the CPU from
corrupted descriptor pointers and lengths; they are not isolation against
a malicious bus master. DT, module parameters and the two sysfs controls
are administrative inputs.

**Performance: no unbounded or accidental per-packet work.**

| Path | Bounded cost and assessment |
|---|---|
| RX | at most `budget` descriptors; one replacement skb per delivered frame; packet-data invalidate plus pkthdr/mbuf writeback-invalidate on rearm; GRO and 2 ms NAPI IRQ deferral retain the measured batching gain |
| TX | opportunistic reclaim bounded by the 127 usable ring slots; packet cache writeback required for non-coherent DMA; TXFD MMIO pulses coalesced and drained at `xmit_more` batch end |
| TX skb release | xmit-side deferred list capped at 64 entries and drained by NAPI; beyond the cap, immediate release |
| Background | switch watchdog: a few MMIO reads once per second; link polling disabled by default; the 4 ms reclaim timer exists only while the TX queue is stopped |
| Recovery/open | table walks and the ~650 ms silicon reset run only in process context; `open()` ~30 ms |

The remaining throughput ceiling is architectural (per-packet skb
replacement/cache maintenance on RX, non-coherent DMA/MMIO on TX).
`page_pool`/`build_skb`, descriptor shadows or a different cache strategy
would be a redesign and must be A/B bench-gated on the RTL8196E.

---

## 2d. LAN LED functional validation A4 (2026-07-30, driver 2.23 → 2.24)

The visual gate drove STATUS and LAN through BRIGHT, DIM and OFF on the
Lidl board. STATUS passed; LAN `off` stayed DIM — ETHDRV-016 (§2), fixed
in v2.24.

---

## 3. Kernel 6.18 simplification / optimization review (A1)

| ID | Status | Proposal and outcome |
|----|--------|----------------------|
| ETH-S01 | done v2.12 (resource-claim variant) | DT node declares three windows (`cpu-interface` 0x1000, `asic-table` 0x100000, `switch-core` 0x8000); probe claims + maps them via `devm_platform_ioremap_resource()` and **fails probe** if a mapped base differs from the compile-time KSEG1 constant — the constants become an enforced invariant, `/proc/iomem` is honest. Retires the hardcoded-KSEG1 class (F17 / ETH-005 / ETHDRV-006). Routing MMIO through the mapped base is **rejected**: on MIPS ioremap returns the same KSEG1 alias, so it would only replace foldable `lui+lw/sw` pairs with dependent loads in the `__iram` hot paths (the F11/F13/F15 class, −47 Mbit/s) |
| ETH-S02 | done v2.10 | bring-up scaffolding removed (`dbg_timer`, `tx_debug_once`, four `tx_dbg_*` ethtool slots, ISR `dbg_irqs`, `rtl8196e_force_trap`, the `rtl8196e_debug` parameter, six dbg-only ring accessors; −~190 lines). `rtl8196e_tx_kicks_*` stats are bench telemetry and stay. Closes ETHDRV-011 |
| ETH-S03 | done v2.11 | one-time SoC bring-up (`rtl8196e_hw_init()`: pinmux, switch-clock toggle with its 650 ms of sleeps, MEMCR, FULL_RST, L2 table clear) moved from `ndo_open` to probe, before the ETHDRV-010 quiesce; `open()` >1 s → ~30 ms. nRST verified after a down/up flap |
| ETH-S04 | done v2.10 | dead defines dropped (`PIN_MUX_SEL`/`PIN_MUX_SEL2`, `TX_ALL_DONE_IE_ALL`/`IP_ALL`, `PortStatusNWayEnable`); Kconfig help figures → 94/73; `depends on !RTL819X` dropped (symbol absent from the 6.18 tree) |
| ETH-S05 | done v2.13, kept after bench gate | BQL on the single TX queue: `netdev_sent_queue` after a final submit, `netdev_completed_queue` at the three reclaim sites, `netdev_reset_queue` next to every ring reset. Both sides count `skb->len`. Gate: RX 94.0 / P4 93.8 / P8 93.9 / TX 69.5, limit converged ~2.5 KB; ≤1 % cost (PERFORMANCE.md, History (tag v4.4.0), v2.13). Correctness rule: `DESIGN.md` invariant 5 corollary |
| ETH-S06 | done v2.10 | never-set `rtl8196e_hw.base` dropped; `(void)hw` casts removed, `hw` parameter kept (F16) |
| ETH-S07 | done v2.10 | `tx_submit` no longer takes `flags`; `PKTHDR_USED | PKT_OUTGOING` owned by the ring layer |

### Considered and rejected

- **devm-converting probe**: devm would release the IRQ after `remove()`
  has `free_netdev`d the `dev_id`; manual `free_irq`-before-`free_netdev`
  is correct.
- **Per-CPU 64-bit stats**: rejected as needless on UP; superseded by v2.23,
  which uses one shared `u64_stats_sync` accumulator.
- **phylib / fixed-link**: the four PHYs are internal to the switch ASIC
  (PSRPx/MDIO vendor registers); the ethtool shim plus link timer/IRQ is
  smaller and loses nothing this hardware can express.
- **KSEG1 descriptor pools** (F6), **rearm `wback_inv → inv`** (F13),
  **WRAP-from-index** (F15), **`dma_cache_inv` removal** (F11): all
  hardware-tested and rejected — −1.2 to −47 Mbit/s. Do not revisit bundled.
- **Gated RX checksum** (ETHDRV-003): the A0 variants regressed
  ≥13 Mbit/s; superseded in v2.23 by the safe-default policy (§1.3).
- **AcptMaxLen / AcceptL2Err register writes** (§2b): rejected, reset
  defaults load-bearing (`DESIGN.md` invariant 12).
- **Scatter-gather TX**: rejected before 6.18 (`SPECIFICATIONS.md` §2).

---

## 4. Finding ID registry (complete)

A0, first pass 2026-04-23 (driver 2.2 → 2.3), F-series:

| ID | Status | One-liner |
|----|--------|-----------|
| F1 | fixed | tx_timeout napi_disables around the ring reset (now in the worker, §2b F1) |
| F2 | fixed | MAC change refused while UP |
| F3 | fixed | `mdelay` → `msleep` in hw_init (650 ms, process ctx) |
| F4 | fixed | poll-ready loops use `usleep_range` |
| F5 | fixed | RX drop/bad-len paths update rx_errors/rx_dropped |
| F6 | rejected (bench) | KSEG1 descriptor pools: TCP TX −1.2 Mbit/s |
| F7 | fixed | ISR masks before W1C, owned bits only |
| F8 | fixed | sysfs via `attribute_group` |
| F9 | fixed | kick_tx through the MMIO helpers |
| F10 | fixed | table_write aborts on TLU timeout |
| F11/F13/F15 | rejected (bench) | ring micro-opts bundle: −47 Mbit/s RX |
| F12 | fixed | `ph->ph_mbuf` pool-bounds check in rx_poll |
| F14 | fixed | stop() W1Cs latched CPUIISR |
| F16 | intentional → closed | `(void)hw` vestigial API (ETH-S06, v2.10) |
| F17 | intentional → closed | KSEG1 virtual addresses in HW DMA registers (ETH-S01, v2.12) |

A0, second pass 2026-05-01 (driver 2.3 → 2.4; the CHANGELOG writes these
as `RTL8196E-ETH-00n`): ETH-001 (TX zero-padding, fixed), ETH-002 (ring
resets in stop(), fixed), ETH-003 (= ETHDRV-003), ETH-004 (BUILD_BUG_ON ABI
guards, fixed), ETH-005 (= F17), ETH-006 (DT port-mask bounds, fixed),
ETH-007 (debug params opt-in, intentional), ETH-008 (= F13, stays
rejected).

A0, third pass 2026-05-03 (driver 2.4 → 2.5): ETHDRV-001 (syscon
EPROBE_DEFER, fixed), ETHDRV-002 (canonical descriptor rearm, fixed),
ETHDRV-003 (blanket CHECKSUM_UNNECESSARY — resolved in v2.23, §1.3),
ETHDRV-004 (BE-bitfield compile guard, mitigated), ETHDRV-005 (0644 debug
params, intentional), ETHDRV-006 (= F17).

Driver 2.6 (no audit pass): shadow-skb lookup by hardware mbuf index,
TX/RX pool-bounds validators, `ethtool -S` anomaly counters.

A1, 2026-06-12 (driver 2.6 → 2.13): ETHDRV-007 (closed v2.7/v2.8),
ETHDRV-008/009/010/012 (closed v2.9), ETHDRV-011 (closed v2.10 by
deletion); ETH-S01…S07 (§3, all done).

C1, 2026-06-15…07-08 (driver 2.13 → 2.18): ETHDRV-013 (mitigated v2.14,
insufficient alone), ETHDRV-014 (fixed v2.14), ETHDRV-015 (fixed v2.15).

A2, 2026-07-17 (driver 2.18 → 2.23): §2b F1–F8, N2, N6, N8 and the
unnumbered hardening rows — all implemented except F4 and F5-A (rejected).

A4, 2026-07-30 (driver 2.23 → 2.24): ETHDRV-016 (fixed v2.24).

Cross-references: **GPIO-007** (`drivers/gpio/AUDIT.md`) is ETHDRV-007
seen from the GPIO side; both closed in v2.7. The CSCR bit-layout notes in
`rtl8196e_regs.h` (Realtek SDK V3.4.7.3) derive from the ETHDRV-003
characterisation. `CHANGELOG.md` uses "ETHDRV-016" for the v2.15 → v2.18
switch-core self-recovery (§2, ETHDRV-015); in this ledger ETHDRV-016 is
the LAN LED fix.

---

## 5. Conclusion

Every actionable finding is implemented; the open items are the
evidence-backed rejections in §2b and §3. No open memory-safety item
remains. The v2.24 change is limited to the LED slow path; true OFF is
validated on Lidl, while the Sengled G4 still needs a visual board check.
