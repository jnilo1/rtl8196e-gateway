# irq-rtl819x — cumulative driver audit

> **Cumulative audit ledger.** The current-state table and §4 registry are
> authoritative. The detailed review below records the v1.0/v1.1 pass; the
> later v1.2 diagnostic-hygiene pass is summarized separately in the ledger.

| Current state | Authoritative value |
|---|---|
| **Current implementation** | `irq-rtl819x` v1.2 |
| **Release target** | firmware v4.0.0 |
| **Last audit pass** | 2026-07-22 — A3, targeted diagnostic-hygiene review |
| **Last fully audited baseline** | v1.1 (A2 plus implemented recommendations) |
| **Post-baseline changes** | v1.2 corrects CPU-line labels/counters and diagnostic semantics; routing and interrupt data path are unchanged |
| **Validation state** | A2 boot-verified on target; A3 independently reviewed with no security/runtime defect |
| **Maintained kernels** | Linux 6.18 and 7.1, identical source |
| **Current finding registry** | §4 plus A3 ledger entry |
| **Scope** | interrupt controller, arch dispatcher/diagnostics, DT parent routing |
| **Companion** | `DESIGN.md` |

## Audit-pass ledger

| Pass | Date | Baseline | Result | Validation |
|---|---|---|---|---|
| A1 | 2026-05-01 | pre-v1.0 | IRQ-001…007 and routing disposition established | historical |
| A2 | 2026-06-12 | v1.0 → v1.1 | IRQ-008/009 fixed; IRQ-010 accepted; S01/S02 implemented | build and target boot/interrupt check |
| A3 | 2026-07-22 | v1.1 → v1.2 | corrected IP3/IP4 labels, TC0 counter and telemetry meaning; no routing change | targeted independent source review |

Pass A2 **cancels and replaces** the 2026-05-01 audit. That pass produced
driver 1.0 (findings IRQ-001/-003/-004 fixed, IRQ-002 rejected with the
bootloader-sourced TC0 analysis, IRQ-005/-006/-007 deferred) plus the
PERF-UART1-IRR routing swap; all of it shipped in v3.4.0 and has soaked
through six releases since. The legacy IDs are preserved in the registry
(§4). Everything below was re-verified against the A2 baseline; the A3
follow-up is summarized in the current-state table and ledger.

---

## 1. Security review

### 1.1 Attack surface

Effectively none. The driver exposes no userspace interface — no sysfs, no
ioctl, no module parameters. Its inputs are the device tree (build-time,
trusted), MMIO registers at `intc@3000`, and the kernel-internal irqchip
callbacks. The only "remote" influence is a peripheral raising its interrupt
line, which is the driver's job to handle; a storm from a peripheral is that
peripheral driver's problem (and is what issue #99's instrumentation
watches), not an INTC vulnerability.

### 1.2 Verified correct

- **Bounds checks.** `mask`/`unmask`/`ack` all reject `hwirq >= 32` before
  touching `BIT(hwirq)` — no out-of-range register write is reachable even
  through a misprogrammed consumer.
- **GIMR RMW locking.** Mask/unmask take `raw_spin_lock_irqsave` around the
  read-modify-write; callable safely from process and hardirq context. The
  GISR ack is a lock-free single W1C write, which is correct (no RMW).
- **Chained-handler flow.** `chained_irq_enter`/`exit` bracket the dispatch;
  `pending = GIMR & GISR` ensures masked sources are never dispatched; an
  unmapped pending bit hits `pr_warn_ratelimited`, not a NULL dispatch
  (`generic_handle_irq` is guarded by `likely(virq)`).
- **Cross-IP drain is benign (and intentional).** All three chained parents
  (IP2/IP3/IP4) share one handler that drains *every* pending GIMR&GISR bit.
  When two IPs assert together, the first invocation services both sources
  and the sibling IP's invocation finds `pending == 0` — exactly the
  "spurious" path the in-code comment documents (enter/exit, no loop).
  Single-core, IRQs disabled in the handler: no double dispatch is possible.
  Within one invocation `__ffs` services the lowest bit first — UART0 (12),
  UART1 (13), switch (15) — which keeps the UART-before-Ethernet intent of
  the PERF-UART1-IRR swap even on the drain path.
- **virq cache has no race.** The three cached virqs are written in
  `intc_map()`, which the legacy domain runs eagerly at create time —
  strictly before any chained handler is installed. IRQ-007's deferral
  (no `READ_ONCE` needed on UP) remains correct.
- **TC0 invariant intact.** `GIMR = BIT(8)` at init is still the only
  unconditional arm, and the IRQ-002 analysis still holds: there is no
  direct TC0→IP7 hardware path, so clearing bit 8 would hang the kernel at
  clocksource init. The block comment above the write carries the full
  rationale with bootloader line references — good.
- **Init ordering.** IRR routing is programmed before the domain exists and
  before GIMR enables anything; UART/switch sources stay disabled until
  their consumer's `request_irq()` walks `.irq_unmask` (IRQ-001 fix,
  re-verified).

### 1.3 Verdict

No security-relevant flaw; no userspace-reachable surface at all. The
remaining items are hygiene and dead-weight notes below.

---

## 2. Findings (new this audit)

### IRQ-008 — no SPDX identifier; non-kernel indentation (info)

The file opens with the long-form GPL-2.0 paragraph instead of an
`// SPDX-License-Identifier: GPL-2.0` line (every other custom driver in
this tree carries one), and the body is indented with 4 spaces rather than
tabs. Purely cosmetic on a private tree, but it is the only driver here that
would fail `checkpatch.pl` on sight. Fix opportunistically when the file is
next touched (IRQ-S01).

### IRQ-009 — error path leaks the irq domain (info)

In `intc_of_init()`, if the DT describes no parent IRQ the code jumps to
`err_iounmap` without `irq_domain_remove(domain)`. Unreachable in practice
(the in-tree DT always has `interrupts = <2>, <3>, <4>` and a system that
took this path would be unbootable anyway — no peripheral interrupts), so
this is recorded for completeness, not urgency.

### IRQ-010 — virq cache is now near-redundant (info, keep as is)

The legacy domain allocates a contiguous revmap, so `irq_find_mapping()` on
the default path is an O(1) array lookup; the three-case `switch` saves only
a function call and its checks. The cache stays — it is correct, costs
nothing, and sits in a hot `__iram` path on a bench-gated platform where
removal would buy no measurable win in exchange for churn — but it should
not be *extended* to new sources; new mappings should just rely on
`irq_find_mapping()`.

---

## 3. Simplification / 6.18 alignment

The driver already uses the current API surface (`IRQCHIP_DECLARE`,
`irq_domain_create_legacy` with `fwnode_handle`, `irq_of_parse_and_map`
walking the DT parent list). Candidates:

| ID | Change | Value |
|---|---|---|
| IRQ-S01 | SPDX line + retab to kernel style | Closes IRQ-008; zero functional risk |
| IRQ-S02 | `irq_domain_remove()` on the no-parent error path | Closes IRQ-009 |

### Considered and rejected (this audit)

- **Legacy → linear domain migration** (IRQ-005). Still deferred: all
  consumers resolve through the DT, nothing depends on the fixed virq base
  16, `irq_domain_create_legacy` is alive and well in 6.18, and the linear
  variant would change `/proc/interrupts` numbering for zero gain.
- **Dropping the virq cache** (mirror of IRQ-010). Equivalent performance
  either way; removal is churn in an `__iram` hot path on a platform where
  every perf change is bench-gated. Not worth a test cycle.
- **Combined `.irq_mask_ack` callback.** `handle_level_irq` currently runs
  mask (lock + RMW + write) then ack (write) as two calls; a fused callback
  would save one lock round-trip per interrupt. At 200 MHz bus that is a few
  hundred ns on a path measured in µs — below this platform's 1 Mbit/s
  regression threshold and below measurability. Rejected.
- **DT-driven source-bit validation / IRR routing tables** (IRQ-006). The
  `REALTEK_HW_*_BIT` constants match the in-tree DT, and the one external
  port in progress (Sengled G4) is the same SoC with the same routing. A
  mismatch would fail loudly (no interrupts), not silently. Still deferred.
- **`READ_ONCE`/`WRITE_ONCE` on the virq cache** (IRQ-007). Single-core,
  and §1.2 shows the writes complete before any reader exists. Still
  rejected.

---

## 4. Finding ID registry

Legacy IDs from the 2026-05-01 pass (statuses re-verified against current
code), then this audit's additions:

| ID | Severity | Status | Summary |
|---|---|---|---|
| IRQ-001 | high | **fixed (v3.4.0)** | GIMR armed all sources at init, before consumers existed |
| IRQ-002 | high | rejected | TC0 "dual-routing" is the only hardware path (bootloader-verified); change would hang boot |
| IRQ-003 | medium | **fixed (v3.4.0)** | parent IPs now declared in DT and parsed, not hardcoded |
| IRQ-004 | medium | **fixed (v3.4.0)** | duplicate GISR ack dropped; `.irq_ack` via level flow only |
| IRQ-005 | medium | deferred | legacy domain with base 16 — works, no gain migrating |
| IRQ-006 | low | deferred | hardcoded source bits not validated against DT (DT matches; fails loudly) |
| IRQ-007 | low | rejected | `READ_ONCE` on virq cache — UP, writes precede readers |
| PERF-UART1-IRR | perf | **applied (v3.4.0)** | UART1→IP4 / Switch→IP3 swap; soak-validated |
| IRQ-008 | info | fixed (2026-06-12) | no SPDX line; 4-space indentation |
| IRQ-009 | info | fixed (2026-06-12) | irq domain not removed on no-parent error path |
| IRQ-010 | info | accepted | virq cache redundant vs legacy revmap; keep, don't extend |
| IRQ-S01..S02 | — | implemented (2026-06-12) | see §3 and the note in §5 |

---

## 5. Conclusion

The smallest and cleanest driver audited so far (200 pure LOC), and the
2026-05 fixes have held: init arms exactly one source, the DT is the single
source of truth for the parent topology, and the TC0 special case is
documented in the code at the point of risk. No security surface exists.
The only open items are two hygiene nits (IRQ-S01/S02) suitable for the next
time the file is touched for any other reason — neither justifies a kernel
rebuild on its own.

**Implementation note (2026-06-12):** S01–S02 implemented on maintainer
request: SPDX `GPL-2.0-only` header (long-form GPL paragraph dropped),
file retabbed to kernel style, `irq_domain_remove()` on the no-parent
error path (plus `rtl819x_intc_base` NULLed after iounmap). No
functional change on the success path; boot-verified on the .88 gateway
(`/proc/interrupts` normal, ERR=0).
