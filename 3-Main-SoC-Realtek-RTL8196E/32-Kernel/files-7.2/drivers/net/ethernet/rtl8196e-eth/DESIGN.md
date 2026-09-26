# RTL8196E Ethernet driver (`rtl8196e-eth`) — design

| | |
|---|---|
| **Document date** | 2026-08-02 |
| **Driver version** | 2.24 (`RTL8196E_DRV_VERSION` in `rtl8196e_main.c`) |
| **Active release** | v4.6.0 (kernels `6.18.51` and `7.2.5`, `-rtl8196e-v4.6.0`); driver 2.24 adds true board-aware LAN LED off without changing the v2.23 datapath/recovery baseline |

Architecture reference for the from-scratch Ethernet driver. Findings
and audit history live in `AUDIT.md`; the goals/non-goals contract is
`SPECIFICATIONS.md`; per-release throughput is in `PERFORMANCE.md` and the
tuning rationale in its archived history (all in this directory).

---

## 1. What the hardware actually is

The RTL8196E "MAC" is a **5-port managed switch ASIC** with the CPU
attached as a sixth port (port 5). There is no conventional MAC+PHY
pair:

- Frames reach the CPU only if the switch's lookup decides so — via a
  static hashed **L2 "toCPU" entry** for the interface MAC, a broadcast
  entry, or the trap-all fallback. Programming the **VLAN**, **NETIF**
  and **L2** tables is therefore part of bringing the link up, not an
  offload feature.
- DMA is descriptor-list based but uses the vendor's BSD-style
  `rtl_pktHdr` / `rtl_mBuf` two-level format (20 B + 32 B, big-endian
  bitfields, ABI locked by `BUILD_BUG_ON` + `__BIG_ENDIAN_BITFIELD`
  `#error` in `rtl8196e_desc.h`).
- The four downstream PHYs are internal, managed through PSRPx status
  registers and a vendor MDIO window — there is no MDIO bus to expose
  to phylib.
- On the Lidl board a single physical port (4) is wired; the driver is
  deliberately single-netdev (see `SPECIFICATIONS.md` non-goals).

## 2. Module decomposition

```
 rtl8196e_main.c        net_device lifecycle, NAPI poll, ISR, xmit,
       │                ethtool, sysfs (led_mode, kick_threshold), timers
       │ calls
 ┌─────┴──────────────┬─────────────────────────┐
 │ rtl8196e_hw.c      │ rtl8196e_ring.c         │ rtl8196e_dt.c
 │ switch bring-up,   │ descriptor rings, SKB   │ &ethernet/interface@0
 │ pinmux (syscon),   │ lifecycle, cache        │ parsing + validation
 │ MDIO/PHY, VLAN/    │ discipline, TX kick     │ (vlan/ports/mtu bounds)
 │ NETIF/L2 tables,   │ coalescing, anomaly     │
 │ DMA reg setup, IRQ │ counters                │
 │ mask/unmask        │                         │
 └────────────────────┴─────────────────────────┘
 headers: rtl8196e_regs.h (register map + MMIO helpers),
          rtl8196e_desc.h (HW descriptor ABI), *_hw.h/*_ring.h/*_dt.h (APIs)
```

`rtl8196e_hw.c` is intentionally the only file that knows switch
register semantics; `rtl8196e_ring.c` is the only one that knows the
descriptor ABI and cache rules; `main.c` owns kernel-facing policy.

## 3. Memory & cache model (the heart of the driver)

Non-coherent write-back L1 — every DMA byte is managed by hand. Three
memory classes:

| Object | Address space | Coherency strategy |
|---|---|---|
| Ring arrays (u32 entries: descriptor ptr + OWNED/WRAP bits) | **KSEG1** (uncached alias of kmalloc) | CPU and ASIC both see DRAM directly; ownership flips are instantly visible (the initial cached-alias flush ETHDRV-008 required is done at alloc since v2.9) |
| Descriptor pools (`rtl_pktHdr`/`rtl_mBuf`) | **KSEG0** (cached) | per-descriptor `dma_cache_wback_inv` before handover, `dma_cache_inv` before reading HW-written fields (KSEG1 pools benched −1.2 Mbit/s — F6, rejected) |
| Packet buffers (skb data) | KSEG0 (cached) | TX: `dma_cache_wback_inv(data, len)` after padding; RX: `inv` before reading, full-span `wback_inv` of the fresh buffer before rearm |

Ownership protocol per entry: bit 0 = SWCORE-owned (1) / RISC-owned
(0), bit 1 = WRAP, remaining bits = descriptor pointer (the pools are at
least 4-byte-aligned so the low bits are free). Every handover is
`fields → wback → wmb() → ownership store (WRAP-preserving) → wmb()`.

**Pkthdr false-sharing — cache-line-strided slots (driver 2.22).**
`sizeof(rtl_pktHdr)` is 20 B but the L1 line is 32 B
(`CONFIG_MIPS_L1_CACHE_SHIFT=5`), so a naive 20-B-strided pool packs two RX
pkthdrs into one line. When the CPU rearms a descriptor (`ph_len`/`ph_flags`
write, then `dma_cache_wback_inv(ph, 20)`) it writes the *whole* line back to
DRAM — including bytes the ASIC may have just DMA-written into the adjacent
descriptor, silently zeroing a live neighbour's `ph_len` and dropping a valid
frame as `rx_bad_len`. The ownership protocol does not cover this: it is a
geometry defect, not a logic one. The pkthdr pool is therefore
`struct rtl8196e_pkthdr_slot { struct rtl_pktHdr ph; } __aligned(L1_CACHE_BYTES)`
— one descriptor per cache line, so a rearm can never touch a neighbour. Each
ring entry carries a *pointer* to its descriptor, so the padding is free on
the HW ABI; only the pool arithmetic and the `ptr_in_pool` stride change. The
mbuf pool is `L1_CACHE_BYTES`-aligned at allocation for the same reason. (TX
sharing was always benign — every mutable field is rewritten each submit.)

Addresses programmed into HW (ring bases, `m_data`) are KSEG0/KSEG1
*virtual* addresses — the ASIC ignores the top segment bits. Documented
as intentional (F17/ETHDRV-006; ETH-S01 tracks the ioremap cleanup).

## 4. Data paths

### TX (`start_xmit`, `__iram`)

1. linearize if needed; `skb_put_padto(ETH_ZLEN)` **before** any flush
   (info-leak guard, ETH-001);
2. opportunistic `tx_reclaim` (TX_ALL_DONE IRQ is deliberately never
   armed — software reclaim here, in NAPI, and via the TX-reclaim timer
   below replaces one IRQ per completion);
3. flush data, `tx_submit` (pool-bounds-validated descriptor, ≤1518 B),
   retry once after a reclaim, else stop queue / `NETDEV_TX_BUSY`;
4. `kick_tx`: pulse CPUICR.TXFD immediately on cold start (ring was
   empty), otherwise coalesce `kick_threshold` (default 4, sysfs
   tunable) submits per pulse — a pulse costs ~1.44 µs of MMIO. Two
   drains flush a sub-threshold remainder so nothing idles in the ring:
   `kick_drain` at the end of a dequeue batch (`netdev_xmit_more()`
   clear) and again in the NAPI poll. The batch-end drain is the
   load-bearing one on an **RX-silent** link — a coalesced descriptor
   left `SWCORE_OWNED` on an already-parked TX engine would otherwise
   have no puller (no ambient RX to wake NAPI), `tx_cons` would sit
   stuck, and the TX-done watchdog would eventually deep-reset the
   switch on a *phantom* "stuck TX". Draining at the `xmit_more`
   boundary bounds the exposure to the current burst while still
   coalescing within a batch under load;
5. queue stops at < 4 free slots, NAPI rewakes at ≥ 16 (hysteresis);
6. **BQL** (ETH-S05): `netdev_sent_queue(skb->len)` after the submit is
   final (a `NETDEV_TX_BUSY` return above hands the skb back
   unaccounted). The completion side is `netdev_completed_queue` at the
   three reclaim sites (opportunistic xmit, xmit-retry, NAPI); both
   sides count `skb->len`. BQL keeps the *in-driver* queue shallow so
   bulk traffic cannot bury latency-sensitive frames in the 128-deep
   ring — see the archived performance record (`PERFORMANCE.md`, "History") (the ring can hold ~15 ms of buffering;
   BQL's limit converges to ~2.5 KB ≈ ~0.2 ms under load). On this box
   that latency budget matters: the UART↔TCP bridge's EZSP/CPC/Spinel
   control frames share eth0 with any bulk transfer, and their protocol
   timers do not tolerate the jitter a deep FIFO injects. BQL is also
   the *enabler* for the qdisc — `pfifo_fast` today, a future
   `fq_codel` — which can only schedule packets still in the qdisc, not
   ones already dumped into the ring.

### TX-reclaim timer (the no-RX stall)

Both reclaim drivers above (xmit, NAPI) need *activity* to run: a fresh
xmit, or an RX-woken poll. A queue that stops (ring-full XOFF or BQL
byte-limit XOFF) while no RX is arriving — a purely-TX or low-RX workload
such as a border router idling with no paired peer — therefore has nothing
to reclaim its in-flight descriptors and would sit stopped until the 10 s
netdev watchdog fires `tx_timeout`. The **software TX-reclaim timer**
(`RTL8196E_TX_RECLAIM_MS`, 4 ms) closes that window: `start_xmit` and the
poll arm it whenever they leave the queue `netif_xmit_stopped`; the callback
`napi_schedule`s (reclaim + `netdev_completed_queue` + wake/un-freeze) and
the poll re-arms it while the queue stays stopped. Once the queue drains,
`netif_xmit_stopped()` is false and the timer lapses — bounded, never runs
on a moving queue. The arm is guarded by `timer_pending()` so the TX hot
path pays nothing once armed (an unconditional `mod_timer` per packet cost
~5 % TX on this CPU; see the archived performance record (`PERFORMANCE.md`, "History")).

### RX (`rtl8196e_ring_rx_poll` from NAPI, `__iram`)

For each RISC-owned pkthdr entry, up to budget: validate the pkthdr
pointer against the exact RX sub-pool and cache-line element stride
(fallback to index mapping), `inv` + read it, validate `ph_mbuf` against
the exact RX mbuf sub-pool and element stride, then look the shadow skb up **by
hardware mbuf index** — under saturation the ASIC links pkthdr[i] to a
mbuf of a different index, the v2.6 lesson — bound-check `ph_len`,
`inv` the data, hand the old skb to `napi_gro_receive`, install a fresh
`napi_alloc_skb` buffer, and rearm from a canonical descriptor state on
*every* exit path (nominal/drop/bad-len) so HW never sees stale fields.
All anomaly paths count in `ethtool -S` (`rx_wild_*`, `rx_bad_len`,
`rx_mbuf_no_shadow`, …) and must stay at zero in nominal flow.

Checksum offload is opt-in per frame: only characterized, unfragmented IPv4
UDP with both descriptor checksum bits set receives
`CHECKSUM_UNNECESSARY`. Every other protocol and encapsulation uses
`CHECKSUM_NONE`; the stack therefore validates uncharacterized traffic.

### Reading `rx_dropped`: the driver is not its only writer

The `drop` column of `/proc/net/dev` has **two** independent producers, and on a
gateway sitting on an ordinary LAN the second one dominates:

1. **The driver.** `ndo_get_stats64` publishes `ndev->stats.rx_dropped`, which is
   incremented on exactly three paths, all reaching the `rearm_drop` label of
   `rtl8196e_ring_rx_poll`: wild mbuf index, missing shadow skb, RX skb
   allocation failure. Each has its own `ethtool -S` counter —
   `rx_mbuf_no_shadow`, `rx_no_skb`, `rx_alloc_fail`. **If those three read zero
   the driver dropped nothing**, whatever the column says.
2. **The core.** `dev_get_stats()` adds `dev->rx_dropped` on top of whatever the
   driver returned, and `__netif_receive_skb_core()` bumps that atomic once per
   frame no registered `packet_type` matched
   (`SKB_DROP_REASON_UNHANDLED_PROTO`). `/proc/net/ptype` is the authoritative
   list of what the running kernel accepts; on this firmware it is three entries
   — `0800 ip_rcv`, `0806 arp_rcv`, `86dd ipv6_rcv`. `CONFIG_BRIDGE` and
   `CONFIG_LLC` are both out, so there is no 802.2/LLC handler either.

A double-digit percentage in `drop`, growing linearly, with `errs`/`fifo` at zero
and every diagnostic counter at zero, is therefore **not** a driver defect and
not a receive-path loss worth chasing.

Measured in the field on two gateways on unrelated LANs (2026-08-02): the whole
column was **RLDP, the Realtek Loop Detection Protocol** — ethertype `0x8899`,
subtype `0x23`, broadcast, zero source MAC, **one frame every 2.000 s**, so the
total tracks `uptime / 2` since boot.

The expansion is worth pinning down, because the acronym is badly overloaded: a
plain search for "RLDP" returns Ruijie's *Rapid Link Detection Protocol* and
Cisco's *Rogue Location Discovery Protocol* first, and neither has anything to do
with this frame. The authority for the reading here is tcpdump's own decoder,
`print-realtek.c` — "Format and print Realtek Remote Control Protocol (RRCP),
Realtek Loop Detection Protocol (RLDP), and Realtek Echo Protocol (REP) packets",
with `RTL_PROTOCOL_RLDP 0x03` and `RTL_PROTOCOL_RLDP2 0x23`. That is also what
prints the bare `RLDP` string in a capture, which on its own expands to nothing.

It is emitted by consumer Realtek switching gear, one emitter per segment. The
gateways are not the source: a
segment carrying **two** RTL8196E units showed a single 0.5/s stream with one
constant emitter-id field, where two emitters would give 1/s and two ids, and
nothing in this tree emits or enables RLDP (`EnRRCP2CPU`, MSCR bit 7, is a
bootloader `#define` that is never used).

What this is **not**: IP multicast. mDNS, ND and Matter traffic carry ethertype
`0800`/`86dd`, match `ip_rcv`/`ipv6_rcv` and are **delivered**; if they are then
discarded it happens at the IP layer and lands in `/proc/net/snmp`, never in
`rx_dropped`.

To settle it on any box, use `canari/ethercensus` (AF_PACKET ethertype census, no
promiscuous mode, writes nothing). Its own `ptype_all` registration suppresses
the `drop:` label while it runs, so `rx_dropped` freezing for exactly the capture
window and resuming afterwards is itself the proof that the unhandled-protocol
path was the producer.

### IRQ / NAPI

ISR (`__iram`): read `CPUIISR ∧ CPUIIMR`, W1C owned bits only,
`IRQ_NONE` otherwise; LINK_CHANGE updates carrier inline. RX_DONE /
RUNOUT and **both** timer kicks go through the shared
`rtl8196e_napi_kick()`: mask device IRQs **first**, then publish
`NAPI_STATE_SCHED`, so an RX hard IRQ arriving in the window cannot
re-enter the poll (its `napi_schedule_prep` sees SCHED set and the ISR
never leaves `CPUIIMR` unmasked) — `napi_complete_done()` is the single
site that unmasks. A kick against an active poll records `MISSED` while
leaving IRQs masked; a kick while the recovery-failed latch is set masks
and returns without scheduling. Poll: RX up to budget (delivered
packets/bytes aggregated once into the `u64_stats_sync` totals behind
`ndo_get_stats64`) → TX reclaim (+ `netdev_completed_queue` for BQL) →
kick drain → conditional queue wake → re-arm the TX-reclaim timer if the
queue is still stopped → on completion W1C runout bits and re-enable
IRQs. The probe sets `napi_defer_hard_irqs = 1` +
`gro_flush_timeout = 2 ms` **before** `netif_napi_add` (6.x copies them
at add time): batching the GRO flush is worth +33 % RX / +36 % TX on
this CPU (rationale block in probe).

## 5. Bring-up sequence

Since v2.11 (ETH-S03) the one-time SoC bring-up runs **once at probe**:
`hw_init` (pinmux + board 0x44 pad state via syscon, switch-clock cycle
with 650 ms of sleeps, MEMCR, FULL_RST, LED direct mode, queue mapping,
L2 clear), followed by the ETHDRV-010 IRQ quiesce.

`ndo_open` only programs the volatile per-open state — measured ~30 ms
on the bench (was >1 s): ring base registers → PHY reset + autoneg →
VLAN table + PVIDs → NETIF entry (MAC/VID/MTU) → L2 setup + toCPU entry
+ broadcast entry + readback verify → `hw_start` (TXCMD/RXCMD/TRXRDY) →
`napi_enable` → unmask IRQs → carrier from PSRPx. Table handshakes fail
fast and always restore SWTCR/TLU state. VLAN/NETIF failures abort open;
L2 entry failures retain the trap-to-CPU fallback.

The 0x44 pad write moved to probe with the rest: nothing re-clears pad
muxes at runtime since v2.7 (this write *was* the clobberer), so one
boot-time board-state write is the whole contract — re-validated on the
bench (nRST RSTACK after a down/up flap).

`stop()` quiesces in the reverse direction, reclaims completed TX after
stopping hardware, counts only unreclaimed SKBs as dropped, and **resets both
rings** (descriptors rebuilt, shadow SKBs reused) so the next `open()` — which
only reprograms base registers — starts from a canonical state.

**Recovery is worker-based and atomic-safe (driver 2.22).** `ndo_tx_timeout`
runs in a timer softirq under `dev->tx_global_lock` and **must not sleep** —
the earlier inline body called `napi_disable()`, which in 6.x takes the netdev
mutex and then `usleep_range()`s while `NAPI_STATE_SCHED` is set (kept set
across the `gro_flush_timeout` defer window), a sleep-in-atomic that fires
precisely under the load a TX timeout accompanies. The callback now only stops
the queue, logs the recovery fingerprint and `schedule_work(&swcore_reset_work)`.
The deep-reset worker (sleepable) reclaims and **resets both rings**, runs the
~650 ms switch-core reset (`rtl8196e_hw_swcore_reset` — the vendor
`reinitSwitchCore` depth, not just a pointer resync), reprograms VLAN/NETIF/L2,
replays the operator `led_mode` (the reset rewrote `LEDCREG`), and wakes the
queue. Both rings must reset together: `hw_stop()`/`hw_start()` rewinds the
switch RX engine (TRXRDY) to descriptor 0, so a TX-only recovery desyncs the RX
cursor and storms `PKTHDR_DESC_RUNOUT` — the #99 soft-lockup (AUDIT ETHDRV-013,
invariant 9). The poll-side RUNOUT detector uses the lighter
`rtl8196e_hw_ring_resync()` (ring pointers only, no silicon reset) from poll
context; the worker is the escalation when that shallow resync keeps failing or
the periodic TX-done watchdog finds the core wedged. All three triggers share
the one `swcore_reset_work`, so the workqueue serializes resets — only one runs
at a time.

If switch reprogramming fails, the worker publishes the `swcore_recovery_failed`
latch **before** re-enabling NAPI. The latch keeps carrier, queue and device
IRQs down across link polling, NAPI completion, `rtl8196e_napi_kick` and the
watchdog timers — nothing re-opens the interface on a half-programmed switch. A
dedicated timer schedules at most `RTL8196E_RESET_RETRY_MAX` (3) new worker
attempts with 1/2/4 s backoff; after that the interface is held down until an
administrative down/up starts a fresh attempt. Fault-injection on the bench
exercised both arms (transient fail → auto-recovers after 1 retry; persistent
fail → 3 retries then permanent hold-down, IRQs never re-opened, `ip link`
down/up recovers).

## 6. Concurrency model

- **UP SoC, three actors**: process context (open/stop/sysfs under
  RTNL), softirq (NAPI poll + timers), hardirq (ISR). No spinlocks in
  the fast path by design:
  - `start_xmit` runs with BH disabled → cannot interleave with NAPI on
    the single CPU; the TX ring is strict SP/SC with
    `READ_ONCE`/`WRITE_ONCE` on the indices.
  - `pending_kicks` races at worst into one extra TXFD pulse —
    idempotent on a non-empty ring.
  - ISR vs poll IRQ-mask handoff follows the standard
    `napi_schedule_prep → disable → __napi_schedule` pattern, factored
    into `rtl8196e_napi_kick()` (mask-first) and shared by the ISR and
    the two timer kicks.
  - `ndo_tx_timeout` runs in softirq under `tx_global_lock` and must not
    sleep: it only stops the queue and schedules `swcore_reset_work`; the
    sleeping recovery (napi_disable, ~650 ms switch reset) runs in the
    worker. The three recovery triggers (tx_timeout, poll-side RUNOUT
    escalation, TX-done watchdog) share the one work item, so the
    workqueue serializes them — at most one reset at a time.
  - The `swcore_recovery_failed` latch (`READ_ONCE`/`WRITE_ONCE`) is the
    one cross-actor recovery flag: set by the worker on a failed
    reprogram, consulted by every path that could re-open carrier or IRQs
    (napi_kick, poll completion, link/reclaim/retry timers) so none does
    on a half-programmed switch.
- Link state: hardirq LINK_CHANGE plus an optional poll timer
  (`link_poll_ms` DT property or module param, 0 = off) for setups
  where the latched IRQ proves unreliable.
- TX-reclaim timer: a softirq-context timer that only ever
  `napi_schedule`s — it touches no ring state itself, so it adds no new
  locking surface (the reclaim runs in the poll it wakes).
- Slow-path MMIO (MDIO, table engine, TLU) is process-context only,
  serialised by RTNL; the wait loops sleep (`usleep_range`).

## 7. External dependencies

| Dependency | Where | Role |
|---|---|---|
| `CONFIG_RTL8196E_ETH=y` | `config-6.18-realtek.txt`, `Kconfig` here | builds the four objects into `rtl8196e_eth.o` |
| `ethernet@10000` node + `interface@0` child | `rtl819x.dtsi` / `rtl8196e.dts` | IRQ 15 on `&intc`, `realtek,syscon` phandle, VLAN/ports/MTU config (`reg` window currently unused — ETH-S01) |
| sysc syscon regmap | `rtl819x.dtsi` | PIN_MUX_SEL/PIN_MUX_SEL_2 writes in `hw_init` — shared with `8250_rtl819x` and `gpio-rtl819x` (the former ETHDRV-007 / GPIO-007 contention is closed in v2.7: 0x44 fields derive from the GPIO node) |
| `gpio0` node `gpio-line-names` + `realtek,led-pads` | board DTS | decides each B2–B6 pad's 0x44 function — named → GPIO (`0b11`), in `led-pads` → LED_PORTn (`0b00`), neither → unclaimed GPIO/Hi-Z (`0b11`); `led-pads` absent → v2.7 fallback (unnamed → `0b00`) |
| Ethernet `lan-led-gpios` | board DTS | active-low GPIO descriptor for the same physical LAN LED pad: Lidl B6, Sengled G4 B2; preloads the inactive level and lets `led_mode=off` disconnect only that pad from the ASIC |
| `__iram` (`asm/mach-realtek/imem.h`) | arch overlay | hot functions (xmit, poll, ISR, ring ops) in 16 KB zero-wait I-SRAM |
| `dma_cache_*` (`asm/cacheflush.h`) | MIPS arch | the entire coherency model of §3 |
| S10network (userdata) | rootfs/userdata | persists the random MAC across boots (`ifconfig hw ether` at boot) |
| `scripts/test_rtl8196e_eth.sh` | `32-Kernel/scripts/` | the mandatory regression gate (~94 RX / ~73 TX Mbit/s, OTBR stopped) |

## 8. Invariants (do not break)

1. **Every perf-affecting change goes through the full iperf gate.**
   This platform has rejected four "obviously safe" optimisations on
   hardware evidence (F6, F11/F13/F15, gated csum, SG). >1 Mbit/s
   sustained delta or non-zero retrans = regression.
2. **The descriptor ABI is frozen** — sizes, offsets, big-endian
   bitfields, 4-byte pool alignment (ownership bits live in the low
   pointer bits). The compile-time guards must stay.
3. **Handover ordering** (`wback → wmb → ownership store → wmb`) and
   the canonical-rearm rule (every RX exit path rebuilds ph/mb fields)
   are load-bearing for the non-coherent cache model.
4. **`skb_put_padto` stays ahead of the data flush** — reordering
   reintroduces the ETH-001 slab leak on the wire.
5. **TX_ALL_DONE stays unarmed**; reclaim lives in xmit, NAPI, and the
   software TX-reclaim timer (the timer is what lets a no-RX stall recover
   without that IRQ — ETHDRV-014). Arming TX_ALL_DONE reintroduces one IRQ
   per completion on a 400 MHz core.
   **Corollary — BQL accounting must stay balanced**: every byte passed
   to `netdev_sent_queue` must later reach `netdev_completed_queue`, and
   every ring reset (`open`, `stop`, `tx_timeout`) must pair its
   `tx_reset` with `netdev_reset_queue`. A leaked count (sent without a
   matching completed, or a reset that drops in-flight bytes without
   resetting BQL) leaves BQL's limit stuck and the qdisc throttled — the
   TX queue wedges silently. The `tx_reclaim_no_skb` anomaly path is
   safe because it routes to `tx_timeout`, whose reset re-syncs BQL.
6. **GRO-defer fields are set before `netif_napi_add`** — setting them
   after is a silent no-op in 6.x.
7. **Shadow SKBs are indexed by hardware mbuf index, never by ring
   position** — position correspondence breaks under RX saturation
   (driver 2.6 fix; `rx_mbuf_no_shadow` counts violations).
8. **PIN_MUX_SEL bits [4:3] = 01, and 0x44 follows the GPIO node** —
   the UART1 mux value must be preserved exactly (v1.2 lesson). Since
   v2.8 the 0x44 fields are three-state from the GPIO controller node
   (named in `gpio-line-names` → `0b11` GPIO, listed in
   `realtek,led-pads` → `0b00` LED_PORTn, neither → `0b11` unclaimed
   GPIO/Hi-Z, `[17:15]` cleared; property absent → v2.7 fallback). The
   LED pad stays *declared*, never derived from `member-ports` — it is
   a wiring fact only a visual check proves (a member port may have no
   LED wired). Both known boards do follow the Table 36 1-1 naming
   (Lidl port 4 → B6/LED_PORT4, G4 port 0 → B2/LED_PORT0, verified by
   eye after the Lidl value first shipped wrong as B2 — register
   readback cannot catch a wrong LED pad, #126). Any future
   edit must keep the board — not the driver — as the owner of that
   decision. Since v2.24 `led_mode` is the one intentional runtime exception:
   it updates only the field selected by the matching board
   `lan-led-gpios`, routing that pad to GPIO for true OFF and back to
   LED_PORTn for BRIGHT/DIM. It must never rewrite the other B2–B6 fields.
9. **`tx_timeout` resets *both* rings, symmetric with `open`/`stop`** — it
   cycles the switch via `hw_stop`/`hw_start`, which rewinds the RX engine
   to descriptor 0; a TX-only recovery desyncs RX and triggers the #99
   `PKTHDR_DESC_RUNOUT` storm (AUDIT ETHDRV-013). Keep `ring_rx_reset` +
   `hw_set_rx_rings` in the recovery path alongside the TX reset.
10. **`ndo_tx_timeout` stays atomic** — it runs in softirq under
    `tx_global_lock` and must not sleep. It may only stop the queue and
    `schedule_work(&swcore_reset_work)`; all the sleeping recovery lives in
    the worker. Re-inlining `napi_disable()`/the switch reset reintroduces
    the sleep-in-atomic that fires under load (driver 2.22).
11. **The pkthdr pool is cache-line-strided** —
    `struct rtl8196e_pkthdr_slot { rtl_pktHdr ph; } __aligned(L1_CACHE_BYTES)`,
    one descriptor per 32-B line. A 20-B-packed pool false-shares adjacent RX
    descriptors and lets a rearm's `wback` clobber a live neighbour written by
    the ASIC (driver 2.22). `ptr_in_pool` must validate against the *slot*
    stride, not `sizeof(rtl_pktHdr)`.
12. **Two switch-register defaults are load-bearing — never program them.**
    `AcptMaxLen` (PCR [2:1]) stays at its 1536 reset default (the driver's PCR
    RMWs preserve it) — writing it explicitly wedged the port. `AcceptL2Err`
    (CSCR bit 3) stays *set*: the driver sets `EXCLUDE_CRC` so the CPU injects
    FCS-less TX frames, and bit 3 is what lets the CPU port accept them —
    clearing it killed TX. Both bricked eth0 on the bench (v2.22); `l2_setup`
    clears only CSCR bits 0–2, and `rtl8196e_regs.h` carries `AcptMaxLen_*`
    as reference-only defines (AUDIT §2b).
13. **RX checksum defaults to `CHECKSUM_NONE`** — `CHECKSUM_UNNECESSARY` is
    granted only to unfragmented IPv4 UDP with both `CSUM_IP_OK` and
    `CSUM_TCPUDP_OK` set; every other protocol/encapsulation is validated by
    the stack. The trade (a measured ~2–4 % RX under load) is deliberate — do
    not widen it back to a blanket assignment (AUDIT §1.3). `NETIF_F_RXCSUM`
    keeps it togglable via `ethtool -K`.
14. **The recovery hold-down latch never re-opens the interface.** On a failed
    reprogram the worker sets `swcore_recovery_failed` *before* re-enabling
    NAPI; carrier, queue and device IRQs stay down across every timer, NAPI
    completion and `napi_kick` until either a bounded retry succeeds or an
    admin down/up clears the latch. Any new path that could unmask IRQs or
    raise carrier must consult it first.
