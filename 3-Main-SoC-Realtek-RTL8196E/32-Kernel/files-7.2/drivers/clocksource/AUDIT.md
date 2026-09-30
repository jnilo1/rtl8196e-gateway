# RTL8196E timer/clocksource — cumulative production audit

> **Cumulative audit ledger.** The current-state table and disposition table
> are authoritative. Detailed prose is retained as evidence for the pass that
> produced it.

| Current state | Authoritative value |
|---|---|
| **Current implementation** | `timer-rtl819x` v1.3 |
| **Audited code** | driver 1.3 (`DRV_VERSION` in `timer-rtl819x.c`) |
| **Last audit pass** | 2026-07-29 — A1, full production audit |
| **Last fully audited baseline** | v1.3 |
| **Post-baseline changes** | none |
| **Validation state** | build and DNT feasibility/soak passed; DNT (`CONFIG_RTL819X_TC0_DNT=y`) has shipped in every image since v4.0.0 |
| **Maintained kernels** | Linux 6.18 and 7.2, identical source |
| **Current finding registry** | `Disposition` below |
| **Scope** | Timer0 clockevent, Timer1 clocksource, DT/config/IRQ/watchdog coupling |

## Audit-pass ledger

| Pass | Date | Baseline | Result | Validation |
|---|---|---|---|---|
| A1 | 2026-07-29 | v1.3 | production design and TMR-010…020 dispositions established | build plus 26.3 h / 54.8 M-reprogram DNT soak; the hardware canary was not run as a separate gate; DNT has shipped since v4.0.0 |

## Verdict

There is no driver-specific unprivileged ABI, allocation, DMA, parser, or user
pointer. The residual risk is availability and time integrity: Timer1 is the
only clocksource and TC0 is the only clockevent. The v1.3 DNT implementation avoids a
TC0 enable-edge failure observed in the field; the physical CDC mechanism is
an inference, not a vendor-documented erratum.

## Disposition

| ID | Disposition |
|---|---|
| TMR-001 | Legacy, closed (v3.4.0): divider validated (0 < div ≤ 0xffff), `busclk` enable checked; its 200 MHz fallback was later removed by TMR-010. |
| TMR-002 | Legacy, closed (v3.4.0): Timer0 quiesced and `request_irq()` done before `clockevents_config_and_register()` (IP7 storm prevention). |
| TMR-003 | Legacy, closed (v3.4.0): handler returns `IRQ_NONE` when `TC0_PENDING` is clear. |
| TMR-004 | Legacy, closed (v3.4.0): `clocksource_register_hz()` return propagated. |
| TMR-005 | Legacy, closed (v3.4.2 cycle, shipped in v3.5.0): slowclk CDBR rework, DivFactor 8 → 8000, 25 kHz regime, `min_delta` 0x300 → 8; the clocksource side of the watchdog WDT-005 closure. |
| TMR-006 | Legacy, fixed (driver v1.1): an IRQ-map failure panics like the other init failures instead of soft-failing. |
| TMR-007 | Legacy, fixed (driver v1.1): `clk_put()` while the clock was enabled; the references are now held. |
| TMR-008 | Legacy, closed as a documented exception: family-generic compatible kept; superseded by TMR-014 (RTL8196E compatible, generic fallback retained). |
| TMR-009 | Legacy, fixed (driver v1.1): DT `reg` window overlapped `watchdog@311c`; node shrunk to `0x1c` (the claim itself is TMR-013). |
| TMR-010 | `busclk` is mandatory; missing, zero or failed clocks panic. |
| TMR-011 | Divider must be exact, in range, and pass CDBR readback. |
| TMR-012 | Both timers and both IRQ sources are quiesced before CDBR changes. |
| TMR-013 | `timer_of` claims the 0x1c timer resource. |
| TMR-014 | RTL8196E DT compatible and schema are retained. |
| TMR-015 | MODE=1 is named TIMER mode; MODE=0 is COUNTER mode. |
| TMR-016 | `NO_HZ_IDLE=y` is restored in both maintained configurations. |
| TMR-017 | `irqtime=1` remains a measured diagnostic option only. |
| TMR-018 | Open: only an external RTC/NTP/host can detect oscillator drift or a later TC1 stall. |
| TMR-019 | `min_delta=8` bounds generic timer-driven IRQ load to about 3125/s. |
| TMR-020 | This document and DESIGN/VALIDATION describe v1.3. |

## TC0 operating modes

`CONFIG_RTL819X_TC0_DNT=n` is the Kconfig default, but no shipped image uses
it. It uses protected COUNTER-mode
arming: disable, load DATA0, enable, then prove progress from COUNT0 over a
bounded TC1 window; it retries a fresh enable edge up to four times. CTRL
readback is not accepted as proof because it only reflects the bus register.

`CONFIG_RTL819X_TC0_DNT=y` is the production DNT path, set in both shipped
configurations. At the first
clockevent it masks/acknowledges TC0, enters TIMER mode once, and keeps TC0_EN
set. Each later deadline writes DATA0 then enables the IRQ while preserving a
pending minimum-delta expiry. ISR and NO_HZ shutdown only operate on IR; they
never write CTRL or DATA0. There is no DT, sysfs, debugfs or userspace runtime
mode switch; reboot starts protected.

## Performance and concurrency

At 25 kHz, one timer tick is 40 us and `min_delta=8` is 320 us. `NO_HZ_IDLE`
removes the periodic idle tick, but does not replace DNT: busy systems still
need one-shot programming. DNT's steady-state arm is DATA0 write plus IR
read/modify/write; its ISR is IR read/modify/write. It has no busy wait,
telemetry counter, reserved DRAM page, or host control path.

Protected arming is intentionally more expensive: it polls COUNT0/COUNT1 with
interrupts disabled for at most six slow-clock edges and retries at most four
times. It is a safe default/fallback, not the low-cost production regime.

The platform is UP/CPU0 and clockevent callbacks execute with local IRQs
disabled, so CTRL/IR RMW operations need no lock. Timer1 IRQ remains disabled.

## Validation boundary

The DNT feasibility suite validated TIMER-mode DATA0 reload, one IRQ per
deadline, NO_HZ transitions and min/intermediate/MAX cells. The cause-level
soak completed 26.3 h and 54.8 million valid reprograms under saturation with
zero wedge, WDT reset, missed IRQ, double IRQ, parasitic IRQ, or unresolved
write-ahead record in the soak instrumentation.
`pend_at_unmask` at delta 8 was observed as delivered-IRQ telemetry, not a
failure. DNT then shipped as the production configuration in v4.0.0; the
idle/wakeup and saturation checks in VALIDATION.md remain the gate for any
later timer change.
