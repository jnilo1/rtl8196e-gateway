# 8250_rtl819x — cumulative driver audit

> **Cumulative audit ledger.** The current-state table and unified registry are
> authoritative. Part I and Part II retain their original pass-local evidence
> and terminology.

| Current state | Authoritative value |
|---|---|
| **Current implementation** | `8250_rtl819x` v1.7 |
| **Release target** | firmware v4.0.0 |
| **Last audit pass** | 2026-07-18 — A2, independent security/performance audit |
| **Last fully audited baseline** | v1.7 |
| **Post-baseline changes** | none |
| **Validation state** | both kernels built; Lidl and LOOP-rig hardware/load/throttle tests passed; listed variant checks remain pending |
| **Maintained kernels** | Linux 6.18 and 7.1, identical source/docs |
| **Current finding registry** | unified registry at the end of Part II |
| **Scope** | driver, 8250/serial-core contracts, Kconfig/Makefile, DT, IRQ and flow-control paths |
| **Companion** | `DESIGN.md` |

## Audit-pass ledger

| Pass | Date | Baseline | Result | Validation |
|---|---|---|---|---|
| A1 | 2026-06-12 | v1.2 → v1.4 | 8250RTL-006…009 and legacy findings reviewed/fixed | build and target UART checks |
| C1 | 2026-06-13…07-17 | v1.4 → v1.6 | stuck-IIR recovery and observability evolution | field/incident evidence |
| A2 | 2026-07-18 | v1.6 → v1.7 | 8250RTL-010…017 reviewed and fixes implemented | both-kernel build plus hardware/load/throttle tests |

This is the consolidated audit record for the driver. **Part I** is the
2026-06-12 audit, which cancelled and replaced the out-of-band early-2026
audit (findings 8250RTL-001..005 — dispositions preserved). **Part II**
is the 2026-07-18 security & performance audit, performed independently
(without consulting the in-tree markdown) against the code and the
6.18.38 core sources, and implemented as driver v1.7. The unified
finding registry for both parts is at the end.

Between the two audits the driver gained **v1.5** (stuck RX-timeout IIR
recovery in `handle_irq` — root cause of the multi-day field
soft-lockups, see the block comment in the code) and **v1.6** (recovery
silent by default, `phantom_count`/`phantom_log` observability). Both
fall inside Part II's scope.

The driver is the **Zigbee-link UART glue**: it owns ttyS1, the wire to
the EFR32 radio. Everything the radio path does — uart-bridge, Z2M,
cpcd, otbr-agent, `flash_efr32.sh` — sits on top of this driver.

---

# Part I — 2026-06-12 audit (v1.2 → v1.4)

## 1. Security review

### 1.1 Attack surface

Small and local. The driver exposes no sysfs of its own and parses no
userspace input; reachable surfaces are:

| Surface | Reachable by | Risk |
|---|---|---|
| termios on `/dev/ttyS1` (CRTSCTS toggling → flow-control RMW) | root only (tty is root-owned; normally held by the uart-bridge) | RMW is under `port->lock` (#89); no state escapes the port |
| `TIOCMBIS/TIOCMBIC` → `set_mctrl` | root only | guard only ORs a bit; cannot be abused to gain anything |
| DT properties (`realtek,syscon`, `clock-frequency`, `auto-flow-control`) | build-time | trusted input by definition |

No network exposure, no DMA, no unbounded loops, no allocation after
probe. There is no security flaw in this driver; the findings below are
correctness/effectiveness issues.

### 1.2 Verified correct (against the 6.18.35 tree)

- **The #89 locking fix is sound.** `->set_termios` is invoked by
  `serial_core.c` *without* the port lock held (the core takes its guard
  only after the call returns — `uart_change_line_settings`), so
  `rtl8196e_uart_enable/disable_flow_control()` taking
  `uart_port_lock_irqsave()` is correct and deadlock-free. The 8250
  core's competing MCR writes (`serial8250_do_set_mctrl`) all run under
  the same lock. The two probe-time calls with `port == NULL` were
  pre-registration / pre-exposure, where no concurrency exists
  *(revisited and tightened in Part II, 8250RTL-010)*.
- **The #109 `set_mctrl` guard matches the core it defends against.**
  6.18's `uart_throttle()` does exactly what the driver comment says:
  with CRTSCTS set but `UPSTAT_AUTORTS` not advertised, it falls through
  to `uart_clear_mctrl(port, TIOCM_RTS)` — and the 8250 core never sets
  `UPSTAT_AUTORTS` itself. Re-ORing `TIOCM_RTS` in `set_mctrl` closes
  every runtime path *(guard since replaced by the `UPSTAT_AUTORTS`
  route — Part II, 8250RTL-011)*.
- **The byte-lane model is right.** The core's MCR accesses are byte
  writes that land in bits 31:24 of the 32-bit word at +0x10 on this BE
  bus; `BIT(29)` of that word *is* `UART_MCR_AFE`. The driver's
  `readl`/`writel` RMW under the port lock composes correctly with the
  core's `writeb`.
- **The N+1 divisor compensation** programs `quot - 1` with a `quot > 1`
  floor; `quot == 1` is unreachable below 6.25 Mbaud at uartclk 200 MHz.
  `quot_frac` is correctly ignored (no fractional divisor on this SoC).
- **Probe hardening from the previous audit holds**: syscon lookup
  failure is fatal (8250RTL-001), registration on any line other than
  ttyS1 unwinds and fails with `-EBUSY` (8250RTL-002), the error path
  releases the clock.

### 1.3 Verdict

No security flaw. The substantive discovery of this audit is that the
driver's **FCR configuration had never reached the hardware**
(8250RTL-006/-007): the RX-trigger tuning written and reasoned about
during #89 was dead code, and since the F-08 refactor the port's FCR was
programmed to **0** — **confirmed on target**: the UART honours it and
ttyS1 ran in 16450 char mode, FIFO off, with AFE providing per-byte
RTS backpressure. The radio link's proven stability at 460800–892857
baud rested on that per-byte handshake, not on the FCR policy the code
claimed to set.

## 2. Findings

### 8250RTL-006 — template `uart.fcr` is dead code in every kernel version (low)

`serial8250_register_8250_port()` copies membase/irq/uartclk/
capabilities/hooks from the caller's template — **but not `fcr`**
(`grep fcr 8250_core.c` finds nothing). The carefully-commented
`uart.fcr = RTL8196E_UART_FCR` (FIFO + R_TRIG_01 = trigger 4, the #89-era
headroom analysis for 460800 baud) had therefore never influenced the
registered port, in any kernel this driver ever ran on. Absent the
template, the port's FCR comes from `uart_config[PORT_16550A]`
(trigger 8) via `serial8250_config_port()` — except that path was broken
too, see 8250RTL-007. The 40-line comment block documented a tuning that
was not in effect; that is a documentation hazard in its own right.

### 8250RTL-007 — devm region request defeats `config_port`: FCR runs at 0, FIFO confirmed OFF on target (medium, confirmed)

Since the F-08 refactor (`devm_platform_get_and_ioremap_resource`,
commit 747486c), probe **requested** the 0x18002100+0x100 mem region.
`uart_add_one_port()` → `uart_configure_port()` unconditionally calls
`->config_port()` (UPF_BOOT_AUTOCONF is force-set by the registration
path), and `serial8250_config_port()` *starts* with
`serial8250_request_std_resource()` — a second, conflicting
`request_mem_region(mapbase, 32, "serial")` against our now-busy devm
region. It failed with `-EBUSY` and `config_port` **returned early**,
silently skipping its last two statements:

1. `register_dev_spec_attr_grp(up)` — so
   `/sys/class/tty/ttyS1/rx_trig_bytes` did not exist (the documented
   runtime knob for exactly the trigger tuning #89 wanted);
2. `up->fcr = uart_config[PORT_16550A].fcr` — so `up->fcr` stayed **0**,
   and every `set_termios` wrote **FCR = 0x00** (FIFO-disable request)
   to the hardware (`serial8250_set_fcr()`).

**Confirmed on target 2026-06-12** (bench gateway,
`6.18.35-rtl8196e-v3.10.0`), all three fingerprints:

- `/sys/class/tty/ttyS1/rx_trig_bytes` — absent (early-return proven);
- `/proc/iomem` — UART0 showed the core's own `serial` claim
  (`18002000-180020ff : serial`, the ns16550a control case where
  `config_port` succeeded), UART1 showed only the devm claim
  (`18002100-180021ff : 18002100.serial serial@2100`), no nested
  `serial` entry;
- IIR (`devmem 0x18002108`) = `0x01000000` → IIR byte 0x01, bits 7:6 =
  `00` = **FIFOs disabled** (a 16550A in FIFO mode reads `11` there).
  The Realtek UART honours FCR bit 0: ttyS1 genuinely ran in 16450
  char mode.

Why the link was clean anyway: AFE (the #89/#109 machinery, in the MCR
and unaffected by this bug) still gated RTS — with FIFOs off it
throttles on "RBR full", i.e. after every single byte. That per-byte
hardware handshake made overruns impossible, at the price of one IRQ
per RX byte and an unused 16-byte FIFO on the most latency-sensitive
peripheral of the box. Every v3.x radio-path validation, including the
892857-baud benches, actually ran in this mode. The trigger-4 headroom
reasoned about in #89 never existed at any point.

Side effect for completeness: the never-taken unregister path would log
a bogus release warning (it releases a 32-byte "serial" region that was
never granted) — harmless, the port never unbinds.

Fixed by S01 (drop the region request; the `8250_dw`/glue pattern is
plain `devm_ioremap`), radio-soak gated.

### 8250RTL-008 — `flow_active` starts stale-true; guard semantics drift until first CRTSCTS transition (low)

Probe (DT has `auto-flow-control`) called `enable_flow_control(NULL)`,
which set `flow_active = true`. The first open of ttyS1 carries the
default termios (no CRTSCTS): the core cleared AFE in hardware (via
`up->mcr`/`serial8250_set_afe`), but the driver's transition-only check
(`crtscts_new == crtscts_old → return`) never ran the disable path —
so `flow_active` stayed true while AFE was actually off, and the #109
guard forced RTS asserted in a window where, per its own contract, "HW
flow control owns the line" was false. Consequence: none worth a
release (RTS-asserted is the friendly idle state, and the bridge enables
CRTSCTS moments later, resyncing everything). Fixed by S03:
`set_termios` syncs on the absolute CRTSCTS state instead of
edge-detecting. *(The `flow_active` variable itself was retired in
v1.7 — Part II, 8250RTL-011.)*

### 8250RTL-009 — hardware erratum: RX trigger levels above 1 cause erratic overruns (medium, fixed v1.4)

Found by the loopback A/B bench that followed the v1.3 implementation
(2026-06-12, 16550 LOOP-mode stress through the uart-bridge, 20 s
sustained line-rate floods, flow control off, CPU ~90-100 % busy):

| RX trigger | 460800 flood | 892857 flood | IRQ/KB rx |
|---|---|---|---|
| 1 | **oe=0** | **oe=0** | 544–887 |
| 4 | — | oe=548 | 195 |
| 8 (16550A table default) | oe=3 | oe=53–83 | 117 |
| 14 | oe=2088 | oe=2 | 64–67 |
| v1.2 baseline (FCR=0) | oe=0 | oe=0 | 552–909 |

Two facts fall out. First, the v1.2 "FIFO off" mode was never a 1-byte
buffer: its IRQ rate (~1–2 bytes per IRQ at line rate) and its zero-OE
behaviour match *trigger 1 with the 16-byte FIFO alive* — on this clone
FCR bit 0 gates the trigger logic, not the buffer. That retroactively
explains the entire clean v3.x track record. Second, triggers above 1
overrun under sustained load and **non-monotonically in the trigger
value and the baud** (548 at trig4/892857 vs 53 at trig8; 2088 at
trig14/460800 vs 2 at trig14/892857): the clone's trigger/timeout
semantics are not trustworthy, and no margin model predicts them. AFE
does not save the day either — its RTS threshold is hardwired near
14/16 (as the #89-era comment already suspected), leaving a ~2-byte
margin (oe=53 with AFE on at 892857).

Fix (v1.4): pin `up->fcr` to `ENABLE_FIFO | R_TRIG_00` — wire-identical
to the proven v3.x behaviour, full 16-byte cushion, with the v1.3
correctness work (honest FCR path, `rx_trig_bytes` present — and still
writable for experiments) retained. The #89 trigger-4 idea is hereby
invalidated by hardware, not just dead-coded. Validated: both floods
re-run on v1.4 → oe=0, byte-perfect echo of 1.96 MB at 892857.
*(The pinning site moved from post-registration to the `startup`
callback in v1.7 — Part II, 8250RTL-010.)*

### Legacy findings — status after re-verification

- **8250RTL-001** (syscon must be fatal) — **fixed**; in code, with the
  audit ID cited in the comment. Verified: lookup failure other than
  `-EPROBE_DEFER` fails probe.
- **8250RTL-002** (ttyS1 is a platform contract) — **fixed**; `ret != 1`
  unregisters and fails with `-EBUSY`.
- **8250RTL-003** (AFE RMW vs core MCR writes, "hypothesis to test") —
  **resolved** by the #89 fix: both sides now run under `port->lock`,
  and the field evidence (OE counters at 460800) plus this audit's
  read of `serial8250_do_set_mctrl()` close the hypothesis.
- **8250RTL-004** (silent 200 MHz fallback) — **accepted/mitigated** at
  the time *(hard fallback removed in v1.7 — Part II, 8250RTL-015)*.
- **8250RTL-005** (tristate Kconfig allows a module build that breaks
  UART1/bridge init order) — fixed in v1.3 (`bool`,
  `depends on SERIAL_8250=y`).

## 3. Simplification / 6.18 alignment

API level is current: `devm_*` probe, void `remove`, `uart_port_lock_*`
wrappers (this tree even carries the upstream-submitted SysRq lock
variants), `of_property_read_bool`, syscon/regmap for the pin mux. No
deprecated calls. Candidates:

| ID | Change | Value |
|---|---|---|
| 8250RTL-S01 | replace `devm_platform_get_and_ioremap_resource` with `platform_get_resource` + `devm_ioremap` (the 8250-glue idiom — the 8250 core owns the region claim) | Closes 007: `config_port` completes, FCR becomes the 16550A default (FIFO + trigger 8), `rx_trig_bytes` appears |
| 8250RTL-S02 | delete the dead `uart.fcr` assignment + rewrite the trigger-4 comment to match reality | Closes 006; stops the code lying to its reader |
| 8250RTL-S03 | `set_termios` absolute-state instead of edge-triggered | Closes 008 |
| 8250RTL-S04 | Kconfig `tristate` → `bool` (+ `depends on SERIAL_8250=y`) | Closes legacy 005 |

All four were implemented as driver v1.3 the same day and bench-verified
on the .88 gateway (probe banner, `rx_trig_bytes` present, `/proc/iomem`
shows the core's own 32-byte `serial` claim on UART1,
IIR = `0xC1000000` — FIFOs enabled, first FIFO-mode operation since
F-08, MCR = `0x2B000000`, `nrst_pulse` RX smoke clean). The A/B bench
that followed produced 8250RTL-009 and v1.4 (trigger pinned back to 1);
the loopback floods exceed any real radio workload, so the
sustained-soak gate was considered covered for the RX path.

### Considered and rejected (this audit)

- **Advertising `UPSTAT_AUTORTS`/`UPSTAT_AUTOCTS`** instead of the #109
  `set_mctrl` guard. It is the mainline-idiomatic way to tell
  `uart_throttle()` to keep its hands off RTS, and would let the guard
  be deleted. Rejected at the time: the guard was field-proven across
  months of OTBR uptime and the win looked aesthetic. **Superseded:**
  Part II (8250RTL-011) identified real correctness gaps in the guard
  (B0, `-CRTSCTS` transitions) and v1.7 adopts exactly this route,
  with throttle/unthrottle callbacks completing the semantics.
- **Doing the pin mux in pinctrl instead of a syscon poke.** The
  pinmux write (bits 1/3/6 of 0x40) predates any pinctrl driver and is
  duplicated as a defensive measure against the historical
  ethernet-driver clobber of bits[4:3]. Moving it to a pinctrl driver
  would be cleaner but couples two drivers' probe order for zero
  functional gain; the regmap poke is idempotent and self-contained.
- **Dropping the `quot > 1` floor** as unreachable — kept at the time
  as a guard against a zero divisor *(revised in v1.7: zero is the
  correct N+1 encoding for `quot == 1` — Part II, 8250RTL-017)*.

---

# Part II — 2026-07-18 security & performance audit (v1.6 → v1.7)

## 4. Scope and method

Independent review of the driver's security, robustness and
performance, deliberately performed **without consulting the in-tree
markdown** (this file included), then reconciled. Covered:

- `probe`, `startup`, `shutdown` and `remove` paths;
- MCR programming and Automatic Flow Control (AFE);
- interactions between `set_termios`, RTS/CTS and the serial-core
  throttle machinery;
- interrupt handling and the phantom RX-timeout recovery;
- FIFO configuration and the baud divisor;
- MMIO resource, pin mux and clock management;
- the corresponding 8250-core and `serial_core` implementations in
  Linux 6.18.38 (every claim verified by reading those sources);
- the platform kernel configuration and Device Tree.

## 5. Summary

The review found no buffer overflow, no use-after-free, no direct
userspace pointer access and no unbounded loop in the interrupt path.
The main risks were: port finalisation racing the ttyS1 registration,
an RTS/AFE scheme that bypassed `serial_core` semantics, the IRQ
amplification inherent to the 1-byte RX trigger, hardware state not
restored on probe failure or removal, optional logging from hard IRQ
context, and several hardening gaps (counters, MMIO size, clock
fallback, divisor floor). All except the trigger-related IRQ cost were
fixed in v1.7 (findings below); the trigger stays per the 8250RTL-009
erratum.

## 6. Findings and fixes (all implemented in v1.7)

### 8250RTL-010 — port finalisation raced the ttyS1 registration (medium, fixed)

The driver called `serial8250_register_8250_port()`, then poked
`up->fcr` and re-ran the AFE enable with `port == NULL`. But the serial
core publishes the TTY (and serdev) *before*
`serial8250_register_8250_port()` returns: a consumer could open the
port while those post-registration writes were still in flight. On an
SMP or preemptible configuration that is a data race on `up->fcr` and a
lost-update window on the MCR (8-bit core writes vs 32-bit driver RMW);
even on this UP/non-preempt platform it relied on boot ordering rather
than a guarantee. Possible consequences: default trigger-8 FIFO in use
(8250RTL-009 territory), stale AFE, an EFR32 link wedge.

**Fix:** `startup()` callback pins `up->fcr` (FIFO, trigger 1)
*before* `serial8250_do_startup()` programs the FCR, then arms AFE with
the real `uart_port` under `port->lock`; symmetric `shutdown()`
disarms AFE in the 8250 shadow and the hardware. Every
`port == NULL` flow-control call is gone; probe no longer touches AFE
or FCR at all. Side effect worth knowing: **MCR reads 0x00000000 until
the first open** (the probe-time pre-enable is deleted).

### 8250RTL-011 — RTS/CTS state machine bypassed serial_core semantics (medium, fixed)

The #109-era `set_mctrl` guard forced RTS on whenever `flow_active`
was set. That protected the link from the software-throttle wedge, but
also intercepted legitimate requests:

- on a `CRTSCTS` → `-CRTSCTS` transition,
  `serial8250_do_set_termios()` calls `set_mctrl` before the driver
  resyncs, so physical RTS could stay asserted while `port->mctrl`
  said otherwise;
- on `B0`, `serial_core` asks for DTR *and* RTS to drop — the guard
  re-asserted RTS while AFE was still armed.

**Fix:** the guard (and `flow_active`) are gone. The driver now
advertises `UPSTAT_AUTOCTS | UPSTAT_AUTORTS` — set only after the MCR
read-back confirms the full pattern — which makes 6.18's
`uart_throttle()` call the driver's `throttle()` callback *instead of*
`uart_clear_mctrl(TIOCM_RTS)` (`serial_core.c`); the wedge path is
closed structurally. `throttle()` stops the RX interrupts so the FIFO
fills and **hardware** AFE deasserts RTS (the core's `skip_rx` logic in
`serial8250_handle_irq` explicitly supports this: with the UPSTAT bits
set and RDI/RLSI masked it refuses to drain the RX FIFO);
`unthrottle()` re-enables `UART_IER_RLSI | UART_IER_RDI`. This is the
mainline `8250_omap` pattern. `B0` now genuinely drops DTR and RTS
(byte-lane write clears bits 24/25, AFE bit 29 untouched — verified on
target, MCR `0x28000000`).

### 8250RTL-012 — IRQ amplification with the 1-byte trigger (medium, accepted)

The 1-byte RX trigger costs ~1 IRQ per 1–2 RX bytes: theoretical ceilings
of 23–46 k IRQ/s at 460800 and 45–89 k IRQ/s at 892857 on a 200 MHz
CPU. A misbehaving or hostile peer that ignores RTS can burn a large
share of the CPU in hard IRQ. **Decision: keep trigger 1** — the
8250RTL-009 erratum makes every higher trigger an overrun regression;
changing this needs a DMA or hardware strategy, not a knob. The new
throttle path improves behaviour under pressure (RX IRQs off, AFE
backpressure) *when the peer honours RTS*. Longer-term options: DMA
support if the block allows it, abnormal-IRQ-rate detection,
protocol-level rate limiting in the EFR32 firmware.

### 8250RTL-013 — logging from hard IRQ context (low, fixed)

With `phantom_log` enabled, `dev_info_ratelimited()` ran inside the interrupt
handler; on a slow legacy console a single record write can hold the CPU
for tens of milliseconds with interrupts off (the exact iatrogenic
failure the v1.6 change was about). **Fix:** the hard IRQ only records
the last IIR/LSR pair and schedules a per-instance `work_struct`; the
message is formatted and emitted in process context.
`cancel_work_sync()` added to the cleanup paths. (The recorded IIR/LSR
pair can be torn between two events — cosmetic; the counters are
authoritative.)

### 8250RTL-014 — non-atomic counters (low, fixed)

`phantom_count` was a plain global `unsigned int`: unsynchronised sysfs
reads, wrap in ~49 days at storm rates, and no multi-instance safety.
**Fix:** global and per-device counters are `atomic64_t` (read-only
`module_param_cb` for the global), `READ_ONCE`/`WRITE_ONCE` on the
IRQ↔worker handoff. On this 32-bit Lexra CPU `atomic64_t` goes through
the generic spinlock fallback — acceptable, the path is rare by
definition.

### 8250RTL-015 — MMIO size unchecked, silent 200 MHz clock fallback (low, fixed)

Probe assumed the MMIO resource covered the MCR at +0x10 without
checking, never set `port.mapsize`, and fell back silently to
200 MHz when neither DT nor clock framework provided a rate — on a
different board variant that would mean a wrong baud with a successful
probe. **Fix:** probe fails unless the resource covers the MCR word;
`resource_size()` is propagated to `uart.port.mapsize`; no clock rate →
probe fails explicitly. The shipped DTs provide
`clock-frequency = <200000000>` and a 0x100 window on both kernel
lines, so nominal behaviour is unchanged.

### 8250RTL-016 — pin mux not restored (low, fixed)

The UART1 pinmux bits were set during probe and left set on later probe
failure or driver removal. **Fix:** the initial PIN_MUX_SEL value is
saved; the UART1 bits are restored on every post-configuration error
path and in `remove()`, with a warning if the restore itself fails.

### 8250RTL-017 — divisor floor wrong for `quot == 1` (low, fixed, moot in practice)

The N+1 rule means the register should hold `quot - 1` always; the old
`quot > 1` floor made `quot == 1` program 1 (effective divisor 2, half
the requested baud). **Fix:** `quot ? quot - 1 : 0`. Reachable only at
≥12.5 Mbaud with the 200 MHz clock — far beyond the wire — so this is a
correctness nicety; it has not been (and realistically cannot be)
exercised on hardware.

## 7. Security properties preserved

- the IRQ path stays bounded, no busy-waiting added;
- the sysrq-aware port lock of the serial core is kept;
- the MCR read-back stays under the port lock;
- the phantom RX-timeout workaround still performs a single dummy read
  only when IIR reports a timeout with an empty FIFO;
- allocations and mappings remain `devm_*`-managed;
- clock-enable and registration failures unwind correctly;
- only ttyS1 is accepted, per the platform contract.

## 8. Validation

### 8.1 Static / build (2026-07-18)

1. full cross-build of Linux 6.18.38 and 7.1.3 (GCC 15.2.0, Lexra);
2. `8250_rtl819x.o`, `vmlinux`, `vmlinuz` build and link cleanly;
3. targeted `W=1` build of the driver: no warnings;
4. `checkpatch.pl --no-tree --file`: 0 errors, 0 warnings;
5. `git diff --check`: clean;
6. overlay and expanded build trees carry the same driver (both lines).

### 8.2 Hardware non-regression (2026-07-18, lidl gateway .88)

Executed same-day on both kernel lines; full log in the maintainer's
bench archive (`2026-07-18_uart_v1.7_nonreg_bench.md`).

- **Boot & probe** (6.18 build #4, then 7.1.3 build #2): probe banner
  v1.7 / FIFO 16 / AFE on, `/dev/ttyS1` present, `rx_trig_bytes = 1`
  with the port open, no `MCR pattern incomplete` / `LSR safety` /
  `resource busy` / pinmux error, network and SSH immediately up —
  **pass** on both lines.
- **Termios & MCR** (OTBR stack stopped, holder keeping the tty open):
  `460800 crtscts` → MCR `0x2B000000`; `-crtscts` → `0x0B000000` (AFE
  alone cleared); re-enable → full pattern; `B0` → `0x28000000` (DTR
  and RTS physically dropped under AFE — the v1.7 semantics); restore →
  full pattern; **500 open/configure/close cycles** without hang, tty
  alive, dmesg clean — **all-pass** on both lines.
- **OTBR / EFR32** (OT-RCP 2.4.7 @ 460800 hw-flow): leader immediately
  after restart, Spinel round-trip OK (`ot-ctl rcp version`); 10-minute
  soak on 6.18: state and MCR constant, serial counters progressing,
  zero OE/FE/BRK, `phantom_count = 0`, otbr-agent PID stable (no
  supervisor restart) — **pass**.
- Operational notes: with v1.7 the MCR stays `0x00000000` until the
  first open (expected — probe pre-enable removed); the 7.1 line's proc
  node is `/proc/tty/driver/serial_8250` (6.18: `serial`). The
  sengled-e39-g8c images are build-verified only (no G4 hardware on
  this bench).

### 8.3 Load & throttle stress (2026-07-18, LOOP rig)

Executed same-day with the June A/B method (16550 LOOP mode + bridge
TCP:8888, byte-perfect echo harness; raw data in the maintainer's
`uart_64_load_20260718/`). Seven runs, 600 s each unless noted:

- 460800 and 892857, hw-flow and no-flow floods: **zero loss, zero
  mismatch, zero oe/fe**, 700–881 IRQ/KB (June reference 544–887),
  CPU ~98 % busy, no soft-lockup — **pass** on every §6.4 criterion.
- **691200 floods lose bytes at the tty layer** (both flow modes,
  reproducible): all loss is `bo` (flip-buffer insertion failure —
  worker starvation at ~90 % wire utilisation under ~97 % CPU), with
  the hardware FIFO clean (oe=0) and exact driver-level accounting.
  Same baud paced at 80 %: clean. Not a driver defect and not v1.7
  code (the bridge client_ops path bypasses throttle entirely);
  691200 flood had simply never been benched. Production ladder
  ≤460800 and 892857 unaffected.
- **Throttle path exercised** (direct tty, no bridge, LOOP,
  crtscts): writer flood with a stalled reader engages
  `uart_throttle` → the v1.7 callback — IRQ rate collapses 17×
  during stall, both stall/drain cycles resume cleanly, no wedge,
  MCR intact. The oe seen during stalls are a **loopback artifact**:
  LOOP feeds CTS from the MCR RTS shadow bit, not from the AFE-gated
  physical pin, so the transmitter is never gated — the AFE
  handshake is untestable in loopback by construction.

### 8.4 Pending

- **Wire-true physical-RTS backpressure**: only an EFR32 load
  firmware (or a probe on the RTS line) can prove the peer stops
  within the FIFO margin when AFE deasserts physical RTS. This is
  the sole §6.4 item the LOOP rig cannot cover.
- **Endurance**: 48–72 h OTBR campaign, 5-minute sampling (uptime,
  OTBR state, MCR, serial/IRQ counters, `phantom_count`, watchdog and
  Spinel-timeout messages). Acceptance: no reboot, no OTBR wedge, no
  serial error, no load drift.

## 9. Residual risks

1. The 1-byte RX trigger keeps its IRQ cost (8250RTL-012); required by
   the 8250RTL-009 erratum.
2. A peer that ignores RTS can still force overruns and heavy IRQ load.
3. The phantom-timeout dummy read carries the same theoretical race as
   the DesignWare workaround it mirrors (a byte arriving between the
   LSR read and the dummy read could be consumed): negligible against
   the certain soft-lockup without it.
4. The `quot == 1 → 0` divisor change is untestable below 12.5 Mbaud
   (8250RTL-017) — flagged for a logic-analyzer pass if ultra-high
   baud experiments ever happen.
5. `CONFIG_PM` is off on this platform: suspend/resume and dynamic
   frequency paths are out of scope.
6. Two edges inherited from the mainline `8250_omap` idiom, accepted
   knowingly: `unthrottle()` restores the IER but not the
   `read_status_mask` DR bit cleared by `stop_rx()` (benign — character
   insertion does not depend on it; resynced by the next
   `set_termios`); and dropping CRTSCTS *while throttled* leaves RX
   interrupts masked until the next CRTSCTS toggle or reopen (no known
   consumer changes termios while throttled).
7. Platform envelope, not a driver defect (2026-07-18 bench): at
   691200 baud a sustained full-rate bidirectional flood overruns the
   tty flip buffer (`bo`) on this 400 MHz CPU — the flip worker
   starves below hard IRQ + TCP at ~97 % load while the hardware FIFO
   stays clean. 80 %-paced traffic at the same baud is lossless;
   460800 and 892857 floods are lossless. Real radio workloads sit
   far below this envelope. Confirmed pre-existing by a same-day A/B:
   the v1.6 driver reproduces the identical loss signature and
   magnitude under the same load.

---

# Finding ID registry (both audits)

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
| 8250RTL-S01..S04 | — | implemented (v1.3) | see Part I §3 |
| 8250RTL-010 | medium | fixed (v1.7) | post-registration fcr/AFE writes raced the published port — moved to startup/shutdown |
| 8250RTL-011 | medium | fixed (v1.7) | set_mctrl RTS guard broke B0/-CRTSCTS — replaced by UPSTAT_AUTORTS + throttle callbacks |
| 8250RTL-012 | medium | accepted | 1-byte trigger IRQ amplification; erratum-bound, DoS-resistant only while peer honours RTS |
| 8250RTL-013 | low | fixed (v1.7) | phantom_log printed from hard IRQ — deferred to a workqueue |
| 8250RTL-014 | low | fixed (v1.7) | non-atomic phantom counters — atomic64 + READ_ONCE/WRITE_ONCE |
| 8250RTL-015 | low | fixed (v1.7) | MMIO size/mapsize unchecked; silent 200 MHz clock fallback removed |
| 8250RTL-016 | low | fixed (v1.7) | pin mux not restored on probe failure / removal |
| 8250RTL-017 | low | fixed (v1.7) | `quot == 1` programmed divisor 2 instead of 1 — moot below 12.5 Mbaud |

---

# Conclusion

Part I made the FCR path real (the FIFO had been genuinely off since
F-08) and pinned the RX trigger where the silicon erratum demands it.
Part II removed the last shortcuts the flow-control story was carrying:
the port now arms itself through the core's own startup/shutdown
lifecycle, RTS ownership is expressed in the core's native
`UPSTAT_AUTORTS` vocabulary instead of a `set_mctrl` override, hardware
state is restored on every exit path, and nothing logs from hard IRQ
context. Static checks and the on-target non-regression battery
(termios/MCR semantics incl. B0, 500 open/close cycles, OTBR soak) are
green on both kernel lines; the load/throttle stress and the multi-day
endurance run remain the gates before this line ships as GA.
