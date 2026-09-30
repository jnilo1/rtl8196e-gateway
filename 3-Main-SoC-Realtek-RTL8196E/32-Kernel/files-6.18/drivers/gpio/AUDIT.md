# RTL8196E GPIO driver (`gpio-rtl819x`) — cumulative security audit

> **Cumulative audit ledger.** The current-state table and the §4 registry are
> authoritative. Closed passes are summarized; their full text is in the git
> history of this file.

| Current state | Authoritative value |
|---|---|
| **Current implementation** | `gpio-rtl819x` v1.3 |
| **Audited code** | driver 1.3 (`DRV_VERSION` in `gpio-rtl819x.c`) |
| **Last audit pass** | 2026-07-22 — A3, targeted diagnostic/probe-hygiene review |
| **Last fully audited baseline** | v1.2 (A2) |
| **Post-baseline changes** | v1.3 preserves the data path and improves syscon error fidelity, including `-EPROBE_DEFER`; reviewed in A3 |
| **Validation state** | v1.2 target gate passed; v1.3 independent review found no security/runtime defect |
| **Maintained kernels** | Linux 6.18 and 7.2, identical source |
| **Current finding registry** | §4 |
| **Audited surface** | driver, GPIO DT consumers/config, and cross-driver `PIN_MUX_SEL_2` writers |

## Audit-pass ledger

| Pass | Date | Baseline | Result | Validation |
|---|---|---|---|---|
| A1 | 2026-05-01 | pre-v1.1 | GPIO-001…006 established | historical |
| A2 | 2026-06-11/12 | v1.0 → v1.2 | GPIO-007/008 found; GPIO-008 and S01…S07 closed in v1.1/v1.2; GPIO-007 closed by eth v2.7 | build plus LED/button/nRST target gate |
| A3 | 2026-07-22 | v1.2 → v1.3 | syscon error handling and diagnostic wording hardened; no runtime finding | targeted independent source review |

A2 was a fresh audit that superseded the A1 document; the legacy IDs
GPIO-001…006 were re-verified against v1.2 and are kept in §4.

---

## 1. Security review (A2, re-checked in A3)

### 1.1 Attack surface

| Surface | Exposure | Assessment |
|---|---|---|
| `/dev/gpiochip0` (cdev v2 only — `GPIO_CDEV_V1` unset) | `crw------- root root` (0600) | Root-only. The gpiolib core validates every offset (`offset < ngpio`) before any driver op; line claims are exclusive. |
| sysfs | `GPIO_SYSFS=y`, legacy export/unexport ABI compiled out (`GPIO_SYSFS_LEGACY` unset) | Read-only class info; no line manipulation path. |
| Module parameters / ioctls / procfs | none | No interface beyond gpiolib. |
| syscon `0x44` writes | fixed masks/values selected by a `switch` on the core-validated offset | No user-controlled value reaches the regmap write. |

The driver parses no input, owns no buffers, does no DMA and never copies
to/from userspace. No trust boundary is crossed. Root misuse of a GPIO (e.g.
toggling `efr32-nrst` by hand) is a root capability by design.

### 1.2 Verified properties

- **Locking.** The data path is the generic MMIO core (`gpio_generic_chip`,
  GPIO-S01), whose IRQ-safe lock serializes the DATA/DIR read-modify-writes;
  `can_sleep = false` is honest (the syscon regmap is MMIO `fast_io`), so atomic
  consumers such as LED triggers are valid.
- **Glitch-free output.** The generic default `direction_output` is the
  value-first variant (DATA before DIR). It is selected because the driver
  never passes `GPIO_GENERIC_NO_SET_ON_INPUT`; keep it that way.
- **Open-drain consumers.** The chip has no native open-drain; gpiolib's
  emulation (drive low = output 0, release = input) maps onto the direction
  ops, which is how the uart-bridge `nrst-gpios`
  (`GPIO_ACTIVE_LOW | GPIO_OPEN_DRAIN`) is driven.
- **Pinmux error propagation (GPIO-002)** and **match-table tightness
  (GPIO-003)** still hold. The single `realtek,rtl8196e-gpio` compatible is
  load-bearing: the `PIN_MUX_SEL_2` field layout is RTL8196E-specific.
- **No torn RMW on `0x44`.** Every kernel writer of `PIN_MUX_SEL_2` (this
  driver, `rtl8196e-eth`) uses the same syscon regmap, whose lock serializes
  the read-modify-writes. The 8250 driver touches `0x40` only; the uart-bridge
  delegates its B4 mux to this driver. The remaining cross-driver issue was a
  policy race, GPIO-007.

**Verdict:** no vulnerability. Root-only surface, core-validated inputs,
constant-mask syscon writes. The findings below are functional robustness.

---

## 2. Findings from A2

### GPIO-007 — last-writer-wins on `PIN_MUX_SEL_2` across drivers (medium, closed by eth v2.7)

This driver sets the B4 `[7:6]`, B5 `[10:9]` and B6 `[13:12]` fields to `0b11`
(GPIO) at `request()` time. Before the fix, `rtl8196e_hw_init()` in
`rtl8196e-eth` cleared them to 0 and ran from `rtl8196e_open()`, i.e. on every
interface up. Boot ordering hid it (the uart-bridge claims GPIO 12,
`efr32-nrst`, after eth is up), but a later `eth0` down/up un-muxed a held
line: gpiolib still reported it owned while the pad was electrically
disconnected, so an nRST pulse silently stopped resetting the radio, and on a
Sengled G4 the B6 `reset-button` died. There is no pinctrl subsystem on this
platform; `0x40`/`0x44` are shared by convention only.

**Resolution.** `rtl8196e-eth` v2.7 (shipped in v4.0.0) derives every B2–B6
field from this node's `gpio-line-names`: a named pad gets `0b11`, so an eth
flap re-asserts the value `request()` set. eth v2.8 (`realtek,led-pads` on
this node) narrowed `0b00` to declared LED pads only, leaving any other pad
`0b11` (unclaimed GPIO, Hi-Z), so an anonymous cdev claim also survives. Since
eth v2.11 (ETH-S03) `hw_init()` runs once at probe, so an eth flap no longer
touches `0x44` at all. Re-asserting the mux in this driver's `direction_*`/
`set` ops was rejected (hot-path regmap traffic). Any future `free()`-time
restore logic (GPIO-006) must follow the same line-names rule, not fight it.

### GPIO-008 — silent no-mux degradation when the syscon is missing (low, closed v1.1)

Without the syscon, `request()` on a mux-requiring line (B2–B6, offsets 10–14)
used to succeed while the pad stayed in peripheral mode. v1.1 returned
`-ENODEV` with a `dev_err` for those offsets only. Superseded in v1.3: a failed
`realtek,syscon` lookup now fails the probe through `dev_err_probe()`
(propagating `-EPROBE_DEFER`), so the bank never registers without its mux
regmap.

---

## 3. Simplification items (A2, all closed)

| ID | Item | Outcome |
|---|---|---|
| GPIO-S01 | Convert the data path to the generic MMIO core: DATA at 0x0C, DIR at 0x08 (1 = out), 4-byte registers, no set/clr registers | Closed v1.2. On 6.18 and later this is `gpio_generic_chip_init()` with `struct gpio_generic_chip_config` (`<linux/gpio/generic.h>`); `GPIO_RTL819X` selects `GPIO_GENERIC`. Only the custom `.request` (CNR + pinmux) remains. |
| GPIO-S02 | Drop the lock around the single `readl` in `get_direction` | Obsolete: the hand-rolled op no longer exists after S01. |
| GPIO-S03 | `devm_platform_ioremap_resource()` | Closed v1.1. |
| GPIO-S04 | Drop the unused `platform_set_drvdata()` | Closed v1.1. |
| GPIO-S05 | Drop `<linux/of_device.h>` / `<linux/of.h>`, add `<linux/mod_devicetable.h>` | Closed v1.1, build-verified. |
| GPIO-S06 | Retab to kernel style | Closed as a standalone whitespace-only commit (`diff -w` empty, object byte-identical). |
| GPIO-S07 | Declare the IMR pair consistently: `PAB_IMR` 0x14, `PCD_IMR` 0x18 | Closed v1.1. |

Considered and rejected: a `set_config` open-drain implementation — the
hardware has no open-drain mode and gpiolib's emulation is exactly right for
the single open-drain consumer (nRST).

**v1.2 hardware gate (2026-06-12, Lidl board): passed.** STATUS LED duty bands
by DATA-register sampling (0 → 40/40 samples off, 255 → 0/40, 128 → 21/40),
button via the `s40button` cdev path, and an nRST pulse answered by the EFR32's
spontaneous ASH RSTACK frame — the open-drain emulation drives through the
generic direction ops.

---

## 4. Finding ID registry (complete)

The full A1 text of GPIO-001…006 is in the git history of this file.

| ID | Status | One-liner |
|----|--------|-----------|
| GPIO-001 | closed (v3.4.0) | dynamic base (`-1`) instead of deprecated `base = 0` |
| GPIO-002 | closed (v3.4.0) | pinmux `regmap_update_bits` error propagated from `.request()` |
| GPIO-003 | closed (v3.4.0) | match table narrowed to `realtek,rtl8196e-gpio` (the per-SoC convention cited by other driver audits) |
| GPIO-004 | open — deferred | no irqchip despite ISR/IMR registers. The RTL8196E-CG datasheet (Table 36) is now in hand, but there is no consumer: `s40button` polls via the cdev and `efr32-nrst` is an output. Revisit only if an edge-triggered consumer appears |
| GPIO-005 | open — deferred | no `valid_mask` for hardwired pins (B2/LAN LED is ASIC-driven). Soft-mitigated since v3.10.0: `gpio-line-names` leaves such pads unnamed, so name-based lookups cannot land on them |
| GPIO-006 | open — deferred | `free()` restores neither pinmux nor CNR; a B2–B6 line stays in GPIO mode after release. Any restore policy must agree with the GPIO-007 line-names rule |
| GPIO-007 | closed (eth v2.7) | eth `ndo_open` re-cleared B4/B5/B6 mux fields under held GPIOs (§2) |
| GPIO-008 | closed (v1.1; superseded v1.3) | mux-requiring requests succeeded without a syscon; now the probe fails (§2) |
| GPIO-S01 | closed (v1.2) | generic MMIO core conversion, hardware gate passed (§3) |
| GPIO-S02 | closed — obsolete | superseded by S01 |
| GPIO-S03…S05, S07 | closed (v1.1) | probe idiom, drvdata, include hygiene, IMR pair declared (§3) |
| GPIO-S06 | closed | whitespace-only retab (§3) |

---

## 5. Conclusion

No security findings: a thin gpiolib bank on the generic MMIO core, with
request-time pinmux + CNR as its only custom part, constant-mask syscon writes
and no unprivileged surface. The significant catch, GPIO-007, was closed in
`rtl8196e-eth` v2.7. Remaining open items are deliberate deferrals: GPIO-004
(no irqchip), GPIO-005 (no `valid_mask`, soft-mitigated) and GPIO-006
(`free()` is a no-op).
