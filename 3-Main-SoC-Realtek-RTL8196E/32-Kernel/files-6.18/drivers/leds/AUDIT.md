# leds-gpio-pwm — cumulative production audit

> **Cumulative audit ledger.** The current-state table and the finding tables
> are authoritative. Closed findings are summarized; their full text is in the
> git history of this file.

| Current state | Authoritative value |
|---|---|
| **Current implementation** | `leds-gpio-pwm` v1.2 |
| **Audited code** | driver 1.2 (`DRV_VERSION` in `leds-gpio-pwm.c`) |
| **Last audit pass** | 2026-07-29 — A1, full security/performance audit |
| **Last fully audited baseline** | v1.2 |
| **Post-baseline changes** | none |
| **Validation state** | static, `W=1`, checkpatch and DT-schema validation passed; the target lifecycle/PWM gate (§5) was not run as a separate gate; the driver has shipped since v4.0.0 |
| **Maintained kernels** | Linux 6.18 and 7.2, identical source |
| **Current finding registry** | §2 (LED-005…010) and §3 (LED-001…004) |
| **Scope** | driver, LED-core/timer/GPIO contracts, Kconfig/Makefile, DT, lifecycle and performance |
| **Companion** | `DESIGN.md` |

## Audit-pass ledger

| Pass | Date | Baseline | Result | Validation |
|---|---|---|---|---|
| first audit | fixes landed 2026-06-12 | v1.0 → v1.1 | LED-001…004 fixed | historical |
| A1 | 2026-07-29 | v1.1 → v1.2 | LED-005…009 fixed; LED-010 accepted | both kernels (then 6.18 and 7.1) build/schema-clean; see §5 for target acceptance |

## 1. Verdict and reviewed surface

The driver has no driver-specific userspace parser, DMA, user pointer,
variable-sized copy or userspace-controlled allocation; the only userspace
interface is the standard LED-class sysfs API, with brightness clamped by the
LED core to 0..255, and the DT is trusted firmware input. Arithmetic and
resource ownership are bounded, and the deployed RTL8196E GPIO is safe to
access from timer softirq context. v1.2 complies with the atomic LED
`brightness_set()` contract and makes its GPIO/DT assumptions explicit.

A1 covered both overlay copies of the driver, the LED-core callback and
unregister contracts, the generic-MMIO GPIO provider, the Kconfig/Makefile
patches, both production configurations (`LEDS_GPIO_PWM=y`, `HZ=250`,
`NO_HZ_IDLE=y`, UP, `PREEMPT_NONE`), the board DT nodes, the userspace/bridge
path selecting brightness 0, 60 or 255, and teardown/partial-probe unwind,
timer rearming and `NO_HZ_IDLE` interaction.

Properties verified correct:

- **Bounded allocation and child iteration.** The count comes from available
  DT children and the same iterator fills the array; early failure releases the
  current node and devres unwinds prior children.
- **Resource release order.** Each GPIO is acquired before its LED class
  device is registered, so devres unregisters the class device (which requests
  `LED_OFF` and stops the timer) before releasing the GPIO.
- **Logical GPIO polarity.** `gpiod_set_value()`/`gpiod_get_value()` handle the
  active-low board declaration.
- **PWM arithmetic.** `(value * 4 + 127) / 255` cannot overflow and maps to five
  states: 0..31 off, 32..95 at 1/4, 96..159 at 2/4, 160..223 at 3/4, 224..255
  continuously on. The 0/4 and 4/4 rails stop the timer and drive a constant
  level (values 1..31 and 224..254 included).
- **Timer-wheel rearm.** `mod_timer(..., jiffies)` is intentional: `jiffies + 1`
  is rounded into the following bucket and produced the 31 Hz flicker of
  issue #120. Four next-tick callbacks give a 62.5 Hz cycle at `HZ=250`.
- **No hidden high-resolution dependency.** `timer_list`, not hrtimers;
  compatible with `NO_HZ_IDLE`.

## 2. Findings from A1 (v1.2)

| ID | Severity | Status | Summary and resolution |
|---|---|---|---|
| LED-005 | medium (availability; dormant in the current trigger path) | fixed in v1.2 | v1.1 called `timer_delete_sync()` from the atomic `brightness_set()` callback on a rail transition; a hard IRQ interrupting the PWM softirq could wait forever on it. Rail transitions now mark PWM inactive and call non-blocking `timer_delete()` under the state lock; the callback checks the same state before driving or rearming. A devres action runs `timer_shutdown_sync()` only in process-context teardown, after LED-class unregister and before GPIO release. |
| LED-006 | low | fixed in v1.2 | only `threshold` was locked; `pwm_active`/`counter` were lock-free. Threshold, counter, active state, GPIO writes, delete and rearm decisions are now one IRQ-safe spinlocked state machine. |
| LED-007 | low, dormant | fixed in v1.2 | sleep-capable GPIO descriptors were accepted although the timer softirq calls `gpiod_set_value()`. Probe now rejects them with `-EOPNOTSUPP`; no workqueue fallback. |
| LED-008 | low, dormant (shipped DT uses `default-state = "off"`) | fixed in v1.2 | `default-state = "keep"` ignored a negative `gpiod_get_value()` and the `gpiod_direction_output()` return. Both errors are now propagated, as in mainline `leds-gpio`. |
| LED-009 | info | fixed in v1.2 | the driver claimed `gpio-leds` syntax without a binding. `leds-gpio-pwm.yaml` now references the LED common binding, permits the implemented subset (`gpios`, `default-state`, `linux,default-trigger`) and rejects unsupported lifecycle properties (`retain-state-suspended`, `retain-state-shutdown`, `panic-indicator`). |
| LED-010 | info | accepted | brightness 32..223 runs the non-deferrable timer every jiffy (250 callbacks and GPIO writes/s at `HZ=250`) and limits `NO_HZ_IDLE` residency; in production only while a service holds the STATUS LED at dim (60). Record timer IRQ rate and idle residency, not just CPU share, in any power/idle validation. Not a reason to return to the 1 kHz hrtimer implementation, which interfered with UART transfers. |

## 3. Historical findings (fixed in v1.1)

| ID | Severity | Status | Resolution |
|---|---|---|---|
| LED-001 | low | fixed | `default-state = "keep"` requests `GPIOD_ASIS` before reading |
| LED-002 | info | fixed | quantization occurs once; 0/4 and 4/4 use constant GPIO levels |
| LED-003 | info | fixed | initial and steady-state arms both use `jiffies` |
| LED-004 | info | fixed | `DRV_VERSION`, boot banner and `MODULE_VERSION` added |

The LAN LED is not a consumer of this driver; it is controlled by the Ethernet
switch LED block (see `DESIGN.md`).

## 4. Performance

- Intermediate brightness costs one timer callback and one GPIO RMW per jiffy
  per active LED; the board has one software-PWM STATUS LED. The rails cost
  nothing recurring.
- Keep the four-jiffy period unless a hardware PWM provider appears: eight
  jiffies recreates visible 31 Hz flicker at `HZ=250`.

## 5. Verification

Performed in A1 (then on Linux 6.18 and 7.1): source/documentation parity
between the two lines; forced `W=1` object rebuild with the Lexra toolchain;
strict checkpatch clean (0 errors, warnings and checks); production configs
checked; DT consumers and the worker-based `uart-bridge-client` trigger path
reviewed; the binding passes `dt-doc-validate`, `dt_binding_check` and
`dtbs_check`.

A1 recommended a separate target acceptance: rapid 0/60/255 transitions while
the PWM timer fires, trigger changes, unbind/rebind and shutdown, with the
LED-010 wakeup cost measured separately. That gate was not run as a separate
gate; the driver has shipped since v4.0.0. `scripts/test_leds.sh` covers the
ordinary ON → DIM → OFF path visually, not unbind/rebind.
