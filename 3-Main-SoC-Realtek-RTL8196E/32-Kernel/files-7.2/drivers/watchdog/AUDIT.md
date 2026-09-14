# RTL8196E watchdog — cumulative production audit

> **Cumulative audit ledger.** The current-state table is authoritative.
> Descriptions of removed diagnostic implementations are historical rationale,
> not a description of code still present in v1.12.

| Current state | Authoritative value |
|---|---|
| **Current implementation** | `rtl819x-wdt` v1.12; panic record v9 (108 bytes) |
| **Release target** | firmware v4.0.0 |
| **Last audit pass** | 2026-07-29 — A1, production-rewrite audit |
| **Last fully audited baseline** | v1.12 |
| **Post-baseline changes** | none |
| **Validation state** | build passed; 2026-07-30 target panic/reset/one-shot test passed 10/10 |
| **Maintained kernels** | Linux 6.18 and 7.1, identical source |
| **Current finding registry** | findings closed and residual constraints below |
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
