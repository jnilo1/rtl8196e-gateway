# leds-gpio-pwm — cumulative production audit

> **Cumulative audit ledger.** The current-state table and finding table in
> the verdict are authoritative. Historical findings remain below as evidence
> and do not override their current dispositions.

| Current state | Authoritative value |
|---|---|
| **Current implementation** | `leds-gpio-pwm` v1.2 |
| **Release target** | firmware v4.0.0 |
| **Last audit pass** | 2026-07-29 — A1, full security/performance audit |
| **Last fully audited baseline** | v1.2 |
| **Post-baseline changes** | none |
| **Validation state** | static, `W=1`, checkpatch and DT-schema validation passed; target lifecycle/PWM gate pending |
| **Maintained kernels** | Linux 6.18 and 7.1, identical source |
| **Current finding registry** | verdict table below; LED-001…004 are historical fixes |
| **Scope** | driver, LED-core/timer/GPIO contracts, Kconfig/Makefile, DT, lifecycle and performance |
| **Companion** | `DESIGN.md` |

## Audit-pass ledger

| Pass | Date | Baseline | Result | Validation |
|---|---|---|---|---|
| A1 | 2026-07-29 | v1.1 → v1.2 | LED-005…009 fixed; LED-010 accepted | both kernels build/schema-clean; target acceptance pending |

## Verdict

The driver has no driver-specific userspace parser, DMA, allocation controlled
by userspace, or memory-safety exposure. Its arithmetic and resource ownership
are bounded, and the deployed RTL8196E GPIO is safe to access from timer
softirq context.

The v1.2 implementation complies with the atomic LED `brightness_set()`
contract and makes its GPIO/DT assumptions explicit. The findings discovered
by the 2026-07-29 review are resolved:

| ID | Severity | Status | Summary |
|---|---|---|---|
| LED-005 | medium | fixed in v1.2 | atomic callback no longer performs synchronous timer deletion |
| LED-006 | low | fixed in v1.2 | complete PWM state machine is protected by one IRQ-safe lock |
| LED-007 | low, dormant | fixed in v1.2 | sleep-capable GPIO descriptors are rejected at probe |
| LED-008 | low, dormant | fixed in v1.2 | kept-state read and direction errors are propagated |
| LED-009 | info | fixed in v1.2 | DT subset is documented and schema-validated |
| LED-010 | info | accepted | intermediate brightness forces 250 timer wakeups/s at `HZ=250` |

LED-001 through LED-004 from the first audit remain correctly fixed in v1.1.

## 1. Reviewed surface

The audit covers:

- both overlay copies of `drivers/leds/leds-gpio-pwm.c`;
- the 6.18 and 7.1 LED core callback and unregister contracts;
- the RTL8196E generic-MMIO GPIO provider;
- the Kconfig and Makefile patches;
- both production configurations and board DT nodes;
- the userspace/bridge path that selects brightness 0, 60 or 255;
- teardown, partial-probe unwind, timer rearming and `NO_HZ_IDLE` interaction.

The two driver sources, `DESIGN.md` files and pre-audit `AUDIT.md` files were
byte-identical between 6.18 and 7.1. The production configurations use
`LEDS_GPIO_PWM=y`, `HZ=250`, `NO_HZ_IDLE=y`, UP and `PREEMPT_NONE`.

## 2. Security and correctness properties

### 2.1 Attack surface

The only userspace interface is the standard LED-class sysfs API. The DT is
trusted firmware input and brightness is clamped by the LED core to 0..255.
There is no network-facing parser, ioctl, DMA, user pointer, variable-sized
copy, or attacker-controlled allocation. Findings below concern availability,
lifecycle correctness and portability rather than privilege escalation.

### 2.2 Properties verified correct

- **Bounded allocation and child iteration.** The allocation count comes from
  available DT children and the same available-child iterator fills the array.
  Early failure releases the current node and devres unwinds prior children.
- **Resource release order.** Each GPIO is acquired before its LED class device
  is registered. Devres therefore unregisters the class device before releasing
  that GPIO. LED unregister requests `LED_OFF`, which reaches the driver and
  stops the PWM timer in the currently shipped execution model.
- **Logical GPIO polarity.** `gpiod_set_value()` and `gpiod_get_value()` operate
  on logical values, so the active-low board declaration is handled correctly.
- **PWM arithmetic.** `(value * 4 + 127) / 255` cannot overflow and maps the
  LED-class range to five states: 0..31 off, 32..95 at 1/4, 96..159 at 2/4,
  160..223 at 3/4, and 224..255 continuously on.
- **No work at the rails.** Quantized 0/4 and 4/4 states stop the timer and
  drive a constant GPIO value. This includes values 1..31 and 224..254, not
  only the literal endpoints.
- **Timer-wheel rearm.** `mod_timer(..., jiffies)` is intentional. On this
  timer wheel, `jiffies + 1` is rounded into the following bucket and produced
  the observed 31 Hz flicker in issue #120. Four next-tick callbacks give a
  62.5 Hz PWM cycle at `HZ=250`.
- **GPIO callback context on RTL8196E.** The deployed GPIO controller is
  generic MMIO with `can_sleep == false`; a logical write from timer softirq is
  valid.
- **No hidden high-resolution dependency.** The implementation uses
  `timer_list`, not hrtimers, and remains compatible with the restored
  `NO_HZ_IDLE` clockevent configuration.

## 3. Findings resolved in v1.2

### LED-005 — synchronous timer deletion in an atomic LED callback

**Severity: medium (availability); dormant in the current trigger path.**

The v1.1 driver installed `gpio_pwm_brightness_set()` as
`cdev.brightness_set`.
The LED core explicitly defines this callback as non-sleeping and permits it to
run from atomic or hard-IRQ contexts. When brightness moves to either rail, the
callback called `timer_delete_sync()` if PWM was active.

The timer API explicitly forbids `timer_delete_sync()` from interrupt context
for a non-IRQ-safe timer. A hard IRQ can interrupt the PWM softirq while its
callback is running and then wait forever for the interrupted callback to
finish. On PREEMPT_RT the same API may also sleep. This is a contract violation
even though the current `uart-bridge-client` trigger fires from a worker and
the enabled heartbeat/netdev paths have not reproduced it.

**Resolution:** rail transitions now mark PWM inactive and call the
non-blocking `timer_delete()` under the driver state lock. The timer callback
checks the same state before driving or rearming. A devres action performs
`timer_shutdown_sync()` only during process-context teardown, after LED-class
unregister and before GPIO release.

### LED-006 — partially locked PWM state

**Severity: low; current UP/`PREEMPT_NONE` configuration contains the risk.**

In v1.1 only `threshold` was protected by `led->lock`. `pwm_active` and
`counter` were read or written lock-free by both paths. This
relies on single-CPU softirq ordering and is not a valid SMP/PREEMPT_RT state
machine. It also leaves a narrow stale-decision window if a hard-IRQ LED
producer changes brightness while the PWM callback is between its threshold
snapshot and rearm decision.

**Resolution:** threshold, counter, active state, GPIO writes, delete and
rearm decisions are now one spinlocked IRQ-safe state machine. No
`READ_ONCE()` workaround is used.

### LED-007 — sleep-capable GPIO descriptors are not rejected

**Severity: low, dormant on RTL8196E.**

The compatible and Kconfig text describe a generic GPIO LED driver. In v1.1,
probe did not check `gpiod_cansleep()`, while the PWM callback used
`gpiod_set_value()` from timer softirq. That is correct for `gpio-rtl819x` but
invalid for a GPIO expander whose setter may sleep.

**Resolution:** probe rejects a sleep-capable descriptor with
`-EOPNOTSUPP` and a precise diagnostic. There is no jitter-prone workqueue
fallback.

### LED-008 — ignored errors in `default-state = "keep"`

**Severity: low, dormant in the shipped DT (`default-state = "off"`).**

The v1.1 fix correctly requested `GPIOD_ASIS` before reading a kept line, but
did not check a negative return from `gpiod_get_value()` and ignored the return
from `gpiod_direction_output()`. A failed read was interpreted as logical ON
and a failed direction change still allowed LED registration.

**Resolution:** both errors are now propagated with child-specific probe
diagnostics, as in mainline `leds-gpio`.

### LED-009 — partial, unvalidated DT compatibility

**Severity: informational for the current boards.**

The v1.1 Kconfig help and source said the driver accepted the same child syntax
as `gpio-leds`, but there was no `gpio-leds-pwm` binding. The implementation
handles `gpios`, `default-state` and `linux,default-trigger`; it does not
implement the full `gpio-leds` lifecycle properties such as
`retain-state-suspended`, `retain-state-shutdown`, or `panic-indicator`.

**Resolution:** `leds-gpio-pwm.yaml` references the LED common binding, permits
the implemented subset and explicitly rejects unsupported lifecycle
properties. Source, Kconfig and design documentation now say
“gpio-leds-like supported subset”.

### LED-010 — intermediate brightness defeats tickless idle

**Severity: informational; accepted design cost.**

At brightness 32..223 the non-deferrable timer expires every jiffy: 250
callbacks and GPIO writes per second. The arithmetic CPU cost is small, but the
timer also prevents long `NO_HZ_IDLE` residency while PWM is active. In the
production policy this happens only while a service owns the STATUS LED in dim
mode (brightness 60); off and bright modes have zero timer cost.

Do not describe the cost only as a percentage of CPU cycles. Future power or
idle-latency validation must also record timer IRQ rate and idle residency.
This is not a reason to return to the 1 kHz hrtimer implementation, which
previously interfered with UART transfers.

## 4. Historical findings

| ID | Severity | Status | Resolution in v1.1 |
|---|---|---|---|
| LED-001 | low | fixed | `default-state = "keep"` requests `GPIOD_ASIS` before reading |
| LED-002 | info | fixed | quantization occurs once; 0/4 and 4/4 use constant GPIO levels |
| LED-003 | info | fixed | initial and steady-state arms both use `jiffies` |
| LED-004 | info | fixed | `DRV_VERSION`, boot banner and `MODULE_VERSION` added |

Related history: issue #120 identified the timer-wheel rounding that changed
the PWM cycle from 62.5 Hz to a visibly flickering 31 Hz. The LAN LED is not a
consumer of this driver; it is controlled by the Ethernet switch LED block as
documented in `DESIGN.md`.

## 5. Performance and simplification

- Intermediate brightness costs one timer callback and one GPIO RMW per jiffy,
  per active LED. The current board instantiates one software-PWM STATUS LED.
- Brightness at either quantized rail has no recurring timer cost.
- The redundant private brightness field and unused driver-data assignment
  were removed in v1.2.
- Keep the four-jiffy period unless a hardware PWM provider is introduced.
  Increasing resolution to eight jiffies recreates visible 31 Hz flicker at
  `HZ=250`.

## 6. Verification performed

- source and documentation parity checked between 6.18 and 7.1;
- forced `W=1` object rebuild completed cleanly on both maintained kernels with
  the Lexra toolchain;
- strict checkpatch: 0 errors, 0 warnings and 0 checks;
- production configs checked for `LEDS_GPIO_PWM=y`, `HZ=250`,
  `NO_HZ_IDLE=y`, UP and `PREEMPT_NONE`;
- DT consumers and the worker-based `uart-bridge-client` trigger path reviewed;
- the new binding passes `dt-doc-validate`, `dt_binding_check` and
  `dtbs_check` on both maintained kernel trees.

## 7. Production recommendation

The v1.2 source is statically ready for production on both maintained kernel
lines. Hardware acceptance still needs rapid 0/60/255 transitions while the
PWM timer is firing, trigger changes, module unbind/rebind and shutdown. The
accepted LED-010 wakeup cost should be measured separately from functional
correctness.
