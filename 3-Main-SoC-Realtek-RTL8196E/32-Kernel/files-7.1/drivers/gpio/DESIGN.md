# RTL8196E GPIO driver (`gpio-rtl819x`) — design

| | |
|---|---|
| **Document date** | 2026-06-11 (updated 2026-06-12: eth v2.7 closes GPIO-007; v1.2 generic-chip conversion) |
| **Driver version** | 1.2 (`DRV_VERSION` in `gpio-rtl819x.c`) |
| **Active release** | v4.4.0 (kernels `6.18.45` and `7.1.9`, `-rtl8196e-v4.4.0`); v1.1/v1.2 unreleased |

Architecture reference for the SoC GPIO bank driver. Findings and audit
history live in `AUDIT.md` (this directory).

---

## 1. Hardware model

One flat 32-line bank: four 8-bit ports (A–D) packed side-by-side into
single 32-bit registers — port A = bits 0–7 … port D = bits 24–31, so
gpiolib offset *n* is simply bit *n* everywhere:

| Offset | Register | Role |
|---|---|---|
| 0x00 | `PABCD_CNR` | function select per pin: 0 = GPIO, 1 = peripheral |
| 0x04 | `PABCD_PTYPE` | port type (untouched by the driver) |
| 0x08 | `PABCD_DIR` | direction: 0 = input, 1 = output |
| 0x0C | `PABCD_DAT` | data: read = pin state (inputs) / latch (outputs); write = output latch |
| 0x10 | `PABCD_ISR` | interrupt status (unused — no irqchip, GPIO-004) |
| 0x14 / 0x18 | `PAB_IMR` / `PCD_IMR` | interrupt masks (unused) |

The bank registers control logic only. For pads shared with other SoC
functions, **the pad-level routing lives elsewhere**, in the system
controller's mux registers — which is where the platform's real
complexity sits:

### The mux landscape (no pinctrl arbiter exists)

| Register | Fields relevant here | Writers (current kernel) |
|---|---|---|
| `PIN_MUX_SEL` (syscon 0x40) | UART1 TX/RX routing (bits 1,3,6), MII | `8250_rtl819x` (probe), `rtl8196e-eth` (`hw_init`) |
| `PIN_MUX_SEL_2` (syscon 0x44) | B2–B6 pad functions, 2-bit fields: `0b11` = GPIO, else LED_PORTx/peripheral (datasheet Table 36) | **this driver** (on `request()` of offsets 10–14), `rtl8196e-eth` v2.8 (`hw_init`: named-in-`gpio-line-names` → `0b11`, listed-in-`realtek,led-pads` → `0b00`, neither → `0b11` Hi-Z — the two writers agree by construction since GPIO-007 closed) |

All kernel writers go through the same `syscon` regmap, so individual
RMWs are atomic; what does **not** exist is an ownership model — the
registers are shared by convention. Since eth v2.7/v2.8 the convention
is DT-driven and the writers agree (GPIO-007 closed; both derive the
pad function from `gpio-line-names`/`realtek,led-pads` on this node).

## 2. Driver architecture

```
 consumers:  leds-gpio-pwm        uart-bridge (gpiod,        s40button v2
             (status-led, B3)     open-drain nRST, B4)       (cdev poll, "reset-button")
                  │                      │                         │
                  ▼                      ▼                         ▼
            ┌──────────────────── gpiolib core ───────────────────────┐
            │ offset validation · exclusivity · line-names from DT ·  │
            │ open-drain emulation (drive-0 / release-to-input)       │
            └──────────────┬───────────────────────┬─────────────────┘
                  .request │              .get/.set/.direction_*
            ┌──────────────▼─────────┐   ┌─────────▼─────────────┐
            │ this driver:           │   │ generic MMIO core     │
            │ 1. pinmux (B2–B6 only):│   │ (GPIO_GENERIC,        │
            │    syscon 0x44 field   │   │  gpio-mmio.c): shadow │
            │    → 0b11 (GPIO mode)  │   │ + raw-spinlock RMW on │
            │ 2. CNR bit ← 0 (GPIO)  │   │ DIR/DATA, multi-line  │
            │    (own spinlock)      │   │ ops for free          │
            └──────────────┬─────────┘   └─────────┬─────────────┘
                           ▼                       ▼
                 syscon regmap (0x44)       bank MMIO @ sysc+0x3500
```

### Key decisions

- **Generic MMIO core for the data path (v1.2, GPIO-S01).** DATA at
  0x0C / DIR at 0x08 (1 = out) is exactly the `gpio_generic_chip`
  register model, so the hand-rolled get/set/direction ops (~110 lines)
  are gone; the driver keeps only `.request` (pinmux + CNR, under its
  own spinlock) and probe wiring. The generic default
  `direction_output` is the *value-first* variant — same glitch-free
  DATA-before-DIR order the v1.x ops implemented by hand. Re-validated
  on hardware after the conversion: LED duty bands by DATA-register
  sampling, button via the s40button cdev path, and an nRST pulse
  answered by the EFR32's spontaneous ASH RSTACK.
- **Lazy, request-time muxing.** Pad routing for a shared line is fixed
  at the moment gpiolib hands the line out, not at probe. This is what
  makes the driver board-agnostic: a board where the button sits on B6
  (Sengled G4) needs *no driver change* — requesting the line muxes the
  pad. The board contribution is pure DT (`gpio-line-names`), which is
  the v3.10.0 generalization model (#122/#123).
- **`free()` is a no-op** (GPIO-006, deliberate): releasing a line
  leaves GPIO mode configured. Restore-on-free was rejected pending a
  policy — a naive rollback would break consumers that request, set up,
  and close (the cdev pattern).
- **No irqchip** (GPIO-004, deliberate): ISR/IMR exist in hardware but
  edge/level/ack semantics were never characterized; a wrong guess means
  IP-level interrupt storms. All current consumers poll or are outputs.
- **Dynamic numbering, names over numbers.** `gc.base = -1`; userspace
  finds lines by `gpio-line-names` through the cdev (`gpiofind`-style),
  in-kernel consumers via DT phandles. No global GPIO numbers anywhere.
- **Permissive validity** (GPIO-005, deliberate): hardwired pads
  (the ASIC-driven LAN-LED pad — B6 on the Lidl, B2 on the Sengled G4)
  are not masked out, only left unnamed in the DT so the supported
  lookup path cannot reach them.

### Open-drain without open-drain hardware

The EFR32 reset line must never be *driven* high (the EFR32 has its own
pull-up; driving 3.3 V into a held-low pin during its own POR is the
classic double-driver hazard). The chip has no native OD mode — instead
gpiolib's emulation does it with this driver's primitives:

- assert (low): `direction_output(0)` — pad sinks;
- release (high): `direction_input()` — pad floats, pull-up wins.

The DT flag (`GPIO_OPEN_DRAIN`) makes this transparent for the
uart-bridge's `nrst-gpios`; `direction_output` writing DATA *before* DIR
keeps the transition glitch-free (since v1.2 this is the generic core's
value-first variant — see invariant 3).

## 3. Board mapping (Lidl Silvercrest, `rtl8196e.dts`)

| Offset | Pad | Name | Consumer | Mux involvement |
|---|---|---|---|---|
| 9 | B1 | `reset-button` | `s40button` v2 (cdev poll) | none (not shared) |
| 10 | B2 | *(unnamed on Lidl, not in `led-pads` → `0b11` Hi-Z)* | G4: port-0 LAN LED (`led-pads = <10>` in its DTS) | `0x44[1:0]` — per `gpio-line-names`/`realtek,led-pads` |
| 11 | B3 | `status-led` | `leds-gpio-pwm` | `0x44[4:3]` — `0b11` on request *and* re-asserted by eth at every open (named) |
| 12 | B4 | `efr32-nrst` | `rtl8196e-uart-bridge` (gpiod, OD, active-low) | `0x44[7:6]` — `0b11` from boot via eth v2.7 (named): nRST floats high through the EFR32 pull-up before any claim; GPIO-007 closed |
| 13/14 | B5/B6 | B5 *(unnamed → `0b11` Hi-Z)*; B6 in `realtek,led-pads` → `0b00` = **LAN LED** (LED_PORT4, port 4 — #126: first shipped as B2, the dead LED was caught by eye) | G4: B5 = `blmode`, B6 = `reset-button` (named → `0b11`; its LAN LED is B2) | `0x44[10:9]`/`[13:12]` — per `gpio-line-names`/`realtek,led-pads` |

## 4. Context & concurrency model

- **UP SoC.** The generic core's `raw_spinlock_t` (IRQ-safe) serializes
  every DATA/DIR RMW, protecting process-context callers against
  IRQ-context consumers (LED triggers run from softirq;
  `can_sleep = false` advertises exactly that). The driver's own
  spinlock covers only the CNR RMW in `.request`.
- `.get` is a single 32-bit read — atomic by bus contract.
- The pinmux write runs *outside* the bank spinlock: the syscon regmap
  (`fast_io`, MMIO-backed) has its own lock, and nesting the two buys
  nothing (decision recorded in the GPIO-002 fix).
- Probe-only lifecycle: built-in (`=y`), devm-managed, no remove in
  practice, no suspend/resume (SoC has none).

## 5. External dependencies

| Dependency | Where | Role |
|---|---|---|
| `CONFIG_GPIO_RTL819X=y` | `config-6.18-realtek.txt` + gpio Kconfig/Makefile patches | builds the driver |
| `CONFIG_GPIO_GENERIC=y` | `select`ed by `GPIO_RTL819X` (Kconfig patch, v1.2) | the data-path ops (`gpio_generic_chip_init`) |
| `gpio0` node + `realtek,syscon` phandle | `rtl819x.dtsi` | bank MMIO window (0x3500, 0x38) + mux regmap |
| `&gpio0` board override + `gpio-line-names` | board DTS | enables the bank, names the wired lines |
| sysc syscon node (`0x0 0x1000`) | `rtl819x.dtsi` | covers 0x44 (unlike the 0x3100 timer block — that is why this driver *can* use regmap while the watchdog cannot) |
| `GPIO_CDEV=y` (v1 off), `GPIO_SYSFS_LEGACY` off | config | userspace surface = modern cdev only |
| `rtl8196e-eth` `hw_init` v2.7 | `drivers/net/ethernet/` | co-writer of 0x44, driven by this node's `gpio-line-names` (GPIO-007 closed) |
| Consumers | `leds-gpio-pwm`, `rtl8196e-uart-bridge`, `s40button` (userdata) | see §3 |

## 6. Invariants (do not break)

1. **PIN_MUX_SEL_2 has multiple writers and no arbiter.** Any new code
   touching `0x44` must use the shared syscon regmap (atomic RMW) *and*
   respect field ownership: a pad named in `gpio-line-names` belongs to
   a GPIO consumer and its field must read `0b11` (since eth v2.7 both
   writers derive this from the DT — GPIO-007 closed; keep it that way).
2. **Request-time muxing is the portability mechanism.** Do not move the
   B2–B6 mux to probe: boards differ in which shared pads they use, and
   probe-time muxing would steal pads from peripherals on boards that
   never request them.
3. **DATA before DIR in `direction_output`** — glitch-free contract
   relied on by the nRST open-drain emulation. Since v1.2 it is provided
   by the generic core's default (value-first) variant; **never pass
   `GPIO_GENERIC_NO_SET_ON_INPUT`** to the config — that flag selects
   the direction-first variant and silently breaks this contract.
4. **`can_sleep = false`** — every op must remain callable from atomic
   context (LED triggers). No sleeping regmap, no mutexes.
5. **No irqchip until ISR/IMR semantics are characterized** on real
   hardware (GPIO-004) — a guessed ack model risks an IP-line storm.
6. **Offsets are flat bits 0–31** across all registers; any future
   per-port code must preserve the bit = offset identity.
