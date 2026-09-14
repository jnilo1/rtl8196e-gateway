# RTL8196E GPIO driver (`gpio-rtl819x`) — cumulative security audit

> **Cumulative audit ledger.** The current-state table and §4 registry are
> authoritative. Sections 1–5 primarily record the v1.2 audit pass; statuses
> written there are pass-scoped unless §4 or the ledger updates them.

| Current state | Authoritative value |
|---|---|
| **Current implementation** | `gpio-rtl819x` v1.3 |
| **Release target** | firmware v4.0.0 |
| **Last audit pass** | 2026-07-22 — A3, targeted diagnostic/probe-hygiene review |
| **Last fully audited baseline** | v1.2 (A2) |
| **Post-baseline changes** | v1.3 preserves the data path and improves syscon error fidelity, including `-EPROBE_DEFER`; reviewed in A3 |
| **Validation state** | v1.2 target gate passed; v1.3 independent review found no security/runtime defect |
| **Maintained kernels** | Linux 6.18 and 7.1, identical source |
| **Current finding registry** | §4 |
| **Audited surface** | driver, GPIO DT consumers/config, and cross-driver `PIN_MUX_SEL_2` writers |

## Audit-pass ledger

| Pass | Date | Baseline | Result | Validation |
|---|---|---|---|---|
| A1 | 2026-05-01 | pre-v1.1 | GPIO-001…006 established | historical |
| A2 | 2026-06-11/12 | v1.0 → v1.2 | GPIO-007/008 and simplification batch resolved or dispositioned | build plus LED/button/nRST target gate |
| A3 | 2026-07-22 | v1.2 → v1.3 | syscon error handling and diagnostic wording hardened; no runtime finding | targeted independent source review |

This audit **supersedes and replaces** the previous `AUDIT.md`
(2026-05-01, pre-v3.5.0). It is a fresh audit of the current code. Legacy
finding IDs GPIO-001…GPIO-006 are preserved in the registry at the end
(GPIO-003 is cited as the match-table convention from other drivers'
audits); their fixes/dispositions were re-verified against the current
v1.2 source. The v1.3 targeted follow-up is summarized in the ledger above.

Audit questions, per the project audit charter:

1. **Security** — does the driver introduce exploitable flaws?
2. **Simplification / optimization** — can the code be simplified or
   optimized for the Linux 6.18 APIs it targets?

---

## 1. Security audit

### 1.1 Attack surface

| Surface | Exposure | Assessment |
|---|---|---|
| `/dev/gpiochip0` (cdev v2 only — `GPIO_CDEV_V1` unset) | `crw------- root root` (0600) | Root-only. All offsets are validated by the gpiolib core (`offset < ngpio`) before any driver op runs; line claims are exclusive. |
| sysfs | `GPIO_SYSFS=y` but the deprecated export/unexport ABI is compiled out (`GPIO_SYSFS_LEGACY` unset) | Read-only class info; no line manipulation path. |
| Module parameters / ioctls / procfs | none | Driver adds zero interfaces beyond gpiolib. |
| syscon `0x44` writes | fixed masks/values selected by a `switch` on the (core-validated) offset | No user-controlled value ever reaches the regmap write. |

The driver parses no input, owns no buffers, does no DMA, and never
copies to/from userspace. **No trust boundary is crossed.** Root misuse
of a GPIO (e.g. toggling `efr32-nrst` by hand) is a root capability by
design, not a flaw.

### 1.2 Internal correctness re-verified (current code)

- **Locking** — all RMW sequences (CNR, DIR, DATA) run under
  `spin_lock_irqsave`; `.get` is a single lock-free `readl` (safe);
  `can_sleep = false` is honest (no sleeping calls in any op — the
  syscon regmap is MMIO-backed `fast_io`). Callable from atomic
  consumers (LED triggers) without violation. Verified.
- **Glitch-free output** — `direction_output` writes DATA before
  flipping DIR. Verified.
- **Open-drain consumers** — the chip has no native open-drain;
  gpiolib's emulation (drive-low = output-0, release = input) maps onto
  this driver's `direction_*` ops, which is exactly how the uart-bridge's
  `nrst-gpios` (`GPIO_ACTIVE_LOW | GPIO_OPEN_DRAIN`) is operated.
  Bench-verified by the discussion #121 nRST work. Verified.
- **Pinmux error propagation (GPIO-002)** — still present: a failed
  `regmap_update_bits` fails the `request()` with `dev_err`. Verified.
- **Match-table tightness (GPIO-003)** — single
  `realtek,rtl8196e-gpio` compatible; the PIN_MUX_SEL_2 field layout the
  driver writes is RTL8196E-specific, so the narrowing is load-bearing
  for the multi-board era. Verified.
- **No torn RMW on `0x44`** — every kernel writer of PIN_MUX_SEL_2
  (this driver, `rtl8196e-eth`) goes through the *same* syscon regmap,
  whose internal lock serializes read-modify-writes. The 8250 driver
  touches `0x40` only, and the uart-bridge delegated its B4 mux to this
  driver in v1.1. No data race — but see GPIO-007 for the *policy* race.

### 1.3 Security verdict

**No vulnerability found.** Root-only surface, core-validated inputs,
constant-mask syscon writes. The findings below are functional
robustness, not security.

---

## 2. New findings (this audit)

| ID | Type | Severity | One-liner |
|----|------|----------|-----------|
| GPIO-007 | FUNCTIONAL / CROSS-DRIVER | **medium** | *(closed by eth v2.7)* `rtl8196e-eth` re-cleared the B4/B5/B6 mux fields of `0x44` on **every `ndo_open`** — an `ifconfig eth0 down/up` after boot silently un-muxed GPIO lines this driver believes it owns (incl. `efr32-nrst`) |
| GPIO-008 | ROBUSTNESS | low | with the syscon absent, `request()` on a mux-requiring line (B2–B6) silently succeeds while the pad stays in peripheral mode — one probe-time warning is the only trace |

### GPIO-007 — last-writer-wins on PIN_MUX_SEL_2 across drivers

The mux fields this driver sets at `request()` time —
B4 `[7:6]`, B5 `[10:9]`, B6 `[13:12]` = `0b11` for GPIO mode — are
**cleared to 0** by `rtl8196e_hw_init()` in
`drivers/net/ethernet/rtl8196e-eth/rtl8196e_hw.c` ("clear MII/nRST-related
bits"), and that function is called from **`rtl8196e_open()`**, i.e. on
every interface up, not once at probe. The eth code deliberately
preserves B2 `[1:0]` / B3 `[4:3]` (the LED lines) but knows nothing about
B4–B6 ownership.

Boot ordering hides the problem: eth comes up first, the uart-bridge
requests GPIO 12 (`efr32-nrst`, pad B4) afterwards and re-muxes it. But
any later `ifconfig eth0 down && ifconfig eth0 up` (operator debugging, a
DHCP/network script) re-clears `[7:6]` while the bridge still holds the
line: gpiolib still shows the GPIO as owned and operations still write
DATA/DIR, yet the pad is electrically disconnected from the GPIO block.
First observable symptom would be `flash_efr32.sh` / bridge `nrst_pulse`
no longer resetting the radio — silent and far from the cause. On a
Sengled G4 port the same clobber hits B6 (`reset-button`), killing the
button after an eth flap.

There is no pinctrl subsystem on this platform; `0x40`/`0x44` are shared
by convention only.

**Recommendation** (primary fix lands in the eth driver — this finding
will be cross-referenced by the eth audit):

- *Minimal:* in `rtl8196e_hw_init()`, preserve any B4/B5/B6 field that
  currently reads `0b11` (GPIO mode) — i.e. only clear fields still in a
  peripheral state. Mirrors the existing B2/B3 preservation.

**Resolution (eth v2.7, v3.11.0-pre).** Closed in `rtl8196e-eth` v2.7 by
a stronger variant: ownership comes from the DT contract instead of the
current register value. `hw_init()` derives every B2–B6 field from this
driver's node `gpio-line-names` — named pad → `0b11` (GPIO), unnamed →
`0b00` (LED_PORTn). An eth flap now *re-asserts* the same `0b11` the
`request()` hook set, so held lines stay electrically connected, and
named on-demand lines (nRST, blmode) are GPIO-muxed deterministically
from boot. Residual (v2.7): a line claimed via the cdev *without* a DTS
name still got `0b00` on every open — closed by eth v2.8
(`realtek,led-pads` on this node): only declared LED pads get `0b00`,
every other unnamed pad is left `0b11` as unclaimed GPIO (Hi-Z), so an
anonymous cdev claim survives an eth flap too. The GPIO-006 interaction
note stands: any future
`free()`-time restore logic must stay consistent with the line-names
rule, not fight it.
- *Alternative:* move the eth mux write to probe-only (it corrects
  bootloader defaults; nothing re-breaks them at runtime).
- *Defense in depth (this driver):* re-assert the pinmux in
  `direction_*`/`set` is **not** recommended (hot-path regmap traffic);
  a cheap option is re-asserting in `request()` only, which is already
  the case — the gap is external clobbering of *held* lines, which only
  the eth-side fix closes.

### GPIO-008 — silent no-mux degradation when syscon is missing

`probe()` warns once ("no syscon, LED GPIOs may not work") and then
`configure_pinmux()` returns success-without-action for every B2–B6
request. A consumer of `status-led` (B3) or `efr32-nrst` (B4) gets a
fully-functional-looking GPIO whose pad never left peripheral mode. On
the production DT the syscon phandle always resolves, so this is a
DT-regression guard, not a field bug.

**Recommendation.** Return `-ENODEV` from `configure_pinmux()` for
offsets 10–14 when `rg->syscon` is NULL (with a per-call `dev_err`), so
a mis-wired board DTS fails loudly at the first consumer instead of
shipping a dead LED/button. Three lines.

**Resolution (v1.1).** Implemented as recommended: the syscon NULL check
moved after the offset switch, so only mux-requiring offsets (10–14) hit
the `-ENODEV` + `dev_err` path; everything else is unaffected. On the
production DT the path is never taken — bench boot is unchanged.

---

## 3. Simplification & optimization for kernel 6.18

The driver is already idiomatic 6.x where it counts: `devm_*` lifecycle,
`gpio_chip` with int-returning `.set` (the 6.x conversion), dynamic base
(`-1`), DT `gpio-line-names` consumed by the core, exact compatible. The
ops are single-register RMWs — nothing to optimize for speed. Candidates:

| ID | Item | Gain |
|----|------|------|
| GPIO-S01 | Convert get/set/direction ops to **`GPIO_GENERIC`/`bgpio_init()`** (`gpio-mmio`): DATA at 0x0C, DIR at 0x08 (1=out) is exactly the bgpio register model. Keep only the custom `.request` (CNR + pinmux). `CONFIG_GPIO_GENERIC=y` is already in the config — zero config cost | −~120 lines of hand-rolled RMW; `get_multiple`/`set_multiple` for free; bgpio's own spinlock replaces ours. *The one structural modernization* — schedule with hardware re-validation (LED, button, nRST pulse) |
| GPIO-S02 | (If not doing S01) drop the spinlock around the single `readl` in `get_direction` — reads don't race writes on a 32-bit MMIO register | micro |
| GPIO-S03 | `devm_platform_ioremap_resource(pdev, 0)` replaces the `platform_get_resource()` + `devm_ioremap_resource()` pair | −3 lines, canonical idiom |
| GPIO-S04 | Drop `platform_set_drvdata()` — never read back | dead line |
| GPIO-S05 | Drop `<linux/of_device.h>` (nothing used from it; the header is being dismantled upstream) and probably `<linux/of.h>` (no direct `of_*` call — pointer-only use of `of_node`) | include hygiene; verify with a build |
| GPIO-S06 | **Reformat to kernel style**: the file is indented with 4 spaces throughout, unlike every sibling driver (tabs); checkpatch flags ~all lines | consistency; do it as a standalone whitespace-only commit so functional diffs stay readable |
| GPIO-S07 | Align the IMR defines with the header comment: `REG_IMR 0x14` is PAB only; PCD_IMR at 0x18 is undeclared. Both unused until GPIO-004 — either declare the pair or drop the lone define | doc/code drift removal |

**Considered and rejected:** implementing `set_config` for open-drain —
the hardware has no OD mode and gpiolib's emulation is exactly right for
the single OD consumer (nRST); a fake native OD would change semantics
for no gain.

GPIO-S02…S05/S07 are safe to batch with the GPIO-008 fix (with a
`DRV_VERSION` bump). GPIO-S01 and GPIO-S06 each deserve their own commit;
S06 (whitespace) should precede any functional series.

**Implementation status (2026-06-12).** Landed in the audit's prescribed
order, three commits:

1. *S06* — standalone retab (tabs, continuation alignment fixed for
   tab=8); proven whitespace-only (`diff -w` empty, rebuilt `.o`
   byte-identical).
2. *v1.1* — GPIO-008 (+`-ENODEV`), S03 (`devm_platform_ioremap_resource`),
   S04 (drvdata dropped), S05 (`of.h`/`of_device.h` dropped,
   `mod_devicetable.h` added — build-verified), S07 (IMR pair declared:
   `PAB_IMR` 0x14 / `PCD_IMR` 0x18, matching the header comment).
   Bench: probe clean, LED + bridge nRST claim nominal.
3. *v1.2 (S01)* — `gpio_generic_chip` conversion. Note: 6.18 replaced
   the classic `bgpio_init()` with `gpio_generic_chip_init()` +
   `struct gpio_generic_chip_config` (`<linux/gpio/generic.h>`); the
   register model is the same (`dat` 0x0C, `dirout` 0x08, sz 4, no
   set/clr registers → shadowed-write variant). Verified before
   converting that the generic default `direction_output` is the
   *value-first* variant — preserving the glitch-free DATA-before-DIR
   contract (the dir-first variant is only selected by
   `GPIO_GENERIC_NO_SET_ON_INPUT`, which we must never pass).
   `GPIO_RTL819X` now `select`s `GPIO_GENERIC` (Kconfig patch). −67
   net lines; S02 (lock-free `get_direction`) obsoleted by the
   conversion. **Hardware re-validation gate passed** on the Lidl
   board: LED duty bands by DATA-register sampling (0 → 40/40 off,
   255 → 0/40, 128 → 21/40 ≈ 50 %), button via the s40button cdev path
   (line claimed, daemon running), nRST pulse answered by the EFR32's
   spontaneous ASH RSTACK (`1a c102029b7b 7e`) — the open-drain
   emulation drives through the generic direction ops.

---

## 4. Finding ID registry (complete)

Details of GPIO-001…GPIO-006 live in this repo's git history
(pre-v3.10.0 AUDIT.md). Dispositions re-checked 2026-06-11.

| ID | Status | One-liner |
|----|--------|-----------|
| GPIO-001 | closed (v3.4.0) | dynamic base (`-1`) instead of deprecated `base = 0` |
| GPIO-002 | closed (v3.4.0) | pinmux `regmap_update_bits` error propagated from `.request()` |
| GPIO-003 | closed (v3.4.0) | match table narrowed to `realtek,rtl8196e-gpio` (cited as the per-SoC convention by the wdt/clocksource audits) |
| GPIO-004 | open — deferred | no irqchip despite ISR/IMR registers. Update 2026-06-11: the original blocker ("no authoritative datasheet") is partially lifted (RTL8196E-CG datasheet now in hand, cf. Table 36 cites), but the use-case is still absent — `s40button` v2 polls via cdev at trivial cost and `efr32-nrst` is an output. Revisit only if an edge-triggered consumer materializes |
| GPIO-005 | open — deferred | no `valid_mask` for hardwired pins (B2/LAN-LED is ASIC-driven). Soft-mitigated since v3.10.0: `gpio-line-names` leaves such pads unnamed, so name-based lookups (the supported userspace path) cannot land on them |
| GPIO-006 | open — deferred | `free()` restores neither pinmux nor CNR — a B2–B6 line stays in GPIO mode after release. Policy decision still pending; note it interacts with GPIO-007 (any restore logic must not fight the eth driver's clobber) |
| GPIO-007 | **closed (eth v2.7)** | eth `ndo_open` re-cleared B4/B5/B6 mux fields under held GPIOs (§2) — fixed in `rtl8196e-eth` v2.7: 0x44 derived from `gpio-line-names` |
| GPIO-008 | **closed (v1.1)** | silent success of mux-requiring requests without syscon — now `-ENODEV` + `dev_err` (§2) |
| GPIO-S01 | **closed (v1.2)** | `gpio_generic_chip` conversion, HW re-validation gate passed (§3) |
| GPIO-S02 | closed — obsolete | superseded by S01 (the hand-rolled `get_direction` no longer exists) |
| GPIO-S03…S05, S07 | **closed (v1.1)** | probe idiom, drvdata, include hygiene, IMR pair declared (§3) |
| GPIO-S06 | **closed** | retab landed as a standalone whitespace-only commit, `.o`-identical (§3) |

---

## 5. Conclusion

No security findings — the driver is a thin, correctly-locked gpiolib
bank with constant-mask syscon writes and no unprivileged surface. The
audit's real catch was **GPIO-007**: the Ethernet driver's per-`open`
PIN_MUX_SEL_2 clear silently disconnected held GPIO lines (nRST, and the
G4 button) after any interface flap — **closed in `rtl8196e-eth` v2.7**
(v3.11.0-pre), which derives the 0x44 fields from `gpio-line-names`.

**Everything actionable is now implemented** (2026-06-12): GPIO-008 and
the S-batch in v1.1, the `gpio_generic_chip` conversion in v1.2 with its
hardware re-validation gate (LED, button, nRST — all passed). The driver
is down to its irreducible custom part: request-time pinmux + CNR.
Remaining open items are deliberate policy deferrals: GPIO-004 (no
irqchip until ISR/IMR semantics are characterized), GPIO-005 (no
valid_mask; soft-mitigated by unnamed lines), GPIO-006 (`free()` is a
no-op pending a restore policy).
