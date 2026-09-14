# RTL8196E Ethernet driver (`rtl8196e-eth`) — cumulative audit

> **Cumulative audit ledger.** The current-state table and §4 finding registry
> are authoritative. Earlier findings and conclusions remain in this file as
> pass-scoped evidence; they do not override a later disposition.

| Current state | Authoritative value |
|---|---|
| **Current implementation** | `rtl8196e-eth` v2.24 |
| **Release target** | firmware v4.0.0 |
| **Last audit pass** | 2026-07-30 — A4, LED functional validation/fix |
| **Last fully audited baseline** | v2.23 |
| **Post-baseline changes** | v2.24: board-aware true LAN LED off (ETHDRV-016); no datapath change |
| **Validation state** | A2 hardening bench/field evidence plus A3 static revalidation; A4 true-off mechanism visually validated on Lidl and compiled for both boards/kernel lines |
| **Maintained kernels** | Linux 6.18 and 7.1, identical driver source |
| **Current finding registry** | §4 |
| **Audited surface** | driver sources/headers, Kconfig/Makefile, DT/config, DMA/cache/ring lifecycle and privileged controls |

## Audit-pass ledger

| Pass | Date | Baseline | Result | Validation |
|---|---|---|---|---|
| A0 | 2026-04-23…05-03 | early driver | F1–F17, ETH-001…008 and ETHDRV-001…006 | historical; superseded detail remains in git |
| A1 | 2026-06-12 | pre-v2.10 → v2.13 | security/lifecycle findings and simplification batch | build plus .88 target testing |
| C1 | 2026-06-15…07-08 | v2.13 → v2.18 | issue #99 recovery and diagnostic evolution | fault injection and field evidence |
| A2 | 2026-07-17 | v2.18 → v2.23 | independent hardening audit | build, target networking and recovery checks |
| A3 | 2026-07-30 | v2.23 | no new defect; security/performance contracts revalidated | static source/core review |
| A4 | 2026-07-30 | v2.23 → v2.24 | ETHDRV-016: `off` was physically DIM; pad-mux/GPIO fix | register readback + visual Lidl validation; four builds |

Pass A1 **supersedes and replaces** the cumulative audit log of
2026-04-23 / 2026-05-01 / 2026-05-03 (passes F1–F17, ETH-001..008,
ETHDRV-001..006). It was based on that pass's code. All legacy
finding IDs remain resolvable in the registry (§4) because several are
referenced from code comments (`ETHDRV-003` in `rtl8196e_ring.c` and
`rtl8196e_regs.h`) and from the GPIO driver audit (GPIO-007
cross-reference). The detailed bench tables of the rejected experiments
(F6, F11/F13/F15, the three ETHDRV-003 patch variants) live in the
superseded AUDIT.md, retrievable via `git log` on this file;
`PERFORMANCE.md` keeps the per-release throughput history.

Scope baseline: single netdev (`eth0`, port 4 on the Lidl board), NAPI +
GRO-defer tuning, KSEG1 (uncached) descriptor rings over KSEG0 (cached)
descriptor pools with explicit `dma_cache_*` discipline, SP/SC TX ring,
`__iram` hot paths. Current bench baselines (driver v2.23 / kernel
6.18.38): ~90 Mbit/s TCP RX median (the safe-default checksum costs
~2–4 %, §1.3), ~70 Mbit/s TCP TX median in the 69.3–72.8 spread — see
`PERFORMANCE.md` and `32-Kernel/CLAUDE.md`.

---

## 1. Security analysis

### 1.1 Attack surface

| Surface | Reachable by | Driver exposure |
|---|---|---|
| RX frames + ASIC-written descriptors | anyone on the wire / DMA | `rx_poll` trusts nothing: pkthdr and mbuf pointers pool-bounds-checked with index fallback (`rx_wild_*` counters), `ph_len` clamped to `[ETH_ZLEN, buf_size]` before `skb_put`, shadow skb looked up by **hardware** mbuf index with `< rx_cnt` guard (driver 2.6) |
| TX path (skb from stack) | local stack | nonlinear skbs linearised; short frames `skb_put_padto(ETH_ZLEN)` **before** cache flush — no slab-tail leak on the wire (ETH-001); len > 1518 rejected (`tx_bad_len`) |
| Devicetree | build-time (trusted) | `vlan-id` 1–4095, `member-ports` ⊆ 0x1ff and ≠ 0, `untag ⊆ member`, `mtu` 576–1500, `ifname` via `strscpy` (ETH-006) |
| sysfs (`led_mode`, `kick_threshold`) | root only (0644 dev attrs) | `sysfs_streq` whitelist / `kstrtouint` + range 1..64; no parsing of attacker data |
| module params (3 × 0644) | root only | operational knobs by design (ETHDRV-005); `rtl8196e_cpu_port_mask` is consumed by `open()` and recovery reprogramming, stray high bits are masked by the table packers |
| ethtool ops | `CAP_NET_ADMIN` | fixed-size string table, `data[]` filled to exactly `RTL8196E_ETHTOOL_STATS_COUNT`; no variable-length copy |

### 1.2 Verified correct in the current code

- **TX zero-padding before DMA** (`skb_put_padto` ahead of
  `dma_cache_wback_inv`) — the ETH-001 information-leak fix is present
  and ordered correctly; the ring-level `len < ETH_ZLEN → len = ETH_ZLEN`
  raise is now only defence in depth (those bytes are zeroed).
- **TX handover ordering**: descriptor fields → `dma_cache_wback_inv` →
  `wmb()` → ownership flip (WRAP-preserving) → `wmb()`. RX rearm mirrors
  it (skb buffer + ph + mb wback before the SWCORE_OWNED store).
- **SP/SC TX ring**: `start_xmit` runs with BH disabled (netdev xmit
  path) and NAPI is a softirq on the same single CPU — no true
  concurrency; `READ_ONCE`/`WRITE_ONCE` on `tx_prod`/`tx_cons` prevent
  compiler tearing. Ring-full check keeps one slot open
  (`next == tx_cons` → `-ENOSPC`).
- **ISR discipline** (F7): reads `CPUIISR ∧ CPUIIMR`, returns `IRQ_NONE`
  without acking when nothing is owned, W1Cs only the owned bits.
- **`tx_timeout` is atomic** (§2b F1): the watchdog callback runs in a
  timer softirq under `dev->tx_global_lock` and only stops the queue +
  `schedule_work(&swcore_reset_work)`. The sleeping recovery
  (`napi_disable`, ~650 ms switch reset, both-ring reset per ETHDRV-013,
  `led_mode` replay) runs in the worker; on a failed reprogram it latches
  a bounded-retry hold-down (§2b). The former inline `napi_disable()` was
  a sleep-in-atomic under exactly the load a timeout accompanies (fixed
  v2.22).
- **MAC change refused while UP** (F2): prevents a silent NETIF/L2
  desync; next `open()` reprograms both tables from `dev_addr`.
- **Descriptor ABI guards**: `BUILD_BUG_ON` on sizes/offsets (ETH-004)
  and the `__BIG_ENDIAN_BITFIELD` `#error` (ETHDRV-004) are present.
- **Table-engine failure paths** (F10): TLU timeout aborts the write
  with `-EIO`; `open()` falls back to trap-to-CPU mode when the L2
  toCPU entry cannot be installed or verified.
- **Sleeping waits in process context** (F3/F4): `msleep` /
  `usleep_range` throughout `hw_init` and the poll-ready loops.
- **DT refcounts**: `for_each_child_of_node` early-return hands an
  elevated ref to the caller, which `of_node_put`s on every exit.
- **PIN_MUX_SEL (0x40) writes** match the documented v1.2 UART1 fix:
  bits [4:3] = 01 preserved, MII bits cleared; bits 1/6 are owned by
  `8250_rtl819x` and untouched here. Since v2.7 the **PIN_MUX_SEL_2
  (0x44)** write derives each B2–B6 pad field from the GPIO controller
  node — the board, not the driver, decides pad ownership (closes
  ETHDRV-007). v2.8 makes the rule three-state: named in
  `gpio-line-names` → `0b11` (GPIO), listed in `realtek,led-pads` →
  `0b00` (LED_PORTn), neither → `0b11` left as unclaimed GPIO (Hi-Z —
  Table 36 has no disable encoding); `[17:15]` (MII) always cleared.
  Property absent → every unnamed pad falls back to `0b00` (v2.7
  behaviour, third-party DTB compatibility).

### 1.3 ETHDRV-003 — RX checksum integrity (resolved v2.23, trade reversed)

Through v2.17 the RX path applied **blanket** `CHECKSUM_UNNECESSARY` and
accepted the integrity gap (case D): empirical characterisation
(2026-05-03, superseded log) showed bad-UDP-checksum frames reaching
userland sockets, but all three *gated* variants regressed the bench
13–33 Mbit/s, so blanket-trust was kept.

The independent re-audit (§2b, F5) re-weighted this: silently handing
bad-checksum frames to sockets is the wrong default for a border-router
NIC, and the cost of the *safe* direction had never been isolated from
the (separately flawed) gating variants. **v2.23 reverses the trade.**
`rtl8196e_rx_set_csum()` now defaults to `CHECKSUM_NONE` and grants
`CHECKSUM_UNNECESSARY` only to the one characterised case — unfragmented
IPv4 **UDP** with **both** `CSUM_IP_OK` and `CSUM_TCPUDP_OK` set; every
other protocol/encapsulation (TCP, VLAN, IPv6, fragments) is validated
by the stack. `NETIF_F_RXCSUM` is advertised and honoured, so the policy
is togglable with `ethtool -K`. The isolated cost, from a same-build
runtime A/B (blanket vs gated) at P8, is ~4.4 % under parallel load and
~2 % single-stream — a bounded, deliberate integrity-for-throughput
trade, not the old mystery regression (the archived performance record (`PERFORMANCE.md`, "History"), driver v2.23).
The claim that CSCR has a "reject bad L3/L4 toward CPU" bit stays false
(SDK V3.4.7.3, `rtl8196e_regs.h`) — hence a software policy, not a
hardware gate. The `ETHDRV-003` comment references in
`rtl8196e_ring.c`/`rtl8196e_regs.h` now point here.

### 1.4 Verdict

No remotely exploitable memory-safety flaw found in the current code.
The RX hot path validates every hardware-influenced value before use,
and the TX path no longer leaks slab memory. The two medium findings
below were a cross-driver ownership violation (ETHDRV-007 — closed in
v2.7) and a latent cache-aliasing hazard at ring creation (ETHDRV-008)
— both robustness, not exploitability, on this platform. The 2026-07-17
re-audit (§2b) found no new memory-safety flaw either; its findings are
robustness/correctness (atomic `tx_timeout`, RX pkthdr false-sharing,
recovery hold-down) plus the one integrity reversal in §1.3.

---

## 2. New findings (this audit)

| ID | Type | Severity | One-liner |
|----|------|----------|-----------|
| ETHDRV-007 | ROBUSTNESS (cross-driver) | **medium** | *(closed in v2.7)* `hw_init()` (ndo_open) cleared PIN_MUX_SEL_2 fields owned by held GPIO lines — silently broke `efr32-nrst` control after any `ifconfig eth0 up` |
| ETHDRV-008 | ROBUSTNESS (DMA) | **medium** | ring arrays are written via KSEG1 without flushing the cached alias first — stale dirty lines can later evict over live descriptors |
| ETHDRV-009 | ROBUSTNESS | low | `stop()` masks IRQs before `napi_disable()`; a racing NAPI completion re-arms `CPUIIMR` on a downed interface |
| ETHDRV-010 | HARDENING | low | probe `request_irq`s before quiescing `CPUIIMR`/`CPUIISR`; bootloader-latched state (the bootloader uses this NIC for TFTP) can fire the ISR on a not-yet-registered netdev |
| ETHDRV-011 | ROBUSTNESS (debug) | info | `dbg_timer_fn` dereferences ring-entry pointers without the pool-bounds checks the hot paths use (root-only, `rtl8196e_debug` gated) |
| ETHDRV-012 | API | info | no `ndo_change_mtu`: a live MTU change updates `ndev->mtu` but not the NETIF table until the next `open()` |
| ETHDRV-013 | ROBUSTNESS (correctness) | **high** | *(mitigated in v2.14 — **insufficient alone**, see ETHDRV-015)* `tx_timeout()` rebuilt only the TX ring; its `hw_stop()/hw_start()` rewinds the switch RX engine to descriptor 0, desyncing `rx_idx` → continuous `PKTHDR_DESC_RUNOUT` IRQ storm pinning the CPU in `__napi_poll` = the issue-#99 soft-lockup. v2.14 made the recovery resync RX too, closing the `tx_timeout` door — but a v2.7 field unit (olivluca) carrying this fix still recurred after ~3.7 days |
| ETHDRV-014 | ROBUSTNESS | low | *(fixed in v2.14)* with TX_ALL_DONE masked, a TX queue stopped while no RX arrives has no reclaim path and waits out the 10 s netdev watchdog (→ tx_timeout, → ETHDRV-013) — a no-RX/low-RX TX workload stalls; fixed with a software TX-reclaim timer |
| ETHDRV-015 | ROBUSTNESS (correctness) | **high** | *(fixed in v2.15)* the #99 RUNOUT storm is **self-sustaining regardless of how the desync is entered**, and the NAPI poll has no escape (zero-work-under-RUNOUT → re-enable → re-storm). ETHDRV-013 only closes one entry (`tx_timeout`). Fix: a poll-side detector that, after N consecutive zero-work polls with `PKTHDR_DESC_RUNOUT` asserted, runs a full ring resync (`rtl8196e_hw_ring_resync`), plus a ~1 s periodic watchdog — restoring the vendor SDK's `rtl_check_swCore_tx_hang`→`reinitSwitchCore` safety net our rewrite dropped. Full analysis: `issue99.md` |
| ETHDRV-016 | FUNCTIONAL | low | *(fixed in v2.24)* `led_mode=off` wrote `LEDCREG=0` (scan/DIM) and `DIRECTLCR=0` (minimum duty), so the LAN LED retained a visible glow; true off remuxes the board-declared active-low pad to an inactive GPIO output |

### ETHDRV-007 — eth re-clears GPIO-owned mux fields on every open (medium)

`rtl8196e_hw_init()` (`rtl8196e_hw.c:322`) does
`regmap_update_bits(syscon, 0x44, (3<<6)|(3<<9)|(3<<12)|(7<<15), 0)` and
is called from `rtl8196e_open()` — i.e. on **every** interface up, not
once at boot. Fields [7:6]/[10:9]/[13:12] are the B4/B5/B6 pad muxes
that `gpio-rtl819x` sets to `0b11` (GPIO mode) at line-request time. On
the Lidl board B4 is `efr32-nrst`, held by the uart-bridge via gpiod
with open-drain emulation.

Consequence: after any `ip link set eth0 down/up` while the radio is
running, the B4 pad silently leaves GPIO mode. The line stays
*requested* in gpiolib, so nothing re-runs the request-time mux — every
subsequent nRST pulse (EFR32 recovery, `flash_efr32.sh` bootloader
entry) is a no-op until reboot. The `0b00` value historically chosen
here keeps nRST deasserted on the Lidl board, so the radio does not
reset spontaneously — the failure is the *loss of reset control*, which
is exactly the failure class the v3.9.0 nRST rework eliminated. On
boards using B5/B6 as GPIO (Sengled G4 button), the same clobber hits
input lines.

This is the driver-side counterpart of **GPIO-007** (see
`drivers/gpio/AUDIT.md` §2) — the fix belongs here. Recommendation,
in preference order:

1. Move the whole one-time pinmux + clock + reset bring-up out of
   `ndo_open` into probe (couples with ETH-S03). Probe runs before the
   uart-bridge requests the line (and can be ordered against it), so a
   single boot-time clear keeps the historical "nRST not driven low
   before the bridge takes over" guarantee without ever undoing a
   live GPIO claim.
2. Alternatively, make the clear ownership-aware: read each 2-bit
   field first and skip any field already at `0b11` (the "a GPIO
   consumer owns this pad" convention from the GPIO audit).

**Resolution (v2.7, v3.11.0-pre).** Fixed by a third, stronger variant
of recommendation 2: ownership comes from the **DT contract** rather
than from the current register value. `hw_init()` reads the GPIO
controller's `gpio-line-names` and writes each B2–B6 field accordingly —
named pad → `0b11` (GPIO), unnamed → `0b00` (LED_PORTn), `[17:15]`
always cleared. `ndo_open` still runs the write, but for a held GPIO
line it now *re-asserts* the same `0b11` the gpio-rtl819x request hook
set, so a `down/up` no longer un-muxes anything; it also gives named
on-demand lines (nRST, blmode) a deterministic GPIO-input state from
boot — bench-measured: nRST floats high via the EFR32 pull-up before any
claim. Residual exposure (v2.7): a line claimed via the cdev *without* a
DTS name still got `0b00` on every open. **Closed by v2.8** (#126,
hlyi's report): with `realtek,led-pads` present on the GPIO node, only
listed pads get `0b00`; named pads stay `0b11` and *everything else* is
left as unclaimed GPIO (`0b11`, Hi-Z — Table 36 has no disable
encoding) — so an anonymous cdev claim keeps its GPIO mux across
`down/up`, and no unknown wiring is ASIC-driven. The LED pad is
declared, not derived from `member-ports`, because it is a wiring fact
only a visual check proves (a member port may have no LED wired). Both
known boards do follow the Table 36 1-1 naming — Lidl port 4 →
B6/LED_PORT4, G4 port 0 → B2/LED_PORT0, both verified by eye after the
Lidl value first shipped wrong as B2 and killed the LAN LED (register
readback cannot catch a wrong LED pad, #126). ETH-S03
(hoist bring-up to probe) remains open as a pure latency item — it no
longer carries the ownership fix.

### ETHDRV-008 — no cache flush of ring-array memory before KSEG1 aliasing (medium)

`rtl8196e_alloc_uncached()` (`rtl8196e_ring.c:87`) kmallocs the three
descriptor *ring arrays* (`tx_ring`, `rx_pkthdr_ring`, `rx_mbuf_ring`)
and returns the KSEG1 alias; from then on the CPU touches them only
uncached. But the underlying lines may still sit **dirty in the
write-back L1** from the memory's previous lifetime (a freed skb, any
prior slab user — `kfree` does not flush). A later eviction writes the
stale line back to DRAM, clobbering ring entries that were written via
KSEG1 — corrupt ownership bits or wild descriptor pointers. The
defensive `tx_bad_pkthdr`/`rx_wild_*` checks would degrade it from an
oops to dropped slots, but it is a real one-shot corruption window.

The driver demonstrably knows the rule — the descriptor *pools* get a
full `dma_cache_wback_inv` after init (lines 263–264), and the rejected
F6 experiment flushed before aliasing — the ring arrays just never got
the same treatment. Note the odd `dma_cache_inv` on a KSEG1 address in
`tx_reclaim` (F11 heritage): on this virtually-indexed,
physically-tagged cache it can hit and discard the stale KSEG0-alias
line, accidentally protecting the **TX** ring at reclaim time; the two
RX rings have no such accidental cover.

Fix (3 lines, when implemented): `dma_cache_wback_inv((unsigned long)p,
size)` on the cached pointer inside `rtl8196e_alloc_uncached()` before
returning the alias. Cost: one-time, probe only.

**Resolution (v2.9).** Implemented exactly as above. Full iperf gate
passed (see the archived performance record (`PERFORMANCE.md`, "History"), v2.9 entry).

### ETHDRV-009 — stop() ordering lets a racing NAPI re-arm IRQs (low)

`rtl8196e_stop()` order is: `netif_stop_queue` → `disable_irqs` →
`hw_stop` → W1C → `napi_disable`. A NAPI poll already in flight when
IRQs are masked can finish with `work_done < budget`, and its
`napi_complete_done()` branch then calls `rtl8196e_hw_enable_irqs()` —
re-arming `CPUIIMR` on an interface going down. Today this is harmless
(`hw_stop` has already cleared TXCMD/RXCMD/TRXRDY, so no event can
latch), but the masking is illusory and the pattern breaks if `stop()`
ever relies on IRQs being off. Conventional fix: `napi_disable()`
(which waits out the in-flight poll) before `disable_irqs`/`hw_stop`;
the ring resets already sit correctly after both.

**Resolution (v2.9).** Reordered as recommended. The short window where
an RX IRQ can land between `napi_disable` and the mask is benign: the
ISR W1C-acks and `napi_schedule_prep` refuses on a disabled NAPI — no
storm, verified in-code and exercised by a live down/up under traffic
on the bench.

### ETHDRV-010 — probe-time IRQ window before netdev registration (low)

`rtl8196e_probe()` calls `request_irq()` (which unmasks the line at the
interrupt controller) before any quiesce of `CPUIIMR`/`CPUIISR` and
before `register_netdev()`. The bootloader uses this NIC for TFTP and
is not guaranteed to leave the interface masked, so the ISR can run
immediately: `napi_schedule_prep` safely refuses (NAPI still in its
disabled state), but a latched `LINK_CHANGE_IP` would drive
`netif_carrier_on/off` on a **not-yet-registered** netdev. Cheap
hardening: `rtl8196e_hw_disable_irqs()` + W1C `CPUIISR` in probe before
`request_irq` (mirrors what `stop()` already does).

**Resolution (v2.9).** Implemented as recommended.

### ETHDRV-011 — debug timer skips the pool-bounds discipline (info)

`rtl8196e_dbg_timer_fn()` derives `ph`/`rx_ph`/`rx_mb` from raw ring
entries and `dma_cache_inv`s + dereferences them with none of the
`rtl8196e_ptr_in_pool` checks the hot paths apply. A corrupt entry —
precisely the situation in which an operator would enable
`rtl8196e_debug=1` — can oops the debug path that was meant to observe
it. Root-only and off by default; fold into ETH-S02 (retire or harden
the scaffolding) rather than patching in place.

**Resolution (v2.10).** Closed by deletion — the whole scaffolding is
gone with ETH-S02.

### ETHDRV-012 — live MTU change not propagated to hardware (info)

Without an `ndo_change_mtu`, the core accepts any MTU in
`[68, iface.mtu]` and updates `ndev->mtu` only; the NETIF table keeps
the open-time value until the next `open()`. Harmless in practice
(buffers are 1700 B, HW limit 1518 stands), but either propagating the
change or rejecting it while UP (the F2 pattern) would remove the
silent inconsistency.

**Resolution (v2.9).** F2 pattern: `-EBUSY` while UP, accepted while
down (the next `open()` programs the NETIF table from `ndev->mtu` —
verified that is the value `rtl8196e_hw_netif_setup()` consumes).
Bench: `ip link set eth0 mtu 1400` refused UP, accepted down.

### ETHDRV-013 — tx_timeout rebuilds only TX, desyncing RX into a RUNOUT storm (high)

`rtl8196e_tx_timeout()` recovered the interface by reclaiming and
rebuilding the **TX** ring only (`ring_tx_reclaim` → `ring_tx_reset` →
`hw_set_tx_ring`), then cycling the switch with `hw_stop()`/`hw_start()`.
But `hw_start()` re-asserts TRXRDY, which rewinds the switch's RX engine to
descriptor 0, while the driver's `rx_idx` and the RX ring bases were left
untouched. The two pointers desync: the switch sees no RISC-owned RX
descriptor where it expects one and latches `PKTHDR_DESC_RUNOUT`
(`CPUIISR` bit 17). `napi_complete` W1C-clears the bit at the end of each
poll; the switch re-asserts it the next cycle. The result is a spurious
interrupt storm (~100 k/s measured) with zero forward progress that pins
the single CPU in `__napi_poll` until the hardware watchdog resets the SoC.

This is the long-hunted **issue #99** soft-lockup. The field signature
(record-v4 panic record: `napi=[rtl8196e_poll]`, RUNOUT latched,
`rx_packets` frozen, eth IRQ ~99 k/s) matches the bench reproduction
exactly: an early-boot TX timeout left RX desynced at `rx_idx=3` and the
box stormed. The trigger is any `tx_timeout` — see ETHDRV-014 for the
no-RX path that made one fire routinely on an idling border router.

`open()` and `stop()` both reset *both* rings; only the `tx_timeout`
recovery was asymmetric. The defect is a missing pair of calls, not a
wrong one.

**Resolution (v2.14).** Added `rtl8196e_ring_rx_reset()` +
`rtl8196e_hw_set_rx_rings()` to the recovery, symmetric with
`open()`/`stop()`. Reproduced and validated on the bench: a TX timeout
that previously stormed (eth IRQ ~99 k/s, RUNOUT latched, `rx_packets`
frozen) now recovers cleanly (eth IRQ <1/s, no storm, no panic). Shipped
as a **candidate pending field confirmation** from the #99 soakers;
olivluca's field record-v4 capture (`napi=[rtl8196e_poll]`) confirmed the
eth-NAPI engine. `DESIGN.md` invariant 9 makes the both-rings symmetry
load-bearing.

**Field follow-up (2026-06-19) — insufficient alone.** A v2.7 unit carrying
this fix (olivluca, `v3.8.5`) still hit #99 after ~3.7 days. A full review
(see `issue99.md`) showed the storm is **self-sustaining regardless of entry**
and the poll has no escape, so closing the `tx_timeout` door does not close
#99 if the desync arises by any other means. The engine fix is **ETHDRV-015**;
ETHDRV-013 is retained as one-fewer-door defence in depth.

### ETHDRV-014 — no-RX TX stall waits out the netdev watchdog (low)

With TX_ALL_DONE deliberately masked (DESIGN invariant 5), TX reclaim runs
only in `start_xmit` and the RX-woken NAPI poll. A TX queue that stops —
ring-full XOFF or BQL byte-limit XOFF — while no RX is arriving then has no
path to reclaim its in-flight descriptors: `start_xmit` is no longer called
(queue stopped) and NAPI is not scheduled (no RX IRQ). The queue sits
stopped until the 10 s netdev watchdog fires `tx_timeout`. A purely-TX or
low-RX workload — a Thread border router idling with no paired peer — hit
this every 10 s, and each `tx_timeout` then tripped ETHDRV-013. This is the
trigger that turned the rare ETHDRV-013 race into a chronic bench reproducer
once v2.13 BQL lowered the queue-stop threshold (the field condition —
sparse RX from a few paired peers — is the rarer, days-to-reproduce form).

**Resolution (v2.14).** A short per-device software timer
(`RTL8196E_TX_RECLAIM_MS`, 4 ms), armed whenever `start_xmit` or the poll
leaves the queue `netif_xmit_stopped`. Its callback `napi_schedule`s
(reclaim + `netdev_completed_queue` + wake), and the poll re-arms it while
the queue stays stopped; it lapses once the queue drains — bounded, never
runs on a moving queue. The arm is guarded by `timer_pending()` so the TX
hot path pays nothing once armed (an unconditional `mod_timer` per packet
cost ~5 % TX, fixed in the same v2.14 cycle — see the archived performance record (`PERFORMANCE.md`, "History")).
`timer_setup` in probe, `timer_delete_sync` in stop. Bench: an idling
border router that fired a TX timeout every 10 s and dropped SSH now fires
none and stays stable; TCP TX back to baseline.

### ETHDRV-015 — RUNOUT storm has no poll-side escape; ETHDRV-013 is insufficient alone (high)

A v2.7 field unit (olivluca, `v3.8.5`) carrying ETHDRV-013 still hit #99 after
~3.7 days. A full adversarial review (the comprehensive write-up is in
`issue99.md`, with the original Realtek SDK cross-check) established:

- The storm is **self-sustaining regardless of how the desync is entered**. The
  NAPI poll has no escape: a poll that finds `rx_pkthdr_ring[rx_idx]`
  `SWCORE_OWNED` does zero work and does not advance `rx_idx`
  (`ring.c`: `rx_idx` is written only in the re-arm path and in `rx_reset`),
  then `napi_complete_done` re-W1Cs RUNOUT and re-enables IRQs against the
  unchanged starved ring (`main.c rtl8196e_poll`), and the switch re-asserts the
  next cycle. ETHDRV-013 only closes the `tx_timeout` entry — any other entry
  (a second `tx_timeout`, a hardware/link-driven TRXRDY rewind) lands in the
  same trap.
- The original Realtek SDK has the **same** RX architecture and **the same
  storm-prone poll**, but never exhibits #99 because (a) it has no `ndo_tx_timeout`
  partial reset and (b) it ships a runtime stuck-detector,
  `rtl_check_swCore_tx_hang()` → `rtl865x_reinitSwitchCore()` (full
  cursor+engine re-init). Our from-scratch rewrite dropped that safety net.

**Resolution (v2.15).** Restore the safety net, NAPI-friendly. Two detectors,
both feeding the shared full resync `rtl8196e_hw_ring_resync()` (the
`open()`/`tx_timeout` reset+rearm+TRXRDY sequence, factored out):

1. **Poll-side (primary):** after `RTL8196E_RUNOUT_RESYNC_THRESH` (3) consecutive
   zero-work polls with `PKTHDR_DESC_RUNOUT` asserted, resync from poll context
   (no `napi_disable` — we are the poll); the `napi_complete_done` tail then
   re-enables IRQs against an armed, in-sync ring. Breaks the storm in ~µs.
   CPUIISR is read only on a zero-work poll (`&&` short-circuit) → no hot-path cost.
2. **Periodic (belt-and-suspenders):** a ~1 s watchdog timer
   (`RTL8196E_SWCORE_CHECK_MS`); if RUNOUT stays asserted across
   `RTL8196E_SWCORE_HANG_THRESH` (3) checks it `napi_schedule`s a poll — covering
   a non-CPU-pinning stall where NAPI is not otherwise driven. Off the datapath.

Two ethtool counters expose firing: `rtl8196e_rx_runout_resync` (resyncs done)
and `rtl8196e_rx_runout_kick` (periodic kicks); both stay 0 unless a storm hit.
ETHDRV-013 and ETHDRV-014 are retained as one-fewer-door defence in depth.

**Field recurrence (2026-06-24) — RC1 did not hold.** A soaker (frtz13) on
`v4.0.0-rc2` (driver v2.15) hit #99 again after ~4.3 days; the panic record is
the rc2 build (`rtl8196e_swcore_check_timer_fn` present, `rtl8196e_poll+0x0/0x1c8`)
with the original signature. Two surviving hypotheses with opposite fixes —
(A) the detector never fired (storm yields ≥1 packet within 3 polls, or is an
`RX_DONE` storm not a `PKTHDR_DESC_RUNOUT` one → widen the gate), or (B) the
resync fired and the storm continued (TRXRDY rewind insufficient → full
`reinitSwitchCore`). The v2.15 counters that would decide this reset on reboot.
**Resolution deferred; instrument first:** the watchdog panic record was
extended to capture an eth #99 snapshot at panic (`rtl8196e_eth_panic_snapshot()`
in `rtl8196e_main.c`, contract in `include/linux/rtl8196e_eth_panic.h`;
diagnostic-only, no datapath change). **v5** records `resync`/`kick`/`zero`/`seen`
+ live `CPUIISR`/`CPUIIMR` + `rx_idx` (answers A vs B); **v6** adds switch-core /
TX / ring-progress state (`rxdesc`@rx_idx, `tx_prod`/`cons`/`free`, `txdesc`@tx_cons,
`CPUICR`, `SIRR`, `rx`/`tx_packets`) to also catch a broader switch-core or TX-done
stall — the vendor stuck-detector watched TX-done, not only RX runout. Driver v2.17,
wdt v1.9 / record v6. Detail in `issue99.md` §12.

---

## 2b. Independent re-audit (2026-07-17, driver 2.18→2.23)

A from-scratch second audit (code only, `AUDIT.md` not consulted) merged with
a parallel external report (`AUDIT-RTL8196E-ETH-2026-07-17.md`). The two
converged; the merged actionable set is below. Every hunk passed the full
iperf gate on `.88`; the two perf-sensitive ones (cache-line pkthdr,
safe-default checksum) were A/B-measured. Fault-injection on `.88` exercised
the recovery FSM end to end — a transient reprogram failure auto-recovers
after one retry; a persistent one does 3 retries at 1/2/4 s then holds the
interface down with device IRQs never re-opened, and an admin `ip link`
down/up recovers.

| Finding | Type | Sev | Resolution (driver) |
|---|---|---|---|
| **F1** — `tx_timeout` sleeps in atomic ctx | correctness | high | atomic callback → `swcore_reset_work` (2.22) |
| **F2** — RX pkthdr false-sharing on 32-B lines | DMA geometry | medium | cache-line-strided pkthdr slots (2.22) |
| **F3** — coalesced kick strands a `SWCORE_OWNED` TX descriptor on an RX-silent link → phantom deep reset | robustness | medium | batch-end (`xmit_more` clear) `kick_drain` (2.22) |
| **F5** — blanket `CHECKSUM_UNNECESSARY` | integrity | medium | safe-default gated policy + `NETIF_F_RXCSUM` (2.23, §1.3) |
| **F6** — `min_mtu` 68 < HW floor 576 (an `up` after a sub-576 `mtu` set -EINVALs) | correctness | low | `min_mtu = 576` (2.22) |
| **F7** — worker wakes queue on a downing iface | robustness | low | re-test `netif_running` before the final wake (2.22) |
| **F8** — deep reset rewrites `LEDCREG`, clobbering `led_mode` | robustness | low | replay operator mode after reset (2.22) |
| Recovery hold-down (N8) | robustness | medium | failed-reprogram latch + bounded 3× 1/2/4 s retry, then admin-recover only (2.22) |
| Shared `napi_kick` (mask-first) | hardening | low | one helper for ISR + both timer kicks; closes a SCHED-with-IRQs-enabled window (2.22) |
| Table-engine timeouts | robustness | low | bounded `TLU_BUSY` → `-ETIMEDOUT` + SWTCR/TLU restore; `hw_swcore_reset` returns int (2.22) |
| Pool-bounds stride | hardening | low | `ptr_in_pool` requires element alignment (`(addr-base) % stride == 0`); exact TX/RX sub-pools (2.22) |
| 64-bit stats | stats | low | `u64_stats_sync` + `ndo_get_stats64` — non-wrapping RX/TX totals (2.23) |
| `Kconfig` UP-only | hardening | info | `depends on ... && !SMP` — the lockless model is UP-only (2.22) |
| N2 / N6 | doc / cosmetic | info | stale `rx_reset` "called only from .ndo_stop" comment; `l2_check_last` errno sign in ethtool (2.22) |

### Considered and rejected on-target — two switch-register writes (the v2.22 brick)

The first re-audit pass proposed **programming two switch registers to their
"safer" values**; both were built, flashed to `.88`, and **bricked eth0** —
recovered only via serial console + a known-good reflash. Both are documented
**rejected**, with the silicon reset defaults kept:

- **AcptMaxLen (PCR bits [2:1]).** F4 flagged the 1700-B RX buffers against a
  `MBUF_2048BYTES` `CPUICR` and proposed writing an explicit ingress max-len.
  SDK V3.4.7.3 shows the **reset default is 1536** and the driver's PCR RMWs
  already preserve [2:1], so the switch drops >1536 at ingress well under the
  1700 buffer — there was never a remote-overflow path (the external audit's
  synthesis was right; F4 was over-alarmed). Writing the field explicitly
  (even to a valid 1552) **wedged the port**. Kept at reset default;
  `rtl8196e_regs.h` carries the `AcptMaxLen_*` defines as *reference only*
  with this note.
- **AcceptL2Err (CSCR bit 3).** F5-A proposed *clearing* it to reject FCS-bad
  frames toward the CPU. But the driver sets `EXCLUDE_CRC`, so the CPU
  **injects FCS-less frames** on TX, and `AcceptL2Err=1` (reset default) is
  what lets the CPU port accept them. Clearing it left eth0 up but **TX
  dead** — confirmed by `devmem 0x1B804048 32 0x8` (re-setting bit 3)
  restoring TX on the bench. Kept at reset default; `rtl8196e_hw_l2_setup()`
  clears only CSCR bits 0–2 and documents why bit 3 stays.

Lesson, in `DESIGN.md` invariant 12: neither field may be programmed away from
its reset default — each reads like a safe hardening write and each takes the
interface down on this switch. `hw_init`'s L2-table clear was also made
**warn-not-fatal** in the same cycle (a best-effort clear failure must not
fail probe).

---

## 2c. Static security/performance revalidation (2026-07-30, driver 2.23)

This pass re-read the v2.23 sources and their DT/Kconfig contract from the
current tree. The driver sources are byte-identical between the 6.18 and 7.1
overlays, and no driver code changed after the 2026-07-17 re-audit. This was a
static revalidation: no new image was built or flashed and no new throughput
bench was run. The measured figures in the header therefore remain the latest
hardware evidence; this section does not claim a new runtime qualification.

### Security and robustness result

**No new finding.** In particular, the previously high-risk boundaries still
hold:

- every RX ring pointer is stripped of ownership bits, checked against the
  exact cache-line-strided pkthdr or mbuf sub-pool, and required to land on an
  element boundary before dereference;
- `ph_len` is rejected unless it fits all three constraints: Ethernet minimum,
  `mtu + VLAN_ETH_HLEN + ETH_FCS_LEN`, and the 1700-byte backing buffer;
- the RX loop charges every consumed descriptor against the NAPI budget,
  including drop/OOM/corruption paths, so hostile runt or allocation-failure
  traffic cannot create an unbounded softirq loop;
- short TX frames are zero-padded before cache writeback and DMA handover;
  nonlinear skbs are linearised, and the ring rejects frames above 1518 bytes;
- descriptor contents are flushed before the ownership word is flipped, with
  barriers on both RX rearm and TX submit;
- watchdog/timer/NAPI recovery paths do not sleep in atomic context, are
  serialized through one work item, and keep carrier, queue and device IRQs
  down after a failed reprogram;
- the lockless ring/counter model remains explicitly restricted to UP builds by
  `Kconfig: depends on !SMP`.

The residual trust boundary is unchanged: this is a non-IOMMU SoC driver and
the switch ASIC can DMA into the memory assigned to it. The pool checks protect
the CPU from corrupted descriptor pointers and lengths; they are not isolation
against a malicious bus master capable of arbitrary DMA. Devicetree, module
parameters and the two sysfs controls are administrative inputs, not remote
packet inputs.

### Performance result

No new unbounded or accidentally per-packet work was found. The hot-path costs
are explicit and match the existing bench decisions:

| Path | Bounded cost and assessment |
|---|---|
| RX | at most `budget` descriptors; one replacement skb per delivered frame; packet-data invalidate plus pkthdr/mbuf writeback-invalidate on rearm; GRO and 2 ms NAPI IRQ deferral retain the measured batching gain |
| TX | opportunistic reclaim is bounded by the 127 usable ring slots; packet cache writeback is required for non-coherent DMA; TXFD MMIO pulses are coalesced and drained at `xmit_more` batch end |
| TX skb release | the xmit-side deferred list is capped at 64 entries and drained by NAPI; exceeding the cap falls back to immediate release, so memory and latency are bounded |
| Background | the switch watchdog costs a few MMIO reads once per second; link polling is disabled by default; the 4 ms reclaim timer exists only while the TX queue is stopped |
| Recovery/open | table walks and the ~650 ms silicon reset run only in process context; normal `open()` keeps the previously measured ~30 ms path |

The remaining material throughput ceiling is architectural rather than an
unreviewed quick win: per-packet skb replacement/cache maintenance on RX and
non-coherent DMA/MMIO on TX. Replacing them with `page_pool`/`build_skb`,
descriptor shadows, or a different cache strategy would be a redesign and must
be A/B bench-gated on the RTL8196E. The earlier seemingly safe cache and
register changes produced large regressions or a dead interface, so this pass
does not recommend an unmeasured hot-path patch.

### Documentation corrections made by this pass

- The attack-surface inventory now says **three** module parameters, not four:
  `rtl8196e_rx_stall_thresh`, `link_poll_ms`, and
  `rtl8196e_cpu_port_mask`. `kick_threshold` is a sysfs attribute/exported
  variable, not a `module_param`.
- ETHDRV-003 is marked resolved in the registry below; its stale “open,
  accepted case D” label predated the v2.23 safe-default checksum policy.
- The old rejection of 64-bit stats is marked superseded: v2.23 implements
  shared UP-safe `u64_stats_sync` accumulators, not per-CPU counters.

---

## 2d. LAN LED functional validation (2026-07-30, driver 2.23→2.24)

The post-audit visual gate exercised STATUS and LAN independently through
BRIGHT, DIM and OFF on the Lidl board. STATUS passed. LAN read back `off` but
remained visibly DIM with `LEDCREG=0` and `DIRECTLCR=0`; keeping direct
topology with `DIRECTLCR=0` also retained a glow. The SDK definitions explain
the observation: topology zero is scan mode and DIRECTLCR is a duty-scale
register, not an output-disable register.

v2.24 claims the active-low `lan-led-gpios` pad, preloads its inactive GPIO
level, and changes only that pad's PIN_MUX_SEL_2 field: GPIO for OFF,
LED_PORTn for BRIGHT/DIM. The device tree supplies Lidl B6 and Sengled G4 B2,
so no board pad is hard-coded. The GPIO route was visually confirmed fully
dark on Lidl; both overlays build the same source and both board DTBs compile.
This is a slow-path sysfs/recovery change and does not touch RX/TX hot paths.

---

## 3. Kernel 6.18 simplification / optimization review

| ID | Effort | Gain | Proposal |
|----|--------|------|----------|
| ETH-S01 | medium | correctness of the resource model | `devm_platform_ioremap_resource` the DT `reg` window (today decorative) and route CPU-interface MMIO through `readl/writel` on it; SWCORE (0x1B800000) and the ASIC table window need two extra `reg` entries. Retires the hardcoded-KSEG1 class (F17 / ETH-005 / ETHDRV-006) |
| ETH-S02 | low | −~150 lines, −4 ethtool slots | retire (or gate under a debug Kconfig) the bring-up scaffolding: `dbg_timer`, `tx_debug_once`, `tx_dbg_*` stats, `dbg_irqs`, `rtl8196e_force_trap`. Resolves ETHDRV-011 by deletion |
| ETH-S03 | medium | `open()` drops from >1 s to ms (no longer carries the ETHDRV-007 fix — closed in v2.7) | hoist the one-time SoC bring-up (pinmux, switch-clock toggle with its 650 ms of sleeps, MEMCR, FULL_RST, L2 table clear) from `ndo_open` to probe; `open()` keeps only ring/VLAN/NETIF/L2-entry programming + start |
| ETH-S04 | trivial | hygiene | drop dead defines (`PIN_MUX_SEL`/`PIN_MUX_SEL2`, `TX_ALL_DONE_*`, `PortStatusNWayEnable`), fix the stale Kconfig help figures ("91/44 Mbps" → current 94/73), audit `depends on !RTL819X` (legacy symbol absent from the 6.18 tree) |
| ETH-S05 | low | TX latency under load | BQL (`netdev_sent_queue`/`netdev_completed_queue`) on the single TX queue — standard 6.x practice for soft-reclaim drivers; **bench-gated**, this platform has punished "obvious" wins before (F11/F13/F15) |
| ETH-S06 | trivial | API honesty | drop the never-set `rtl8196e_hw.base` member and decide the `(void)hw` question once (F16): either delete the parameter or keep the handle and remove the casts |
| ETH-S07 | trivial | call-site clarity | `rtl8196e_ring_tx_submit()` `flags` argument is constant at both call sites (`PKTHDR_USED | PKT_OUTGOING`) — fold into the ring layer |

**Implementation status (v2.13, 2026-06-12).** ETH-S05 implemented and
**kept after its bench gate**: BQL on the single TX queue —
`netdev_sent_queue` after a final submit, `netdev_completed_queue` at
the three reclaim sites (xmit-opportunistic, xmit-retry, NAPI),
`netdev_reset_queue` in open()/stop()/tx_timeout next to the ring
resets. Accounting is consistent by construction (both sides count
`skb->len`; the `tx_reclaim_no_skb` anomaly path routes to tx_timeout,
whose reset re-syncs BQL). Gate: RX 94.0 / P4 93.8 / P8 93.9 /
stress-300s 94.1 / TX 69.5 (in spread), UDP profile unchanged; BQL
limit observed converged (~2.5 KB) after load. This was the one
remaining performance idea — landed without the throughput cost the
F11/F13/F15 history warned about. The keep-it rationale (the
~15 ms-ring → ~0.2 ms latency win, the Zigbee/Thread control-traffic
argument, the qdisc-enabler point) and the 5-rep TX median A/B
quantifying the ≤1 % cost are in the archived performance record (`PERFORMANCE.md`, "History") (v2.13 entry); the
balanced-accounting + reset-pairing correctness rule is `DESIGN.md`
invariant 5 corollary.

**Implementation status (v2.12, 2026-06-12).** ETH-S01 implemented as a
*resource-claim variant*: the DT node declares the three windows
(`cpu-interface` 0x1000, `asic-table` 0x100000 — `type << 16`
selector, `switch-core` 0x8000) and probe claims + maps all three via
`devm_platform_ioremap_resource()`, **failing probe** if any returned
virtual base differs from the compile-time KSEG1 constant — the
constants become an enforced, executable invariant and `/proc/iomem`
is honest (three named windows visible on-target). The full
pointer-routing variant ("route MMIO through readl/writel on the
mapped base") is **rejected with rationale**: on MIPS, ioremap of these
low-512MB windows returns the very same KSEG1 alias, so routing would
only replace foldable `lui+lw/sw` pairs with unfoldable dependent
loads in the `__iram` hot paths (ISR/kick/NAPI) — the exact class of
"obvious" change this platform benched and rejected before
(F11/F13/F15, −47 Mbit/s). This retires the F17 / ETH-005 / ETHDRV-006
hardcoded-KSEG1 class: the addresses are no longer unverified
assumptions. Full iperf gate passed (the archived performance record (`PERFORMANCE.md`, "History"), v2.12 entry).

**Implementation status (v2.11, 2026-06-12).** ETH-S03 implemented:
`rtl8196e_hw_init()` moved from `ndo_open` to probe (between ring
creation and the ETHDRV-010 quiesce, so nothing FULL_RST latches
survives into `request_irq`). Bench: `open()` measured at ~30 ms
(was >1 s — the 650 ms of bring-up sleeps now run once at probe);
traffic and **nRST RSTACK verified after a down/up flap** — the 0x44
pad write moved to probe with the rest, which is correct because
nothing re-clears pad muxes at runtime since v2.7 (this write *was*
the historical clobberer). Full iperf gate passed (the archived performance record (`PERFORMANCE.md`, "History"),
v2.11 entry).

**Implementation status (v2.10, 2026-06-12).** ETH-S02, S04, S06 and S07
landed in one batch:

- *S02*: `dbg_timer` + `rtl8196e_dbg_timer_fn`, the `tx_debug_once`
  first-packet capture, the four `tx_dbg_*` ethtool slots, the ISR
  `dbg_irqs` prints and `rtl8196e_force_trap` are gone (−~190 lines
  net). The `rtl8196e_debug` module parameter lost its last consumer
  and is removed too (the ring-level `rx_debug_once`/`rx_debug_bad`
  one-shot prints are self-armed and stay). Six dbg-only ring
  accessors (`last_tx_submit`, `tx_count`, `tx_entry`, `rx_index`,
  `rx_pkthdr_entry`, `rx_mbuf_entry`) orphaned by the deletion are
  removed with it. The `rtl8196e_tx_kicks_*` ethtool stats are NOT
  debug scaffolding (tuning telemetry consumed by the bench script)
  and stay. ETHDRV-011 closed by deletion.
- *S04*: dead defines dropped (`PIN_MUX_SEL`/`PIN_MUX_SEL2`,
  `TX_ALL_DONE_IE_ALL`/`IP_ALL`, `PortStatusNWayEnable`); Kconfig help
  figures fixed to 94/73; `depends on !RTL819X` dropped (symbol does
  not exist in the 6.18 tree).
- *S06*: never-set `rtl8196e_hw.base` member dropped; the `(void)hw`
  casts removed and the `hw` parameter kept — ETH-S01 will route MMIO
  through the handle, so the API shape is the one S01 needs.
- *S07*: `tx_submit` no longer takes `flags`; the constant
  `PKTHDR_USED | PKT_OUTGOING` is owned by the ring layer.

Full iperf gate passed (the archived performance record (`PERFORMANCE.md`, "History"), v2.10 entry): RX 93.9 / P4
94.0 / P8 93.9 / stress-300s 94.1, TX 70.2 (in the documented
69.3–72.8 layout spread; v2.9 measured at the high edge, see the
variance note there). ethtool stats verified renumbered on-target.

### Considered and rejected

- **devm-converting probe** (`devm_request_irq` + auto-unwind): devm
  releases the IRQ *after* `remove()` has already `free_netdev`d the
  `dev_id`; the manual `free_irq`-before-`free_netdev` ordering is the
  correct one. Keep manual.
- **Per-CPU 64-bit stats (`ndo_get_stats64`)**: the original proposal was
  rejected as needless on an UP platform. **Superseded by v2.23**, which uses
  one shared `u64_stats_sync` accumulator (not per-CPU storage) so packet/byte
  totals do not inherit the 32-bit `ndev->stats` wrap.
- **phylib / fixed-link integration**: the four PHYs are internal to
  the switch ASIC and managed through PSRPx/MDIO vendor registers; the
  ethtool shim plus link timer/IRQ is smaller than a custom MDIO bus +
  phylib glue and loses nothing this hardware can express.
- **KSEG1 descriptor pools** (F6), **rearm `wback_inv → inv`** (F13),
  **WRAP-from-index** (F15), **`dma_cache_inv` removal** (F11): all
  hardware-tested and rejected — −1.2 to −47 Mbit/s; bench tables in
  the superseded log. Do not revisit bundled.
- **Gated RX checksum** (ETHDRV-003): the *earlier* gating variants
  regressed ≥13 Mbit/s and stayed case D through v2.17; **superseded in
  v2.23** by the safe-default policy whose isolated cost is ~2–4 %
  (§1.3, §2b F5) — the trade was reversed, not re-benched into the old
  hole.
- **AcptMaxLen / AcceptL2Err register writes** (§2b): both proposed as
  hardening, both bricked eth0 on `.88`, both **rejected** — the silicon
  reset defaults (1536 / bit 3 set) are load-bearing (DESIGN invariant
  12).
- **Scatter-gather TX**: rejected pre-6.18 (`feat/tx-throughput`
  archive branch, see `SPECIFICATIONS.md` §2).

---

## 4. Finding ID registry (complete)

First-pass audit 2026-04-23 (F-series, driver 2.2→2.3):

| ID | Status | One-liner |
|----|--------|-----------|
| F1 | fixed | tx_timeout now napi_disables around the ring reset |
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
| F16 | intentional | `(void)hw` vestigial API (now ETH-S06) |
| F17 | intentional | KSEG1 virtual addresses in HW DMA registers (now ETH-S01) |

Second pass 2026-05-01 (driver 2.3→2.4): ETH-001 (TX zero-padding,
fixed), ETH-002 (ring resets in stop(), fixed), ETH-003 (= ETHDRV-003),
ETH-004 (BUILD_BUG_ON ABI guards, fixed), ETH-005 (= F17), ETH-006 (DT
port-mask bounds, fixed), ETH-007 (debug params opt-in, intentional),
ETH-008 (= F13, stays rejected).

Third pass 2026-05-03 (driver 2.4→2.5): ETHDRV-001 (syscon
EPROBE_DEFER, fixed), ETHDRV-002 (canonical descriptor rearm, fixed),
**ETHDRV-003 (blanket CHECKSUM_UNNECESSARY — resolved in v2.23 by the
safe-default gated policy, §1.3)**, ETHDRV-004 (BE-bitfield compile guard, mitigated), ETHDRV-005
(0644 debug params, intentional), ETHDRV-006 (= F17).

Issue-#99 synthesis (driver 2.6, no audit pass): shadow-skb lookup by
hardware mbuf index + TX/RX pool-bounds validators + `ethtool -S`
anomaly counters.

This audit 2026-06-12 (driver 2.6, updated for 2.7..2.10): **ETHDRV-007**
(eth/GPIO mux clobber — **closed in v2.7**, gpio-line-names-derived
0x44 write; residual anonymous-claim exposure closed in v2.8 via
`realtek,led-pads`), **ETHDRV-008** (ring-array cache aliasing —
**closed in v2.9**), **ETHDRV-009** (stop() IRQ re-arm race — **closed
in v2.9**), **ETHDRV-010** (probe IRQ window — **closed in v2.9**),
**ETHDRV-011** (debug-timer wild deref — **closed in v2.10**, ETH-S02
deletion), **ETHDRV-012** (live MTU change — **closed in v2.9**, F2
pattern). The v2.9 batch is probe/teardown-only (zero hot-path edits);
v2.10 implements ETH-S02/S04/S06/S07. Both passed the full iperf gate,
figures in the archived performance record (`PERFORMANCE.md`, "History").

Post-audit #99 fix (driver 2.14, 2026-06-15, no formal audit pass):
**ETHDRV-013** (`tx_timeout` RX desync → `PKTHDR_DESC_RUNOUT` storm = the
issue-#99 soft-lockup — fixed) and **ETHDRV-014** (no-RX TX stall →
netdev watchdog — fixed by the software TX-reclaim timer). Both are
recovery/hot-path-adjacent fixes; the timer arm is `timer_pending()`-guarded
so steady-state TX is unchanged (the same-cycle perf follow-up restored TCP
TX to baseline). Validated on the bench, shipped as a candidate pending #99
field confirmation.

Independent re-audit 2026-07-17 (driver 2.18→2.23, §2b): a from-scratch
second audit merged with an external report. Implemented — atomic
`tx_timeout` (F1), cache-line pkthdr slots (F2), batch-end kick drain (F3),
safe-default RX checksum + `NETIF_F_RXCSUM` (F5, reverses ETHDRV-003 §1.3),
recovery hold-down FSM (N8), shared mask-first `napi_kick`, bounded
table-engine timeouts, element-aligned pool-bounds, `u64_stats_sync` /
`ndo_get_stats64`, `min_mtu = 576` (F6), `led_mode` replay (F8),
`Kconfig !SMP`. **Rejected on-target** — the AcptMaxLen and AcceptL2Err
register writes (bricked eth0; reset defaults kept, DESIGN invariant 12).
Panic instrumentation from v2.16/v2.17 (record v6→v7) remains,
diagnostic-only.

Post-audit functional validation 2026-07-30 (driver 2.23→2.24):
**ETHDRV-016** (`led_mode=off` was physically DIM — fixed by routing the
board-declared LAN LED pad to an inactive GPIO output). Lidl B6 was validated
by eye; Sengled G4 B2 uses the same DT-derived path.

Cross-references: **GPIO-007** (`drivers/gpio/AUDIT.md`) is the same
defect as ETHDRV-007 seen from the GPIO side; the GPIO audit explicitly
defers the fix to this driver — both close together in v2.7. `rtl8196e_regs.h` CSCR layout notes and
`reference_cscr_layout` derive from the ETHDRV-003 characterisation.

---

## 5. Conclusion

The driver is in its most hardened state since the rewrite: the entire
hardware-facing input surface (descriptors, lengths, pointers) is
validated with counters, the historical info-leak and quiesce races are
fixed and verified in-code, and the RX-checksum integrity trade
(ETHDRV-003) is now resolved integrity-safe (§1.3).

**Everything actionable from this audit is now implemented**
(2026-06-12, drivers v2.7 through v2.13, each step behind the full
iperf gate): ETHDRV-007 (v2.7/v2.8), ETHDRV-008/009/010/012 (v2.9),
ETH-S02/S04/S06/S07 + ETHDRV-011 (v2.10), ETH-S03 (v2.11, `open()`
>1 s → ~30 ms), ETH-S01 resource-claim variant (v2.12), ETH-S05 BQL
(v2.13, kept). The post-audit v2.14 then closed the root cause of issue
#99 — the `tx_timeout` RX-resync asymmetry (ETHDRV-013) and the no-RX TX
stall that fired it (ETHDRV-014); see §2. Remaining open items are
deliberate, evidence-backed deferrals: the considered-and-rejected list
above (every rejection carries its bench figures, including the two v2.22
register-write bricks) — ETHDRV-003 is no longer among them, resolved in
v2.23 (§1.3).

**Re-audit status (2026-07-17, driver 2.23).** A second independent audit
(§2b), merged with an external report, is fully implemented behind the
iperf gate: the recovery path is now atomic and self-limiting (worker +
hold-down FSM), the RX pkthdr geometry no longer false-shares, the RX
checksum trade is reversed to integrity-safe, and stats are 64-bit. The
two proposed switch-register writes were rejected after bricking eth0 on
the bench — their reset defaults are documented load-bearing (invariant
12). No open memory-safety item remains.

**Revalidation status (2026-07-30, driver 2.23).** The current 6.18 source
was re-read for security, recovery concurrency and hot-path complexity (§2c).
No code changed since the prior audit and no new security or performance
finding was identified. This is a static result, not a replacement for the
existing on-target fault injection and iperf evidence.

**Functional follow-up (2026-07-30, driver 2.24).** The LED visual gate found
and closed ETHDRV-016 (§2d). The v2.23 datapath remains unchanged; full OFF is
validated on Lidl, while Sengled G4 still requires a visual board check.
