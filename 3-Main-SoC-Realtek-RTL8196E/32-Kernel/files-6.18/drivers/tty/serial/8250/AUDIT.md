# 8250_rtl819x — cumulative driver audit

> **Cumulative audit ledger.** The current-state table and the finding
> registry (§6) are authoritative. §2–§4 keep the technical conclusions that
> the code comments and `DESIGN.md` rely on.

| Current state | Authoritative value |
|---|---|
| **Audited code** | driver v1.7 |
| **Last audit pass** | 2026-07-18 — A2, independent security/performance audit |
| **Last fully audited baseline** | v1.7 |
| **Post-baseline changes** | none |
| **Validation state** | both kernel lines built; hardware non-regression and LOOP-rig load/throttle stress passed (§5); wire-true RTS backpressure and a multi-day endurance campaign were not run (§5.4) |
| **Maintained kernels** | Linux 6.18 and 7.2, identical driver source |
| **Scope** | driver, 8250/serial-core contracts, Kconfig/Makefile, DT, IRQ and flow-control paths |
| **Companion** | `DESIGN.md` |

The driver is the **Zigbee-link UART glue**: it owns ttyS1, the wire to the
EFR32 radio. Everything on the radio path — uart-bridge, Z2M, cpcd,
otbr-agent, `flash_efr32.sh` — sits on top of it.

## 1. Audit-pass ledger

| Pass | Date | Baseline | Scope and outcome | IDs |
|---|---|---|---|---|
| — | early 2026 | pre-v1.2 | out-of-band audit, cancelled and replaced by A1; dispositions kept (§3, legacy findings) | 8250RTL-001…005 |
| A1 | 2026-06-12 | v1.2 → v1.4 | security review against the 6.18.35 sources; found that the FCR had never reached the hardware; S01…S04 implemented as v1.3, then the LOOP A/B bench found the RX-trigger erratum and v1.4 pinned trigger 1 | 8250RTL-006…009, S01…S04 |
| C1 | 2026-06-13…07-17 | v1.4 → v1.6 | v1.5: stuck RX-timeout IIR recovery in `handle_irq` (root cause of the multi-day field soft-lockups, see the code comment); v1.6: recovery silent by default, `phantom_count`/`phantom_log` observability | — |
| A2 | 2026-07-18 | v1.6 → v1.7 | independent security/performance audit against the 6.18.38 core sources, performed without consulting the in-tree markdown, then reconciled; all fixes implemented as v1.7; build, hardware and load/throttle validation (§5) | 8250RTL-010…017 |

Driver v1.7 shipped in firmware v4.0.0.

## 2. Security summary

The attack surface is small and local. The driver's only sysfs surface is two
module parameters (`phantom_count` 0444, `phantom_log` 0644, root-writable),
and it parses no other userspace input.

| Surface | Reachable by | Risk |
|---|---|---|
| termios on `/dev/ttyS1` (CRTSCTS → flow-control RMW) | root only (normally held by the uart-bridge) | RMW under `port->lock`; no state escapes the port |
| `TIOCMBIS/TIOCMBIC` → `set_mctrl` | root only | serial-core semantics since v1.7 (8250RTL-011) |
| DT properties (`realtek,syscon`, `clock-frequency`, `auto-flow-control`) | build-time | trusted input |

No network exposure, no DMA, no allocation after probe. A2 found no buffer
overflow, use-after-free, direct userspace pointer access or unbounded loop in
the interrupt path.

Verified correct (A1, still valid):

- **Flow-control locking (#89).** `->set_termios` is called by `serial_core.c`
  without the port lock, so the driver's `uart_port_lock_irqsave()` RMW is
  deadlock-free; the 8250 core's competing MCR writes run under the same lock.
- **Byte-lane model.** The core's MCR byte writes land in bits 31:24 of the
  32-bit word at +0x10 on this big-endian bus; `BIT(29)` of that word is
  `UART_MCR_AFE`. The driver's `readl`/`writel` RMW under the port lock
  composes correctly with the core's `writeb`.
- **N+1 divisor.** The hardware divides by N+1; `quot_frac` is correctly
  ignored (no fractional divisor on this SoC).

Properties preserved by v1.7: bounded IRQ path with no busy-wait; the
sysrq-aware serial-core port lock; MCR read-back under the port lock; the
phantom RX-timeout workaround performs a single dummy read only when IIR
reports a timeout with an empty FIFO; `devm_*`-managed allocations and
mappings; correct unwinding of clock-enable and registration failures; only
ttyS1 accepted.

## 3. Findings — legacy and A1

### Legacy findings (early-2026 audit)

- **8250RTL-001** (high, fixed) — syscon lookup failure must be fatal: the pin
  mux is mandatory. Lookup failure other than `-EPROBE_DEFER` fails probe.
- **8250RTL-002** (high, fixed) — ttyS1 is a platform contract: registration on
  any other line unregisters and fails with `-EBUSY`.
- **8250RTL-003** (medium, resolved) — AFE RMW vs core MCR writes: closed by the
  #89 fix (both sides under `port->lock`), the OE counters at 460800 and a read
  of `serial8250_do_set_mctrl()`.
- **8250RTL-004** (medium, superseded) — silent 200 MHz uartclk fallback,
  accepted at the time; removed by 8250RTL-015 in v1.7.
- **8250RTL-005** (low, fixed v1.3) — a tristate Kconfig allowed a module build
  that breaks the UART1/bridge init order; now `bool`,
  `depends on SERIAL_8250=y`.

### 8250RTL-006 — template `uart.fcr` is dead code (low, fixed v1.3)

`serial8250_register_8250_port()` copies membase, irq, uartclk, capabilities
and hooks from the caller's template, but **not `fcr`**. The driver's
`uart.fcr` assignment (FIFO + trigger 4, the #89-era headroom analysis) never
influenced the registered port in any kernel. S02 removed the assignment and
rewrote the comment.

### 8250RTL-007 — devm region request defeated `config_port`: FCR ran at 0 (medium, confirmed, fixed v1.3)

Since the switch to `devm_platform_get_and_ioremap_resource`, probe requested
the 0x18002100+0x100 region. `uart_add_one_port()` always calls
`->config_port()`, and `serial8250_config_port()` starts with a second,
conflicting `request_mem_region(mapbase, 32, "serial")`. It failed with
`-EBUSY` and returned early, skipping `register_dev_spec_attr_grp()` (no
`rx_trig_bytes` attribute) and `up->fcr = uart_config[PORT_16550A].fcr` — so
every `set_termios` wrote **FCR = 0x00**.

Confirmed on the bench gateway (2026-06-12, 6.18.35): `rx_trig_bytes` absent;
`/proc/iomem` showed only the devm claim for UART1 (UART0 showed the core's own
`serial` claim); IIR read `0x01000000`, bits 7:6 = `00`.

AFE still gated RTS, so the link stayed clean — see 8250RTL-009 for what that
mode actually was. S01 (plain `devm_ioremap`, the 8250-glue idiom; the core
owns the region claim) fixed it.

### 8250RTL-008 — `flow_active` started stale-true (low, fixed v1.3)

Probe pre-enabled flow control, but the first open carries a termios without
CRTSCTS; the edge-triggered check never ran the disable path, so the driver
believed AFE on while it was off. No practical consequence (the bridge enables
CRTSCTS moments later). S03: `set_termios` syncs on the absolute CRTSCTS state.
`flow_active` itself was retired in v1.7 (8250RTL-011).

### 8250RTL-009 — hardware erratum: RX trigger levels above 1 cause erratic overruns (medium, fixed v1.4)

Found by the LOOP-mode A/B bench after v1.3 (16550 LOOP mode through the
uart-bridge, 20 s sustained line-rate floods, flow control off, CPU ~90–100 %):

| RX trigger | 460800 flood | 892857 flood | IRQ/KB rx |
|---|---|---|---|
| 1 | **oe=0** | **oe=0** | 544–887 |
| 4 | — | oe=548 | 195 |
| 8 (16550A table default) | oe=3 | oe=53–83 | 117 |
| 14 | oe=2088 | oe=2 | 64–67 |
| v1.2 baseline (FCR=0) | oe=0 | oe=0 | 552–909 |

- The v1.2 "FIFO off" mode was never a 1-byte buffer: its IRQ rate and zero-OE
  behaviour match **trigger 1 with the 16-byte FIFO alive** — on this clone FCR
  bit 0 gates the trigger logic, not the buffer. That explains the clean v3.x
  radio track record, including the 892857-baud benches.
- Triggers above 1 overrun under sustained load, **non-monotonically** in both
  trigger and baud; no margin model predicts them. AFE does not help: its RTS
  threshold is hardwired near 14/16, leaving a ~2-byte margin (oe=53 with AFE
  on at 892857).

Fix (v1.4): `up->fcr` pinned to `ENABLE_FIFO | R_TRIG_00` — wire-identical to
the proven v3.x behaviour with the full 16-byte cushion, while keeping the v1.3
honest FCR path (`rx_trig_bytes` present and writable for experiments). Both
floods re-run on v1.4: oe=0, byte-perfect echo of 1.96 MB at 892857. Since
v1.7 the pin happens in the `startup` callback (8250RTL-010).

### A1 considered and rejected

- **`UPSTAT_AUTORTS`/`UPSTAT_AUTOCTS`** instead of the #109 `set_mctrl` guard:
  rejected at the time as aesthetic; **adopted in v1.7** once A2 found real
  gaps in the guard (8250RTL-011).
- **Pin mux through a pinctrl driver**: couples two drivers' probe order for no
  functional gain; the regmap poke of PIN_MUX_SEL bits 1/3/6 is idempotent and
  also defends against the historical Ethernet-driver clobber of bits[4:3].
- **Dropping the `quot > 1` divisor floor**: kept at the time; revised in v1.7
  (8250RTL-017).

## 4. Findings — A2 (all addressed in v1.7)

### 8250RTL-010 — port finalisation raced the ttyS1 registration (medium, fixed)

The driver poked `up->fcr` and re-ran the AFE enable with `port == NULL` after
`serial8250_register_8250_port()` — but the serial core publishes the TTY
before that call returns. On SMP/preemptible configurations that is a data race
on `up->fcr` and a lost-update window on the MCR; on this UP target it relied
on boot ordering. **Fix:** a `startup()` callback pins `up->fcr` (FIFO,
trigger 1) before `serial8250_do_startup()` programs the FCR, then arms AFE
under the real port lock; `shutdown()` disarms AFE in the 8250 shadow and the
hardware. Probe touches neither FCR nor AFE, so **MCR reads 0x00000000 until
the first open**.

### 8250RTL-011 — RTS/CTS state machine bypassed serial_core semantics (medium, fixed)

The #109-era `set_mctrl` guard forced RTS on while `flow_active` was set. It
protected against the software-throttle wedge but also overrode legitimate
requests: on `CRTSCTS` → `-CRTSCTS` physical RTS could stay asserted, and on
`B0` RTS was re-asserted while AFE was armed. **Fix:** guard and `flow_active`
removed. The driver advertises `UPSTAT_AUTOCTS | UPSTAT_AUTORTS` only after the
MCR read-back confirms the full pattern, so `uart_throttle()` calls the
driver's `throttle()` instead of `uart_clear_mctrl(TIOCM_RTS)`. `throttle()`
masks the RX interrupts so the FIFO fills and **hardware** AFE deasserts RTS
(the core's `skip_rx` logic supports this); `unthrottle()` re-enables
`UART_IER_RLSI | UART_IER_RDI`. This is the mainline `8250_omap` pattern. `B0`
now drops DTR and RTS (MCR `0x28000000`, verified on target).

### 8250RTL-012 — IRQ amplification with the 1-byte trigger (medium, accepted)

Trigger 1 costs ~1 IRQ per 1–2 RX bytes: theoretical ceilings of 23–46 k IRQ/s
at 460800 and 45–89 k IRQ/s at 892857. A peer that ignores RTS can burn a large
share of the CPU in hard IRQ. Kept because 8250RTL-009 makes every higher
trigger an overrun regression; changing it needs DMA or a hardware strategy.
The v1.7 throttle path helps under pressure when the peer honours RTS.

### 8250RTL-013 — logging from hard IRQ context (low, fixed)

With `phantom_log` enabled, a ratelimited message was emitted from the
interrupt handler, where a slow console can hold the CPU for tens of
milliseconds. **Fix:** the IRQ records the last IIR/LSR pair and schedules a
per-instance work item; the message is emitted in process context, with
`cancel_work_sync()` on the cleanup paths. The recorded pair can be torn
between two events (cosmetic; the counters are authoritative).

### 8250RTL-014 — non-atomic counters (low, fixed)

`phantom_count` was a plain `unsigned int`. **Fix:** global and per-device
counters are `atomic64_t` (read-only `module_param_cb` for the global),
`READ_ONCE`/`WRITE_ONCE` on the IRQ↔worker handoff. On this 32-bit CPU
`atomic64_t` uses the generic spinlock fallback, acceptable on a rare path.

### 8250RTL-015 — MMIO size unchecked, silent 200 MHz clock fallback (low, fixed)

**Fix:** probe fails unless the resource covers the MCR word at +0x10;
`resource_size()` goes to `port.mapsize`; no clock rate → probe fails. The
shipped DTs provide `clock-frequency = <200000000>` and a 0x100 window.

### 8250RTL-016 — pin mux not restored (low, fixed)

**Fix:** the initial PIN_MUX_SEL value is saved; the UART1 bits are restored on
every post-configuration error path and in `remove()`, with a warning if the
restore fails.

### 8250RTL-017 — divisor floor wrong for `quot == 1` (low, fixed, moot in practice)

The register must hold `quot - 1`; the old `quot > 1` floor made `quot == 1`
program 1 (half the requested baud). **Fix:** `quot ? quot - 1 : 0`. Reachable
only at ≥12.5 Mbaud with the 200 MHz clock, so never exercised on hardware.

## 5. Validation of v1.7 (2026-07-18)

### 5.1 Static and build

Full cross-builds of Linux 6.18.38 and 7.1.3 (GCC 15.2.0, Lexra); driver,
`vmlinux` and `vmlinuz` link cleanly; targeted `W=1` build without warnings;
`checkpatch.pl --no-tree --file` 0 errors / 0 warnings; `git diff --check`
clean. The 7.2 line carries the identical driver source.

### 5.2 Hardware non-regression (Lidl bench gateway, both kernel lines)

- **Boot and probe:** banner v1.7 / FIFO 16 / AFE on, `/dev/ttyS1` present,
  `rx_trig_bytes = 1` with the port open, no MCR/LSR/resource/pinmux error —
  pass on both lines.
- **Termios and MCR** (OTBR stopped, holder keeping the tty open):
  `460800 crtscts` → MCR `0x2B000000`; `-crtscts` → `0x0B000000`; `B0` →
  `0x28000000`; restore → full pattern; 500 open/configure/close cycles without
  hang — pass on both lines.
- **OTBR / EFR32** (OT-RCP 2.4.7 at 460800, hardware flow control): leader
  after restart, `ot-ctl rcp version` OK; 10-minute soak on 6.18 with constant
  state and MCR, zero OE/FE/BRK, `phantom_count = 0`, stable otbr-agent PID —
  pass.
- The 7.1 line's proc node is `/proc/tty/driver/serial_8250` (6.18: `serial`).
  The sengled-e39-g8c images were build-verified only.

### 5.3 Load and throttle stress (LOOP rig)

Same method as the June A/B bench (16550 LOOP mode + bridge TCP:8888,
byte-perfect echo harness), seven runs of 600 s:

- 460800 and 892857, hardware-flow and no-flow floods: **zero loss, zero
  mismatch, zero oe/fe**, 700–881 IRQ/KB (June reference 544–887), CPU ~98 %,
  no soft-lockup — pass.
- **691200 floods lose bytes at the tty layer** (both flow modes,
  reproducible). All loss is `bo` (flip-buffer insertion failure: the flip
  worker starves at ~90 % wire utilisation under ~97 % CPU), with the hardware
  FIFO clean (oe=0) and exact driver-level accounting. The same baud paced at
  80 % is clean. A same-day A/B showed the v1.6 driver reproduces the identical
  loss signature and magnitude, so this is a pre-existing platform envelope,
  not a v1.7 defect (the bridge `client_ops` path bypasses throttle). 460800
  and 892857 are unaffected.
- **Throttle path** (direct tty, no bridge, LOOP, crtscts): a stalled reader
  engages `uart_throttle` → the v1.7 callback; IRQ rate collapses 17× during
  the stall, both stall/drain cycles resume cleanly, MCR intact. Overruns
  during stalls are a loopback artifact: LOOP feeds CTS from the MCR RTS shadow
  bit, not the AFE-gated pin, so the AFE handshake cannot be tested in
  loopback.

### 5.4 Not run

- **Wire-true physical-RTS backpressure:** proving that the peer stops within
  the FIFO margin when AFE deasserts physical RTS needs an EFR32 load firmware
  or a probe on the RTS line. The LOOP rig cannot cover it.
- **Dedicated 48–72 h endurance campaign** with periodic sampling (uptime,
  OTBR state, MCR, serial/IRQ counters, `phantom_count`, watchdog and
  Spinel-timeout messages). The longest recorded run in this ledger is the
  10-minute OTBR soak of §5.2.

### 5.5 Residual risks

1. The 1-byte RX trigger keeps its IRQ cost (8250RTL-012), required by the
   8250RTL-009 erratum.
2. A peer that ignores RTS can still force overruns and heavy IRQ load.
3. The phantom-timeout dummy read carries the same theoretical race as the
   DesignWare workaround it mirrors (a byte arriving between the LSR read and
   the dummy read could be consumed) — negligible against the certain
   soft-lockup without it.
4. The `quot == 1 → 0` divisor change is untested (8250RTL-017).
5. `CONFIG_PM` is off: suspend/resume and frequency-change paths are out of
   scope.
6. Two edges inherited from the `8250_omap` idiom, accepted: `unthrottle()`
   restores the IER but not the `read_status_mask` DR bit cleared by
   `stop_rx()` (benign; resynced by the next `set_termios`); and dropping
   CRTSCTS while throttled leaves RX interrupts masked until the next CRTSCTS
   toggle or reopen (no known consumer does this).
7. The 691200 full-rate flood envelope of §5.3 (platform, not driver).

## 6. Finding ID registry

| ID | Severity | Status | Summary |
|---|---|---|---|
| 8250RTL-001 | high | fixed | probe continued without syscon; pin mux is mandatory |
| 8250RTL-002 | high | fixed | non-ttyS1 registration accepted despite platform contract |
| 8250RTL-003 | medium | resolved | AFE RMW vs core MCR writes — closed by #89 port-lock fix |
| 8250RTL-004 | medium | superseded | silent 200 MHz uartclk fallback — removed by 8250RTL-015 (v1.7) |
| 8250RTL-005 | low | fixed (v1.3) | tristate Kconfig vs built-in init-order contract — now `bool` |
| 8250RTL-006 | low | fixed (v1.3) | template `uart.fcr` never copied by the core — dead assignment removed |
| 8250RTL-007 | medium (confirmed) | fixed (v1.3) | devm region request → `config_port` early-return → FCR=0 |
| 8250RTL-008 | low | fixed (v1.3) | `flow_active` stale-true from probe — absolute-state sync |
| 8250RTL-009 | medium | fixed (v1.4) | hardware erratum: RX trigger >1 → erratic overruns; trigger pinned to 1 |
| 8250RTL-S01..S04 | — | implemented (v1.3) | S01 plain `devm_ioremap` (007); S02 dead `uart.fcr` removed (006); S03 absolute CRTSCTS sync (008); S04 Kconfig `bool` (005) |
| 8250RTL-010 | medium | fixed (v1.7) | post-registration fcr/AFE writes raced the published port — moved to startup/shutdown |
| 8250RTL-011 | medium | fixed (v1.7) | set_mctrl RTS guard broke B0/-CRTSCTS — replaced by UPSTAT_AUTORTS + throttle callbacks |
| 8250RTL-012 | medium | accepted | 1-byte trigger IRQ amplification; erratum-bound, DoS-resistant only while peer honours RTS |
| 8250RTL-013 | low | fixed (v1.7) | phantom_log printed from hard IRQ — deferred to a workqueue |
| 8250RTL-014 | low | fixed (v1.7) | non-atomic phantom counters — atomic64 + READ_ONCE/WRITE_ONCE |
| 8250RTL-015 | low | fixed (v1.7) | MMIO size/mapsize unchecked; silent 200 MHz clock fallback removed |
| 8250RTL-016 | low | fixed (v1.7) | pin mux not restored on probe failure / removal |
| 8250RTL-017 | low | fixed (v1.7) | `quot == 1` programmed divisor 2 instead of 1 — moot below 12.5 Mbaud |

## Conclusion

A1 made the FCR path real and pinned the RX trigger where the silicon erratum
demands it. A2 moved port arming into the core's startup/shutdown lifecycle,
expressed RTS ownership through `UPSTAT_AUTORTS`, restored hardware state on
every exit path and removed logging from hard IRQ context. Static checks and the
on-target non-regression battery (termios/MCR incl. B0, 500 open/close cycles,
10-minute OTBR soak) passed on both kernel lines, and the LOOP-rig
load/throttle stress passed, before v1.7 shipped in firmware v4.0.0. Wire-true RTS
backpressure and a dedicated multi-day endurance campaign were not run (§5.4).
