# RTL8196E watchdog — cumulative production audit

> **Cumulative audit ledger.** The current-state table is authoritative.
> Descriptions of removed diagnostic implementations are historical rationale,
> not a description of code still present in v1.12.

| Current state | Authoritative value |
|---|---|
| **Audited code** | driver v1.12; panic record v9 (108 bytes) |
| **Last audit pass** | 2026-07-29 — A1, production-rewrite audit |
| **Last fully audited baseline** | v1.12 |
| **Post-baseline changes** | none |
| **Validation state** | build passed; 2026-07-30 target panic/reset/one-shot test passed 10/10 |
| **Maintained kernels** | Linux 6.18 and 7.2, identical source |
| **Current finding registry** | findings closed, legacy IDs and residual constraints below |
| **DT dependency** | dedicated `watchdog-crash@1ffd000`, `no-map` |
| **Public interface** | standard root-owned Linux watchdog interface |

## Audit-pass ledger

| Pass | Date | Baseline | Result | Validation |
|---|---|---|---|---|
| A1 | 2026-07-29 | v1.11 → v1.12 | production rewrite, isolated record page and strict v9 parser | both kernels built; target panic test pending at audit time |
| V1 | 2026-07-30 | v1.12 | no code change; panic/reset, persistence and one-shot behaviour confirmed | target gateway: 10/10 checks passed |

## Findings closed by the production rewrite

### Runtime cost and robustness

The former diagnostic implementation maintained a one-Hz flight recorder and
collected scheduler, softirq, Ethernet, NAPI, IRQ-controller, and printk state
during panic. That was appropriate while diagnosing failures, but it expanded
the panic surface and imposed recurring work in a recovery driver.

v1.12 removes those mechanisms. Normal watchdog operations are constant-time
MMIO operations. The panic path performs a fixed number of register reads and
fixed-size memory writes only after the reset has been armed.

### Reserved memory isolation

The previous post-mortem record shared the `boothold` page. Although offsets
were separated, two independent producers in a reset-sensitive page were an
unnecessary coupling. The record now has its own DT-reserved no-map page at
`0x01ffd000`; `boothold` remains exclusively for its bootloader contract.

### Record parsing

The next-boot reader treats DRAM contents as untrusted. It requires the magic,
exact version 9, and exact 108-byte length. It NUL-terminates and sanitizes
the reason string before logging, then clears the magic. Unknown historical
formats are cleared without attempting compatibility decoding.

### Reset ordering

The recovery path preserves the validated two-write hardware sequence:
disable/clear followed by zero. It is executed before any optional record
work. This avoids a stale watchdog count making the reset timing ambiguous.

## Security boundary

`/dev/watchdog` is mediated by the Linux watchdog core and is root-only on
the target. A privileged user can already reset the system or access MMIO; the
driver adds no private ioctl or unprivileged control surface. The reserved
record can be forged by a privileged previous boot, which affects only an
advisory dmesg line and cannot alter control flow.

## Legacy finding IDs still referenced

Source, DT and CHANGELOG entries still cite IDs from the pre-v1.12 registry.
Their original one-line dispositions are kept here; full details are in git
history (the `AUDIT.md` preceding the v1.12 rewrite).

| ID | Status | One-liner |
|---|---|---|
| WDT-005 | closed (v1.1) | CDBR slowclk rework: 25 kHz tick, OVSEL=1001 ceiling ~671 s. At the former 25 MHz tick the window was 671 ms, too short for any userspace feeder, so the watchdog was never in service |
| WDT-006 | closed (v1.0) | OVSEL is a 4-bit field (datasheet), not the SDK's 2-bit |
| WDT-007 | closed (v1.1) | `S25watchdog` feeder enabled, DT `timeout-sec=60` |
| WDT-008 | closed (v1.2) | soft-lockup blind spot: panic notifier + `BOOTPARAM_SOFTLOCKUP_PANIC=y` → ~23 s autonomous recovery |
| WDT-009 | closed (v1.2) | panic notifier priority pinned `INT_MAX` |
| WDT-010 | closed (v1.4) | v1.3 arm-race regression: a single-write arm let a stale up-counter (userspace-armed OVSEL=9) instantly overflow the OVSEL=0 threshold, resetting the chip before the post-mortem write. Fixed by the two-step arm (clear while halted, then enable), still used by `rtl819x_wdt_arm_reset()` |

## CDBR slowclk rationale (formerly section 6)

The watchdog window is 2^24 ticks / f, and the tick comes from CDBR, shared
with Timer0/Timer1. Raising f improves timekeeping resolution but shrinks the
watchdog window by the same factor:

| f | Clock resolution | WDT window | State |
|---|---|---|---|
| 25 MHz (original) | 40 ns | 671 ms | watchdog unarmable, never in service (WDT-005) |
| 250 kHz (rejected 2026-07-02) | 4 µs | 67 s | reset mid-bench under saturating load |
| 25 kHz (current) | 40 µs | 671 s | retained |

The 250 kHz retune (DivFactor=800) passed the idle checks but hard-reset the
box during the second sustained `iperf3 -R` TX rep, with no panic record: a raw
hardware overflow. The 30 s-period BusyBox feeder only kicked in idle gaps,
never inside a CPU-saturating rep. The sizing rule is therefore not "window >
timeout": the window must exceed the longest CPU-saturating episode the box
can legitimately face (bench stress reps run 300 s) plus the feeder period.
671 s absorbs that; 67 s does not. Re-attempting a higher frequency first
requires bounding the feeder-kick tail under 300 s saturating reps, or moving
the kick into a health-gated hardirq context, which is a redesign.

Vendor lineage (V3.4.7.3 SDK): DivFactor=8000 (25 kHz) is the linux-2.6.30-era
BSP value. The linux-3.10 BSP of the stock firmware uses DivFactor=200 (1 MHz
tick) with the watchdog at OVSEL=1001, a 16.8 s window kicked from userspace
through a `rtl_gpio` char-dev write. Neither frequency is vendor parity; 25 kHz
is the value this firmware's validation has proven.

## Residual constraints

- The watchdog clock is derived from CDBR, shared with the timer block. Any
  board change to that divider must revalidate watchdog timing.
- The RTL8196E reset-indicator bit is not reliable on all observed silicon; do
  not use it as sole proof of watchdog reset.
- The compact record is deliberately insufficient for incident forensics.
  Reintroduce diagnostics only in a separate debug build, not this driver.

## Review checklist for future changes

- Keep `start`, `stop`, and `ping` allocation-free and O(1).
- Keep recovery before diagnostics in the panic notifier.
- Keep the magic-last publication and strict reader gates.
- Do not share the crash page with bootloader or unrelated driver state.
- Validate DT placement and a controlled panic/reset after altering WDT or
  timer-clock programming.
