# LED management on the Silvercrest/Lidl Zigbee gateway (RTL8196E)

| | |
|---|---|
| **Last updated** | 2026-07-29 |
| **Driver version** | 1.2 (`leds-gpio-pwm.c`) |
| **Active release** | v4.4.0 (kernels `6.18.45` and `7.1.9`, `-rtl8196e-v4.4.0`) |

This is the design companion to [`AUDIT.md`](AUDIT.md) (code-level audit of
`leds-gpio-pwm.c`). It covers the hardware story both drivers share: the
software-PWM STATUS LED here, and the ASIC-wired LAN LED handled by the
Ethernet driver's `led_mode`.

## Hardware

The gateway has two front-panel LEDs, active-low, wired to RTL8196E
dual-function pads that can be routed to either the GPIO controller or
the switch ASIC LED controller via the `PIN_MUX_SEL_2` register
(0xB800_0044).

Source: RTL8196E-CG datasheet Rev 1.0, Table 3 (Shared I/O Pin Mapping)
and Table 36 (PIN_MUX_SEL_2).

| LED    | Label  | Pin | GPIO pad | LED function | PIN_MUX_SEL_2 bits | 00 = LED   | 11 = GPIO |
|--------|--------|-----|----------|--------------|---------------------|------------|-----------|
| LAN    | lan    | 116 | GPIOB[6] | LED_PORT4    | [13:12]             | LED_PORT4  | GPIOB6    |
| STATUS | status | 114 | GPIOB[3] | LED_PORT1    | [4:3]               | LED_PORT1  | GPIOB3    |

Default value at reset: **10b (Reserved)** — neither LED nor GPIO mode.
Software must explicitly write 00 (LED) or 11 (GPIO) after boot.

In the original firmware, both pads are set to 00 (ASIC LED mode).
Both LEDs share the same electrical characteristics and glow at the
same (fairly high) brightness.

## Original Lidl/Tuya firmware (Linux 3.10, vendor SDK)

The vendor BSP is based on Linux 3.10 but carries over several
drivers from the older 2.6.30 SDK (Ethernet, GPIO, ASIC layer).

All five shared pads (B2–B6) are routed to the switch ASIC LED
controller by the SDK's ASIC L2 init (`rtl865x_asicL2.c`):

```c
REG32(PIN_MUX_SEL2) &= ~((3<<0) | (3<<3) | (3<<6) | (3<<9) | (3<<12) | (7<<15));
//          all five LED fields (B2..B6) set to 0b00 = ASIC LED mode,
//          including B6 = LED_PORT4, the actual LAN-LED pad (#126)
```

A custom Tuya-specific kernel driver (`leds-rtl8196e.c` — not part of
the open Realtek SDK, source not available) exposed a procfs interface
for the STATUS LED:

    echo 1 > /proc/led1    # STATUS LED on
    echo 0 > /proc/led1    # STATUS LED off

This driver most likely controlled LED_PORT1 (B3) through the ASIC LED
controller registers rather than through GPIO, since the pin mux routes
B3 to the ASIC.  This is consistent with both LEDs having identical
brightness in the stock firmware.

### Switch ASIC LED controller

The RTL8196E switch core contains a dedicated LED controller at
register `LEDCREG` (0xBB804300).  During ASIC L2 init
(`rtl865x_asicL2.c`), the vendor code configures it as:

```c
REG32(PIN_MUX_SEL2) &= ~((3<<0) | (3<<3) | ...);  // pins → LED function
REG32(LEDCREG) = (2<<20) | 0;                       // LEDMODE_DIRECT, mode 0
```

- **`LEDMODE_DIRECT`** (bits 21-20 = 0b10): each LED pin is dedicated
  to one switch port — no scanning or multiplexing.
- **Mode 0 = Link / Activity**: the LED is **solidly ON** whenever the
  Ethernet link is up, and blinks off briefly during traffic.

Both pads (B6 for LAN, B3 for STATUS) are routed to the ASIC LED
controller via pin mux (`PIN_MUX_SEL2` bits set to 0b00).  The ASIC
drives the pins entirely in hardware with the same electrical
characteristics, which is why both LEDs glow at the **same high
brightness** — there is no GPIO toggling involved.

## Linux 5.10 port — the regression

When porting to Linux 5.10 with a clean device-tree based architecture,
the two LEDs were unified under the standard `gpio-leds` framework:

```dts
leds {
    compatible = "gpio-leds";
    status-led { gpios = <&gpio0 11 GPIO_ACTIVE_LOW>; };
};
```

This introduced two changes for the LAN LED (initially both LEDs were
declared as gpio-leds, but GPIO 10 has no effect on the LAN LED — see
"Hardware discovery" section below):

1. **Pin mux switched to GPIO mode.**  The GPIO driver
   (`gpio-rtl819x.c`) sets `PIN_MUX_SEL2` bits [1:0] to `0b11` when
   the pin is requested, disconnecting it from the ASIC LED controller.

2. **The `netdev` trigger replaced the ASIC controller.**  Instead of
   the hardware keeping the LED solidly ON with brief off-blinks, the
   software trigger does the opposite: the LED is **OFF by default**
   and flashes **briefly ON** for each TX/RX burst.

3. **`FULL_RST` in `rtl8196e_hw_init()` wipes `LEDCREG`.**  Even if
   the old ASIC driver had been compiled (it was not —
   `CONFIG_RTL819X` is disabled), the new Ethernet driver resets the
   entire switch core on `ndo_open`, clearing any prior LED register
   configuration.  No code re-programmes `LEDCREG` afterwards.

The net result: the LAN LED now has a **very low duty cycle** (~5 % ON)
and appears **visibly dim** compared to the STATUS LED — a clear
regression from the stock firmware where both LEDs matched.

Additionally, the `gpio-leds` driver only supports binary brightness
(0 or 1).  There is no way for users to adjust perceived brightness.

## The `leds-gpio-pwm` driver — rationale and design

### Goal

Provide user-adjustable brightness (0-255) for both LEDs, without
hardware PWM support (the RTL8196E has none), while keeping full
compatibility with standard LED triggers (`netdev`, `heartbeat`,
`default-on`, etc.).

### Approach

A platform driver (`compatible = "gpio-leds-pwm"`) that extends the
`gpio-leds` model with a **software PWM layer** based on `timer_list`:

- Each LED gets one kernel timer firing once per jiffy (250 Hz at HZ=250).
- PWM period = 4 jiffies → 62.5 Hz (above flicker threshold).
- `brightness_set(N)` adjusts the duty cycle to `N / 255`, quantized to
  the 4 physical levels the 4-jiffy window allows (25/50/75/100 %).
  Raising the resolution would lower the PWM frequency in the same
  ratio (8 levels → 31 Hz: visible flicker), so 4 levels at 62.5 Hz is
  the deliberate trade-off — see issue #120 and the driver header.
- In the quantized 0/4 and 4/4 bands the timer is stopped and the GPIO
  is held steady — no interrupt overhead for values 0..31 or 224..255.
- Existing triggers call `brightness_set()` as usual; the PWM layer is
  transparent.

Note: an earlier version used `hrtimer` at 1 kHz, but this caused LX bus
contention with UART during sustained Xmodem transfers (EFR32 flash).
The jiffies-based `timer_list` runs in softirq context and does not
interfere with UART interrupt handling.

### Requirements

- **`CONFIG_LEDS_GPIO_PWM=y`**: replaces `CONFIG_LEDS_GPIO`.
- The GPIO provider must be non-sleeping: PWM edges execute from timer
  softirq context. Probe rejects descriptors for which `gpiod_cansleep()`
  is true.

### Concurrency and teardown

`brightness_set()` is an atomic LED callback and may be called from hard IRQ.
It therefore never uses a synchronous timer deletion. Threshold, counter,
active state, GPIO writes and timer rearm decisions form one state machine
under an IRQ-safe spinlock. A rail transition marks PWM inactive and uses
`timer_delete()`; a callback already in flight observes the inactive state
under the same lock and cannot drive or rearm afterwards.

The blocking operation is confined to devres teardown. Its action is registered
before the LED class device, producing this release order:

1. LED class unregister requests `LED_OFF`;
2. `timer_shutdown_sync()` waits out and permanently disables the timer;
3. the GPIO descriptor and LED allocation are released.

### CPU overhead

On the RLX4181 (Lexra MIPS) at ~400 MHz, one LED toggling at 250 Hz
produces 250 timer callbacks per second.  Each callback is a single
GPIO register read-modify-write (~65 cycles).  Total: **~0.004 % CPU**.
The recurring timer also limits `NO_HZ_IDLE` residency while intermediate
brightness is active. Off and full-on quantized bands have no recurring timer.

### User interface

Standard Linux LED sysfs for the STATUS LED (the LAN LED is controlled
via `led_mode`, not via this driver — see "Dual brightness mode" below):

```sh
# Set STATUS LED to dim (matches LAN LED scan mode)
echo 60 > /sys/class/leds/status/brightness

# Full brightness (no PWM overhead)
echo 255 > /sys/class/leds/status/brightness

# Triggers work unchanged
echo heartbeat > /sys/class/leds/status/trigger
```

### DTS example

```dts
leds {
    compatible = "gpio-leds-pwm";

    status-led {
        label = "status";
        gpios = <&gpio0 11 GPIO_ACTIVE_LOW>;
        default-state = "off";
    };

    /* LAN LED is hardwired to switch ASIC — see section below */
};
```

### Files

| File | Role |
|------|------|
| `leds-gpio-pwm.c`                  | Driver source                          |
| `DESIGN.md`                        | This document                          |
| `AUDIT.md`                         | Dated code audit (finding IDs LED-*)   |
| `Documentation/devicetree/bindings/leds/leds-gpio-pwm.yaml` | Supported DT subset |
| `patches-6.18/drivers-leds-Kconfig.patch`| Adds `CONFIG_LEDS_GPIO_PWM` to Kconfig |
| `patches-6.18/drivers-leds-Makefile.patch`| Adds build rule to Makefile           |

## LAN LED — ASIC/GPIO mux on the board-declared pad

### The wrong-pad detour

Early bring-up assumed the LAN LED sat on pad B2 (pin 117, LED_PORT0 —
the first LED field in Table 36). Driving B2 as a GPIO was fully
functional electrically (mux `0b11`, `PABCD_CNR`/`DIR`/`DAT` all
responding to `gpiod_set_value()` and devmem) yet the physical LED
never changed, while `LEDCREG` writes did change it. That was misread
as "the LAN LED is hardwired to the ASIC LED_PORT0 output, bypassing
the pin mux".

The 2026-06-12 visual re-test (#126, prompted by hlyi's question about
the Table 36 1-1 port naming) falsified the bypass theory: the LED is
simply **not on B2**. It is on **pad B6 (pin 116) = LED_PORT4**,
matching the board's switch port (4) exactly as the datasheet's 1-1
naming suggests:

- B6 field `0b11` (unclaimed GPIO, Hi-Z) → LED dead even under traffic;
- B6 field `0b00` (LED function) → link/activity indication returns;
- B2 in any state → no effect on the LED (whatever B2 is wired to on
  the Lidl, it is not LED-related).

The pin mux is therefore fully effective on the LAN LED. The detour had
a real cost: eth v2.8 first shipped `realtek,led-pads = <10>` (B2) on
the strength of the wrong conclusion, and the floated B6 killed the
LAN LED until the value was corrected to `<14>` — a regression no
register readback could catch.

### What remains true

With the board pad in LED function the LED is driven entirely by the switch
ASIC LED controller and GPIO state is out of the path. This is B6/LED_PORT4 on
Lidl and B2/LED_PORT0 on Sengled G4. `LEDCREG` controls the two illuminated
modes:
- `LEDCREG = 0x0020_0000` (LEDMODE_DIRECT) → full brightness,
  link/activity
- `LEDCREG = 0x0000_0000` (scan mode) → dim blinking (~25 % of full)

### Why DIRECTLCR cannot provide true OFF

The `DIRECTLCR` register (0xBB80_4314, physical 0x1B80_4314) controls
the LED output scale in direct mode.  Its reset value is `0x1003FFFF`.

The former implementation treated `DIRECTLCR = 0` as output disable and also
cleared `LEDCREG`. A 2026-07-30 visual regression test disproved that model:
`LEDCREG = 0` selects scan topology (DIM), and `DIRECTLCR = 0` only selects the
minimum direct-mode duty scale, leaving a visible glow. Both combinations were
tested separately and neither made the physical Lidl LED dark.

The RTL819x SDK v3.4.7.3 definitions agree: topology zero is
`LEDMODE_SCAN`, while `DIRECTLCR` exposes an `LEDONSCALE` field rather than an
output-disable bit.

### True OFF through the pad mux

Driver v2.24 claims the board's active-low `lan-led-gpios` line and keeps its
GPIO latch at logical zero (physical high). For `bright` and `dim`, the driver
programs the ASIC registers and routes the pad to LED_PORTn (`0b00`). For
`off`, it routes the same pad to GPIO (`0b11`), where the preloaded high output
holds the LED fully dark. The sequence was visually validated on Lidl B6; the
same code derives the mux field from the Sengled G4 B2 GPIO specifier rather
than hard-coding either board.

### LED modes (bright / dim / off)

The `S11leds` init script reads `/userdata/etc/leds.conf` and configures
both LEDs at boot:

```sh
echo MODE=off > /userdata/etc/leds.conf     # all LEDs off
echo MODE=dim > /userdata/etc/leds.conf     # reduced brightness
echo MODE=bright > /userdata/etc/leds.conf  # full brightness (default)
/userdata/etc/init.d/S11leds start
```

The mode maps onto the two LEDs as follows — the LAN side is applied
by the Ethernet driver's `led_mode` store (ASIC registers plus pad mux),
the STATUS side by whichever service later takes ownership of the LED:
- **bright**: `DIRECTLCR = default`, `LEDCREG = LEDMODE_DIRECT`; STATUS PWM target 255
- **dim**: `DIRECTLCR = default`, `LEDCREG = 0` (scan mode); STATUS PWM target 60
- **off**: LAN pad muxed to its inactive GPIO output (physical high for the
  active-low LED); STATUS PWM target 0

`S11leds` only writes `led_mode` and forces the STATUS LED to 0 at
boot. It never lights the STATUS LED itself — doing so at boot used
to turn the LED on before any radio service was actually ready, which
operators rightly reported as confusing.

In Thread mode, the `otbr-monitor` housekeeping loop (started and
supervised by `S70otbr`) reads `/sys/class/net/eth0/led_mode` and sets
the STATUS LED to the matching brightness (255/60/0) once otbr-agent
reaches a ready state; `S70otbr stop` clears it.

In Zigbee mode the in-kernel UART bridge drives the STATUS LED via a
Linux LED trigger named `uart-bridge-client`. The bridge fires the
trigger at the configured brightness
(`/sys/module/rtl8196e_uart_bridge/parameters/status_led_brightness`,
default 255) when a TCP client connects, and clears it on disconnect
or on bridge disarm. `S50uart_bridge` binds the trigger to the STATUS
LED and picks the brightness from the current `led_mode` (bright → 255,
dim → 60, off → 0) before arming. This reproduces the pre-v3.0
behaviour where the STATUS LED tracked "Zigbee host connected".
