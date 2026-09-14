# rtl8196e-uart-bridge

In-kernel UART ↔ TCP byte shoveler for the Realtek RTL8196E / Lexra
RLX4181 gateway. Replaces the former userspace `serialgateway` daemon.

## What it does

Bridges `/dev/ttyS1` (Silabs EFR32 Zigbee/Thread radio) to a TCP listen
socket (default `:8888`). A single client connects, and the driver
shovels bytes in both directions from inside the kernel:

```
                 +-------------------+
EFR32 radio <--> | UART1 (ttyS1)     |
                 |   |               |
                 |   | tty_port      |     rtl8196e-uart-bridge
                 |   | client_ops    |     (in-kernel hot path,
                 |   v               |      no ldisc, no userspace)
                 | TCP listen :8888  | <---> Z2M / cpcd / zigbeed / etc.
                 +-------------------+
```

No line discipline, no context switches on the hot path, no userspace
copy. The result is a clean up to 892857 baud (200 MHz ÷ 16 ÷ 14 — the N+1
divisor max for this SoC) with zero overrun under load.

Why a kernel module at all — and why the tty_port client_ops path
rather than a line discipline — is documented in [`DESIGN.md`](DESIGN.md).

## Sysfs interface

Exposed under `/sys/module/rtl8196e_uart_bridge/parameters/`. All knobs
are live: writing flips the running bridge without reload.

| File | R/W | Meaning |
|---|---|---|
| `tty` | rw, root | tty device path. Default `/dev/ttyS1` |
| `baud` | rw | UART baud. Applied live; take care matching EFR32 firmware |
| `port` | rw, root | TCP listen port. Default `8888` |
| `bind_addr` | rw, root | TCP bind address. Default `0.0.0.0`; set `127.0.0.1` for loopback-only |
| `flow_control` | rw | `0`/`none` = off (needed during EFR32 flash), `1`/`hw` = CRTSCTS (default), `2`/`sw` = XON/XOFF handled by the bridge. Readback is always numeric. An `hw` request is clamped to `sw` on a board without `realtek,hw-flow-control` (no RTS/CTS wiring) |
| `enable` | rw | 1 = arm the bridge, 0 = disarm. Boot default 0 |
| `armed` | ro | 1 when both UART and listen socket are live |
| `stats` | ro | `rx=... tx=... drops_nocli=... drops_err=... drops_tx=...` |
| `nrst_pulse` | wo, root | write 1 to pulse the EFR32 nRST line low for 100 ms (radio recovery — resets into the **application**) |
| `nrst_gpio` | rw | gpio-rtl819x line wired to EFR32 nRST. Default 12 (pad B4, Lidl gateway) |
| `blmode_pulse` | wo, root | write 1 to reset the EFR32 **into its bootloader**: assert `blmode_gpio`, pulse nRST 100 ms, hold blmode 1 s, release. `-ENODEV` when `blmode_gpio` is -1 |
| `blmode_gpio` | rw | gpio-rtl819x line wired to the EFR32 bootloader-entry pin. Default -1 = board has none (the Lidl board enters the bootloader in-band) |
| `status_led_brightness` | rw | 0-255 value fired on the `uart-bridge-client` LED trigger when a client connects (default 255; cleared on disconnect) |

Example — arm the bridge manually at 115200 on loopback:

```sh
SYSFS=/sys/module/rtl8196e_uart_bridge/parameters
echo 115200 > $SYSFS/baud
echo 127.0.0.1 > $SYSFS/bind_addr
echo 1 > $SYSFS/enable
cat $SYSFS/armed      # 1
```

The module loads at boot with `enable=0` and does nothing until something
(typically the init script below) arms it. This avoids racing the 8250
probe that creates `/dev/ttyS1`.

## Device tree configuration

Board-specific defaults can be described in an optional root node of the
board DTS, looked up by compatible at driver init (the bridge is not a
platform driver — no device binds to the node, and the unbound platform
device it produces under `/sys/devices/platform/` is harmless):

```dts
radio-bridge {
        compatible = "realtek,rtl8196e-uart-bridge";
        nrst-gpios = <&gpio0 12 (GPIO_ACTIVE_LOW | GPIO_OPEN_DRAIN)>;
        blmode-gpios = <&gpio0 13 (GPIO_ACTIVE_LOW | GPIO_OPEN_DRAIN)>;
        realtek,hw-flow-control;
};
```

- `nrst-gpios` — gpio-rtl819x line wired to the EFR32 nRST pad. Only the
  line number is consumed; the pulse path always claims the line
  active-low + open-drain, because EFR32 RESETn is inherently both. The
  DT cell flags should spell the same for the reader.
- `blmode-gpios` — gpio-rtl819x line wired to the EFR32 bootloader-entry
  pin, for boards that have one (e.g. the Sengled G4; the Lidl board does
  not and omits the property, which keeps `blmode_pulse` disabled). Same
  line-number-only consumption as `nrst-gpios`.
- `realtek,hw-flow-control` — a **board-capability** boolean: present means
  the board physically wires the EFR32 UART's RTS/CTS. It is *not* a
  firmware-mode selection. Present → the `flow_control` default is `hw`;
  absent → the default is `sw`, **and** an `hw` request (from sysfs or the
  init script) is clamped to `sw`, so CRTSCTS is never asserted on an
  unwired UART. The Lidl board sets it; the Sengled G4, which does not wire
  RTS/CTS, omits it. Choosing the runtime mode for a given radio firmware
  is a separate concern — see `FIRMWARE_FLOW_CTRL` below.

Precedence is **DT < kernel command line < runtime sysfs writes**: the
node seeds the boot-time defaults of `nrst_gpio` / `blmode_gpio` and the
hw-flow-control capability (which sets the `flow_control` default);
an explicit `rtl8196e_uart_bridge.nrst_gpio=...` on the command line or a
later sysfs write always wins (subject to the capability clamp on
`flow_control`). With no node the compiled-in Lidl defaults apply
(hw-capable) — third-party DTS files may simply omit it. This is the mechanism that lets RTL8196E-twin ports (e.g. the Sengled
G4, discussion #119) describe their radio wiring without patching the
driver.

## Boot / runtime integration

The userdata init script `S50uart_bridge`
(`3-Main-SoC-Realtek-RTL8196E/34-Userdata/skeleton/etc/init.d/S50uart_bridge`)
is the normal entry point. It reads these keys from
`/userdata/etc/radio.conf`:

```
FIRMWARE_BAUD=115200       # or 460800, 691200, 892857 — match the EFR32 firmware
BRIDGE_BIND=0.0.0.0        # or 127.0.0.1 to force SSH-tunnel-only access
FIRMWARE_FLOW_CTRL=hw      # none|sw|hw, overrides the DT per-board default
```

then writes the corresponding sysfs knobs and flips `enable=1`. When
`MODE=otbr` is set in the same file, the script exits early and leaves
the UART free for `otbr-agent`.

All three keys are optional — missing `FIRMWARE_BAUD` defaults to 460800,
missing `BRIDGE_BIND` defaults to 0.0.0.0 (unchanged from v3.0 behaviour),
and a missing `FIRMWARE_FLOW_CTRL` leaves `flow_control` at its devicetree
per-board default. Since #141, `flash_efr32.sh` writes `FIRMWARE_FLOW_CTRL`
on every application flash from the selected board's `board.env`, so on a
script-flashed gateway the key is present and matches the flow-control mode
the firmware was built with. A hand-edited value (e.g. an XON/XOFF NCP
build flashed via `--firmware-file`) is honoured until the next app flash
resets it, and is always subject to the hw-flow-control capability clamp.

`FIRMWARE_BAUD` is the chip-side baud written by `flash_efr32.sh` on
every successful flash; both `S50uart_bridge` (Zigbee) and `S70otbr`
(OTBR) read this same key, since a working UART link forces both ends
to the same baud. `radio.conf` may also carry the related chip-identity
keys (`FIRMWARE`, `FIRMWARE_VERSION`). The kernel driver itself reads
none of those — they are operator-facing metadata, see
[`34-Userdata/README.md`](../../../../../34-Userdata/README.md#radioconf-keys-full-reference)
for the full reference.

## Stats and observability

`/sys/module/rtl8196e_uart_bridge/parameters/stats` tracks eight counters:

- `rx` — bytes forwarded UART → TCP (radio → host)
- `tx` — bytes forwarded TCP → UART (host → radio)
- `drops_nocli` — UART bytes dropped because no TCP client is connected
- `drops_err` — UART bytes dropped because `kernel_sendmsg()` failed
- `drops_tx` — TCP bytes dropped because the tty->write was short
- `xoff` / `xon` — bare XOFF/XON bytes received from the radio
  (`flow_control=sw` only; both stay 0 in hw/none modes)
- `tx_pause_timeouts` — sw mode: a radio XOFF stayed unanswered for
  more than 1 s and the bridge failed open

Non-zero `drops_err` or `drops_tx` in steady state points to TCP
backpressure or tty congestion, respectively. `drops_nocli` is normal
any time Z2M (or whichever client) is not connected. In sw mode,
`xoff`/`xon` incrementing with `drops_tx` staying 0 is the healthy
signature; non-zero `tx_pause_timeouts` means the radio stopped
draining its UART for over a second (stuck firmware, baud mismatch).

Combined with the 8250 framing/overrun counters in
`/proc/tty/driver/serial` you get a complete picture of both the
wire and the bridge.

## STATUS LED — client connected indicator

When a TCP client is connected the bridge fires the
`uart-bridge-client` LED trigger at the configured brightness; it
clears the trigger on disconnect. This reproduces the pre-v3.0
serialgateway behaviour where the STATUS LED tracked "Zigbee host
connected".

Bind the trigger to the physical LED once (done by `S50uart_bridge`
at boot based on `eth0/led_mode`):

```sh
echo uart-bridge-client > /sys/class/leds/status/trigger
echo 255 > /sys/module/rtl8196e_uart_bridge/parameters/status_led_brightness
```

Set `status_led_brightness` to 0 to disable the LED behaviour without
touching the trigger wiring.

## EFR32 nRST recovery

`nrst_pulse` recovers a stuck radio (crashed app, livelock, corrupted
state) without rebooting the SoC: it claims the GPIO line named by
`nrst_gpio` as an open-drain output, drives it low for 100 ms, then
floats it back to input — the EFR32's internal RESETn pull-up releases
the chip into a clean boot. Stop the radio daemon first and restart it
after; the `recover_efr32` helper script wraps exactly that sequence.

On the Lidl gateway nRST is wired to pad B4 = line 12 on the
`gpio-rtl819x` chip (the default). Ports to other RTL8196E boards set
`nrst_gpio` to their own line at boot — note that the GPIO driver only
auto-muxes pads B2–B6 (lines 10–14, PIN_MUX_SEL_2); a line outside that
range needs its pad mux established separately.

## EFR32 bootloader entry (`blmode_pulse`)

Boards that wire the EFR32 bootloader-entry pin to a SoC GPIO (the
Sengled G4 does; the Lidl board does not) can reset the radio **into the
Gecko bootloader** with `blmode_pulse`: it asserts `blmode_gpio`, pulses
nRST as above, then keeps blmode asserted for 1 s across the bootloader's
pin-sampling window before releasing it. This is the first-flash path on
boards whose stock radio firmware speaks no EZSP (no in-band
bootloader-launch command). It is deliberately separate from
`nrst_pulse`, which keeps meaning "reset into the application" — that is
what `flash_efr32.sh` and `recover_efr32` rely on after a flash. The
sysfs write blocks for the ~1.1 s of the sequence; afterwards the chip
sits in the Gecko bootloader (Xmodem @ 115200, no flow control) until the
next `nrst_pulse` or a bootloader-driven application launch.

## Flashing the EFR32 with the bridge armed

`flash_efr32.sh` (at the repo root) talks to the bridge directly and
handles the EFR32 firmware flash without disarming it. It flips
`flow_control` to 0 for the Gecko Bootloader Xmodem transfer and
restores it to 1 afterwards. No manual teardown required.

`flow_control=0` matters in **all** modes during a flash: Xmodem
payloads contain raw 0x11/0x13 bytes, so leaving sw mode on would make
the bridge eat them as XON/XOFF and corrupt the transfer.

## Security

The bridge is a plaintext single-client TCP listener. Any host that can
reach `gateway:8888` can talk EZSP/CPC/Spinel directly to the Zigbee
radio with no authentication. For anything beyond a fully-trusted LAN,
set `BRIDGE_BIND=127.0.0.1` and reach the bridge through an SSH tunnel
from the host running Z2M/cpcd/otbr.

Full threat model, gateway-side hardening, `autossh` + systemd recipe,
and verification steps: [`SECURITY.md`](SECURITY.md).

## Files in this directory

| File | Role |
|---|---|
| `rtl8196e_uart_bridge_main.c` | driver source (~1800 lines, single file) |
| `Kconfig` / `Makefile` | in-tree build glue |
| `README.md` | this file |
| `DESIGN.md` | rationale + design notes (what was built and why) |
| `SECURITY.md` | SSH-tunnel deployment and threat model |
| `AUDIT.md` | dated security / simplification audit (finding IDs BRIDGE-*) |

The driver header comment at the top of `rtl8196e_uart_bridge_main.c`
is the authoritative reference for the sysfs knobs and their runtime
semantics — anything in this README that drifts from it should be
treated as a documentation bug.
