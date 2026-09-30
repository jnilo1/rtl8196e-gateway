# irq-rtl819x — cumulative driver audit

> **Cumulative audit ledger.** The current-state table and the §4 registry are
> authoritative. Closed passes are summarized; their full text is in the git
> history of this file.

| Current state | Authoritative value |
|---|---|
| **Current implementation** | `irq-rtl819x` v1.2 |
| **Audited code** | driver 1.2 (`DRV_VERSION` in `irq-rtl819x.c`) |
| **Last audit pass** | 2026-07-22 — A3, targeted diagnostic-hygiene review |
| **Last fully audited baseline** | v1.1 (A2 plus implemented recommendations) |
| **Post-baseline changes** | v1.2 corrects CPU-line labels/counters and diagnostic semantics; routing and interrupt data path are unchanged |
| **Validation state** | A2 boot-verified on target; A3 independently reviewed with no security/runtime defect |
| **Maintained kernels** | Linux 6.18 and 7.2, identical source |
| **Current finding registry** | §4 |
| **Scope** | interrupt controller, arch dispatcher/diagnostics, DT parent routing |
| **Companion** | `DESIGN.md` |

## Audit-pass ledger

| Pass | Date | Baseline | Result | Validation |
|---|---|---|---|---|
| A1 | 2026-05-01 | pre-v1.0 | IRQ-001…007 and PERF-UART1-IRR established; driver 1.0 shipped in v3.4.0 | historical |
| A2 | 2026-06-12 | v1.0 → v1.1 | IRQ-008/009 fixed; IRQ-010 accepted; IRQ-S01/S02 implemented | build and target boot/interrupt check |
| A3 | 2026-07-22 | v1.1 → v1.2 | corrected IP3/IP4 labels, TC0 counter and telemetry meaning; no routing change | targeted independent source review |

A2 superseded the A1 document; the legacy IDs are kept in §4 with statuses
re-verified against the A2 baseline.

---

## 1. Security review (A2, re-checked in A3)

### 1.1 Attack surface

None from userspace: no sysfs, ioctl or module parameter. Inputs are the
device tree (build-time, trusted), the MMIO registers at `intc@3000` and the
kernel irqchip callbacks. A peripheral interrupt storm is that peripheral
driver's problem, not an INTC vulnerability.

### 1.2 Verified properties

- **Bounds checks.** `mask`/`unmask`/`ack` reject `hwirq >= 32` before
  touching `BIT(hwirq)`.
- **GIMR RMW locking.** Mask/unmask take `raw_spin_lock_irqsave` around the
  read-modify-write. The GISR ack is a single lock-free W1C write.
- **Chained-handler flow.** `chained_irq_enter`/`exit` bracket the dispatch;
  `pending = GIMR & GISR` never dispatches a masked source; an unmapped pending
  bit hits `pr_warn_ratelimited`, not a NULL dispatch.
- **Cross-IP drain is benign and intentional.** The three chained parents
  (IP2/IP3/IP4) share one handler that drains every pending bit, so a sibling
  IP invocation may find `pending == 0` (enter/exit, no loop). Single core with
  IRQs disabled in the handler: no double dispatch. `__ffs` services the lowest
  bit first — UART0 (12), UART1 (13), switch (15) — which keeps the
  UART-before-Ethernet intent of PERF-UART1-IRR on the drain path.
- **virq cache has no race.** The three cached virqs are written in
  `intc_map()`, which the legacy domain runs at create time, strictly before
  any chained handler is installed (hence IRQ-007).
- **TC0 invariant.** `GIMR = BIT(8)` at init is the only unconditional arm.
  IRQ-002 still holds: there is no direct TC0→IP7 hardware path, so clearing
  bit 8 would hang the kernel at clocksource init. The block comment above the
  write carries the rationale.
- **Init ordering.** IRR routing is programmed before the domain exists and
  before GIMR enables anything; UART/switch sources stay disabled until their
  consumer's `request_irq()` walks `.irq_unmask` (IRQ-001).

**Verdict:** no security-relevant flaw and no userspace-reachable surface.

---

## 2. Findings from A2

- **IRQ-008 (info, fixed).** No SPDX identifier and 4-space indentation, the
  only custom driver failing `checkpatch.pl` on sight. Fixed by IRQ-S01: SPDX
  `GPL-2.0-only` header, file retabbed to kernel style.
- **IRQ-009 (info, fixed).** `intc_of_init()` jumped to `err_iounmap` without
  `irq_domain_remove(domain)` when the DT described no parent IRQ
  (unreachable with the in-tree DT). Fixed by IRQ-S02, which also NULLs
  `rtl819x_intc_base` after the unmap.
- **IRQ-010 (info, accepted).** The legacy domain's contiguous revmap makes
  `irq_find_mapping()` an O(1) lookup, so the three-case virq cache saves only
  a call. It stays — correct, free, and in the bench-gated per-interrupt path —
  but must not be extended: new sources should use `irq_find_mapping()`.

IRQ-S01/S02 were implemented on 2026-06-12 with no functional change on the
success path; boot-verified on the bench gateway (`/proc/interrupts` normal,
ERR=0).

### Considered and rejected (A2)

- **Legacy → linear domain** (IRQ-005): all consumers resolve through the DT,
  nothing depends on the fixed virq base 16, and the change would renumber
  `/proc/interrupts` for no gain.
- **Dropping the virq cache**: equivalent performance; churn in a bench-gated
  path.
- **Fused `.irq_mask_ack`**: saves one lock round-trip (a few hundred ns) per
  interrupt, below measurability and far below the platform's regression
  threshold.
- **DT-driven source-bit validation / IRR tables** (IRQ-006): the constants
  match the in-tree DT, the Sengled G4 uses the same SoC routing, and a
  mismatch fails loudly (no interrupts).
- **`READ_ONCE`/`WRITE_ONCE` on the virq cache** (IRQ-007): single core, and
  the writes complete before any reader exists.

---

## 3. A3 follow-up (2026-07-22, v1.2)

Diagnostic hygiene only: the IP3/IP4 CPU-line labels and counters were
corrected, the TC0 counter and telemetry wording were made accurate. Routing
and the interrupt data path are unchanged; no security or runtime defect was
found.

---

## 4. Finding ID registry

| ID | Severity | Status | Summary |
|---|---|---|---|
| IRQ-001 | high | fixed (v3.4.0) | GIMR armed all sources at init, before consumers existed |
| IRQ-002 | high | rejected | TC0 "dual routing" is the only hardware path (bootloader-verified); changing it would hang boot |
| IRQ-003 | medium | fixed (v3.4.0) | parent IPs declared in DT and parsed, not hardcoded |
| IRQ-004 | medium | fixed (v3.4.0) | duplicate GISR ack dropped; `.irq_ack` via the level flow only |
| IRQ-005 | medium | deferred | legacy domain with base 16 — works, no gain migrating |
| IRQ-006 | low | deferred | hardcoded source bits not validated against DT (DT matches; fails loudly) |
| IRQ-007 | low | rejected | `READ_ONCE` on the virq cache — UP, writes precede readers |
| PERF-UART1-IRR | perf | applied (v3.4.0) | UART1→IP4 / switch→IP3 swap; soak-validated |
| IRQ-008 | info | fixed (2026-06-12) | no SPDX line; 4-space indentation |
| IRQ-009 | info | fixed (2026-06-12) | irq domain not removed on the no-parent error path |
| IRQ-010 | info | accepted | virq cache redundant with the legacy revmap; keep, don't extend |
| IRQ-S01, IRQ-S02 | — | implemented (2026-06-12) | SPDX + retab; `irq_domain_remove()` on the error path (§2) |

---

## 5. Conclusion

A small driver whose v3.4.0 fixes have held: init arms exactly one source, the
DT is the single source of truth for the parent topology, and the TC0 special
case is documented in the code at the point of risk. No security surface and
no open actionable item; IRQ-005/006 remain deliberate deferrals.
