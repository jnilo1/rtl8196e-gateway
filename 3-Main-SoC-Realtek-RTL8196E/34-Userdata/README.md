# User Data Partition

This directory contains the writable user partition for the gateway.

> **Board- and kernel-agnostic:** the same `userdata.bin` works for every board
> (`lidl`, `sengled-e39-g8c`) and kernel line (`6.18`, `7.2`). No `BOARD`/`KERNEL`
> selection applies here.

## Overview

Unlike the root filesystem (read-only SquashFS), the userdata partition uses **JFFS2** — a writable, wear-leveling filesystem designed for flash memory.

This partition is **essential** because:

1. The rootfs `/etc` symlinks most configuration files here
2. **All init scripts** are stored here (the rootfs only contains a bootstrap)
3. User applications and customizations live here

This design allows modifying configuration and adding services without rebuilding the read-only rootfs.

## Boot Integration

The rootfs bootstrap (`/etc/init.d/rcS`) mounts this partition at `/userdata`, then executes all scripts matching `/userdata/etc/init.d/S??*` in alphanumeric order.

```
Rootfs bootstrap
  ↓
Mount /userdata (JFFS2)
  ↓
Execute init scripts:
  S05syslog      → Start system logging
  S10network     → Configure network (DHCP or static)
  S11leds        → Apply LED brightness mode from leds.conf
  S15hostname    → Set hostname, /etc/hosts
  S20time        → Set timezone, start the ntpd daemon
  S25watchdog    → Feed the hardware watchdog
  S26panicrec    → Persist a kernel panic post-mortem left in reserved DRAM
  S30dropbear    → Start SSH server
  S40button      → Front-panel button daemon (long press → recover the radio)
  S50uart_bridge → Arm the in-kernel UART↔TCP bridge (skipped if radio mode = otbr)
  S70otbr        → Start Thread border router (if radio mode = otbr)
  S80netwatch    → Network-isolation watchdog (OFF unless enabled — see below)
  S90checkpasswd → Warn if default password
```

## Partition Structure

```
/userdata/
├── etc/
│   ├── passwd          # User accounts (with password hashes)
│   ├── group           # User groups
│   ├── hostname        # Gateway hostname
│   ├── profile         # Shell profile (TERM, TERMINFO, resize)
│   ├── motd            # Message of the day
│   ├── TZ              # Timezone
│   ├── ntp.conf        # NTP server configuration
│   ├── eth0.bak        # Static IP template (for reference)
│   ├── radio.conf      # Radio mode: MODE=otbr for Thread (absent = Zigbee)
│   ├── netwatch.conf   # Network-isolation watchdog (NOT shipped — create to enable)
│   ├── dropbear/       # Dropbear SSH host keys (generated on first boot)
│   └── init.d/         # Init scripts (executed by rootfs bootstrap)
│       ├── S05syslog
│       ├── S10network
│       ├── S11leds
│       ├── S15hostname
│       ├── S20time
│       ├── S25watchdog
│       ├── S26panicrec
│       ├── S30dropbear
│       ├── S40button
│       ├── S50uart_bridge
│       ├── S70otbr
│       ├── S80netwatch
│       └── S90checkpasswd
├── ssh/
│   └── authorized_keys # SSH public keys for passwordless access
├── netwatch/           # Incident snapshots + reboot budget (created once enabled)
├── panic/              # Kernel panic post-mortems (created by S26panicrec)
├── thread/             # Thread network credentials (created by otbr-agent)
└── usr/
    ├── bin/            # User applications (nano, otbr-agent, ot-ctl, boothold)
    └── share/
        └── terminfo/   # Terminal definitions (linux, vt100, vt102, xterm)
```

## Network Configuration

`flash_userdata.sh` asks for network configuration at flash time. The settings
are baked into `userdata.bin` before flashing.

To change the IP after flashing, edit `/userdata/etc/eth0.conf` on the running gateway:

```bash
vi /etc/eth0.conf
```

Format:
```
IPADDR=192.168.1.88
NETMASK=255.255.255.0
GATEWAY=192.168.1.1
```

Then restart the network: `/userdata/etc/init.d/S10network restart`

`/userdata/etc/eth0.bak` is provided as a reference template.

## Radio Mode

The gateway supports two radio modes, selected when the separate EFR32 is
flashed. **Mode is controlled by `/userdata/etc/radio.conf`**, which
`flash_efr32.sh` generates or updates automatically after a successful
application flash:

| Mode | `radio.conf` keys | Init script that wakes up | EFR32 firmware | Use case |
|------|---------|-------------|----------------|----------|
| **Zigbee** (default) | `FIRMWARE_BAUD=<baud>` (no `MODE`) | `S50uart_bridge` | NCP, RCP, or OT-RCP for ZoH/OTBR-host | Zigbee2MQTT, ZHA, OTBR-on-host |
| **Thread** | `FIRMWARE_BAUD=<baud>` + `MODE=otbr` | `S70otbr` | OT-RCP | Matter, Home Assistant Thread (OTBR-on-gateway) |

When `MODE=otbr` is present, `S50uart_bridge` is skipped and `S70otbr`
starts `otbr-agent` instead. When absent (Zigbee mode), `S50uart_bridge`
arms the in-kernel UART bridge at `FIRMWARE_BAUD`.

See `ot-br-posix/README.md` for Thread-specific documentation.

### radio.conf keys (full reference)

| Key | Values | Default | Written by | Read by |
|-----|--------|---------|------------|---------|
| `FIRMWARE` | `ncp`, `rcp`, `otrcp`, `router` | (absent) | `flash_efr32.sh` | docs / diagnostics |
| `FIRMWARE_VERSION` | e.g. `7.5.1` (NCP, Router only) | (absent) | `flash_efr32.sh` | docs / diagnostics |
| `FIRMWARE_BAUD` | `115200`, `230400`, `460800`, `691200`, `892857` | `460800` | `flash_efr32.sh` | `S50uart_bridge`, `S70otbr` |
| `FIRMWARE_FLOW_CTRL` | `none`, `sw`, `hw` | (absent ⇒ devicetree per-board default) | `flash_efr32.sh` — app flashes (#141) | `S50uart_bridge`, `S70otbr` |
| `BOOTLOADER_VERSION` | e.g. `2.4.2` | (absent) | `flash_efr32.sh` — every flash | docs / diagnostics |
| `MODE` | `otbr` (or absent) | (absent = Zigbee) | `flash_efr32.sh` | `S50uart_bridge`, `S70otbr` |
| `BRIDGE_BIND` | `0.0.0.0`, `127.0.0.1` | `0.0.0.0` | (manual) | `S50uart_bridge` |

> **Migrated from v3.0.x?** Older releases wrote two redundant host-side
> keys, `BRIDGE_BAUD` (Zigbee) and `OTBR_BAUD` (OTBR), in addition to
> `FIRMWARE_BAUD`. v3.2+ collapses them to the single `FIRMWARE_BAUD`
> truth. Both init scripts still fall back to the legacy keys when
> `FIRMWARE_BAUD` is absent, and the next `flash_efr32.sh` run strips
> them automatically — no user action needed.

`flash_efr32.sh` writes the right `MODE` and `FIRMWARE_BAUD` based on
the firmware you flash — manual editing is only needed for advanced
cases like OT-RCP in ZoH or OTBR-on-host modes (see
[`2-Zigbee-Radio-Silabs-EFR32/26-OT-RCP/docker/README.md`](../../2-Zigbee-Radio-Silabs-EFR32/26-OT-RCP/docker/README.md#switching-radio-mode-no-efr32-reflash-needed)).

#### `FIRMWARE` / `FIRMWARE_VERSION` / `FIRMWARE_BAUD` / `BOOTLOADER_VERSION` (v3.2+)

Four informational keys describing what's actually on the EFR32 — both
the application slot AND the Stage-2 Gecko Bootloader. They let an
offline reader (or a future migration script) tell exactly what's
running without probing the chip via `universal-silabs-flasher`.

* `FIRMWARE` — name of the app firmware in the EFR32's application slot:
  `ncp` | `rcp` | `otrcp` | `router`. **Never `bootloader`** — the
  Gecko Bootloader is a runtime mode, not an application. A bootloader-
  only flash leaves this key untouched (the existing app is still the
  one in the slot).
* `FIRMWARE_VERSION` — when the GBL filename embeds it (currently NCP
  and Router carry the EmberZNet version). Absent for RCP and OT-RCP
  — for those, the meaningful version lives host-side (`zigbeed` for
  RCP, `ot-br-posix` for OT-RCP).
* `FIRMWARE_BAUD` — the chip's UART baud as configured at last flash.
  Single source of truth: both `S50uart_bridge` (Zigbee) and `S70otbr`
  (OTBR) read this same key, since a working UART link forces both ends
  to the same baud. If you ever set it to something the chip isn't
  actually running at, the host-side daemons can't reach the chip — fix
  it by re-running `flash_efr32.sh` or by editing `radio.conf` to match.
* `BOOTLOADER_VERSION` — Gecko Bootloader Stage-2 version (e.g. `2.4.2`)
  as reported by `universal-silabs-flasher` during the last flash. Both
  bootloader-only and app flashes refresh this — USF transits the
  bootloader to upload either kind of GBL, and logs its version on the
  way through.

If the chip happens to be sitting in the Gecko Bootloader (empty or
corrupt application slot), `FIRMWARE` may be stale — the actual
runtime state is detected by `flash_efr32.sh`'s pre-flight probe.

#### `FIRMWARE_FLOW_CTRL` (v4.0.0, #141)

The chip's flow-control mode (`hw` | `sw` | `none`), written on every
application flash from the selected board's
`2-Zigbee-Radio-Silabs-EFR32/boards/<board>/board.env`
(`BOARD_UART_FLOW` — `hw` on the Lidl reference, `sw` on the Sengled G4).
`S50uart_bridge` writes it verbatim to the bridge's `flow_control` knob;
`S70otbr` omits the `uart-flow-control` parameter from the spinel URL for
`none`/`sw` — OpenThread treats that parameter as a presence flag, so any
value (even `false`) would enable CRTSCTS (#142).
When the key is absent (a config from before v4.0.0, never reflashed),
the bridge falls back to the devicetree per-board default and `S70otbr`
assumes `hw` — the case that used to require adding the key by hand on a
G4 running OT-RCP.

### Switching Radio Mode

To switch between modes on a running gateway:

**Thread → Zigbee:**
```bash
# Reflash EFR32 with NCP firmware (from your workstation)
./flash_efr32.sh -y ncp                    # gateway from gateway.env
# or: ./flash_efr32.sh -y -g 10.0.0.5 ncp  # custom IP

# That's it. The script stops otbr-agent if running, flashes the new
# firmware, writes FIRMWARE_BAUD=<baud> to /userdata/etc/radio.conf
# (no MODE= line → S50uart_bridge takes over instead of S70otbr),
# then reboots.
```

**Zigbee → Thread:**
```bash
# Reflash EFR32 with OT-RCP firmware
./flash_efr32.sh -y otrcp

# That's it. The script stops the bridge daemons, flashes OT-RCP,
# writes MODE=otbr + FIRMWARE_BAUD=460800 to radio.conf so S70otbr
# launches otbr-agent on next boot, then reboots.
```

> **Pre-v3.1 manual approach** (still works, no longer needed): edit
> `radio.conf` by hand before flashing. Since v3.1, `flash_efr32.sh`
> handles both the chip flash AND the gateway-side `radio.conf` rewrite
> in one shot.

Do not reflash the userdata partition just to change radio mode. Run
`flash_efr32.sh` so the EFR32 application and its host-side configuration are
updated together.

## SSH Passwordless Access

The `/userdata/ssh/authorized_keys` file allows SSH access without a password. Add your public key to this file:

```bash
# On your workstation, copy your public key
cat ~/.ssh/id_ed25519.pub

# Add it to authorized_keys on the gateway
echo "ssh-ed25519 AAAA... user@host" >> /userdata/ssh/authorized_keys
```

An RSA user key works just as well — the gateway stopped generating an RSA
*host* key, which is a different thing. `ssh-copy-id root@<gateway-ip>` does all
of this for whichever key you already have.

Dropbear is configured to read this file, enabling secure key-based authentication.

## Contents

| Directory/File | Description |
|----------------|-------------|
| `skeleton/` | Base structure for the user partition |
| `nano/` | GNU nano text editor build |
| `ot-br-posix/` | OpenThread Border Router build |
| `wireguard/` | WireGuard build **and** opt-in payload — NOT shipped, see its README |
| `build_userdata.sh` | Script to assemble and package the partition |

The Zigbee UART↔TCP bridge is now in-kernel (`rtl8196e-uart-bridge`, part of
the 6.18 kernel tree — see `../32-Kernel/files-6.18/drivers/net/rtl8196e-uart-bridge/`).
No userspace component to build here.

## Building

```bash
# Build nano (optional)
cd nano && ./build_nano.sh && cd ..

# Build otbr-agent (optional, for Thread mode)
cd ot-br-posix && ./build_otbr.sh && cd ..

# Assemble and package userdata
./build_userdata.sh
```

## Output

- `userdata.bin` — Flashable JFFS2 image with Realtek header (~12 MB)

## Included Applications

### nano

Lightweight text editor for editing configuration files directly on the gateway.

**Terminal support:** nano requires terminal capability definitions (terminfo) to display correctly. The `profile` sets up `TERMINFO` to point to `/userdata/usr/share/terminfo/` which includes definitions for common terminal types:

| Terminal | Use case |
|----------|----------|
| `linux` | Direct console access |
| `vt100` | Basic serial terminal |
| `vt102` | Minicom, PuTTY (VT102 mode) |
| `xterm` | SSH from modern terminals |

If your terminal emulator uses a different type, you can add the corresponding terminfo file to `/userdata/usr/share/terminfo/<first-letter>/<name>`.

**Terminal size:** The `profile` automatically runs `resize` at login to detect the terminal dimensions. This ensures nano and other curses applications display at the correct size.

### WireGuard — available, not shipped

**Neither the userdata image nor the shipped kernels carry WireGuard.** The
complete, working setup — `wg`, `wg-link`, `wireguardctl`, `S60wireguard` and
the config templates — lives in `wireguard/` and is installed by hand on the
gateways that want it.

The reason is throughput, not maturity: `CONFIG_WIREGUARD=y` costs **3.7 Mbit/s
of Ethernet TX** (about 5 %) on this SoC *while never executing*. The option
links ahead of the whole network stack and its ~71 KiB displace every hot
symbol downstream, which a 16 KiB I-cache and an 8 KiB D-cache pay for. Padding
the same link slot with inert bytes reproduces the loss exactly, and the
symbols WireGuard selects cost nothing on their own — so it really is the
placement. `CONFIG_WIREGUARD=m` is not an option: `CONFIG_MODULES` is off on
both kernel lines.

Charging every gateway 5 % of its TX for a feature that defaults to off was not
worth it. **`wireguard/README.md` carries the measurements and a step-by-step
install** — rebuild a kernel with the driver, build the tools, copy them into
`/userdata`, then configure. Note that a hand-install survives reboots but not
a userdata reflash.

### In-kernel UART↔TCP bridge (`rtl8196e-uart-bridge`)

An in-kernel driver (6.18 kernel, `CONFIG_RTL8196E_UART_BRIDGE=y`)
that exposes the Zigbee UART (`/dev/ttyS1`) over the network on
TCP:8888. Replaces the former userspace `serialgateway` daemon from
v3.0. Allows Zigbee2MQTT, Home Assistant ZHA, or other Zigbee
coordinators to communicate with the Silabs EFR32 radio remotely.

**Default configuration (writable via sysfs, armed at boot by
`S50uart_bridge`):**

| sysfs param | Default | Description |
|-------------|---------|-------------|
| `tty` | `/dev/ttyS1` | TTY device path (root only) |
| `baud` | `460800` via `FIRMWARE_BAUD` in `/userdata/etc/radio.conf` | UART baud rate |
| `port` | `8888` | TCP listen port (root only) |
| `bind_addr` | `0.0.0.0` | TCP bind address (root only) |
| `flow_control` | `1` | `0`/`none`, `1`/`hw` (RTS/CTS), `2`/`sw` (XON/XOFF) — set `0` for EFR32 flash; readback is numeric |
| `enable` | `0` → `1` by S50uart_bridge | 1=armed, 0=disarmed |
| `armed` | read-only | Actual bridge state |
| `stats` | read-only | Live rx/tx/drop counters |

All under `/sys/module/rtl8196e_uart_bridge/parameters/`. The table above
is the day-to-day subset; the full parameter list (`nrst_pulse`,
`nrst_gpio`, …), the device-tree seeding and the stats reference are in the
[bridge README](../32-Kernel/files-6.18/drivers/net/rtl8196e-uart-bridge/README.md).
Example:

```bash
# Change baud rate at runtime (no disarm needed)
echo 230400 > /sys/module/rtl8196e_uart_bridge/parameters/baud

# Read live counters
cat /sys/module/rtl8196e_uart_bridge/parameters/stats
```

**Usage with Zigbee2MQTT:**
```yaml
serial:
  port: tcp://<LINUX_IP>:8888
```

**Usage with Home Assistant ZHA:**
```
socket://<LINUX_IP>:8888
```

Source: `../32-Kernel/files-6.18/drivers/net/rtl8196e-uart-bridge/`.

### netwatch — network-isolation watchdog (`S80netwatch`)

**Shipped disabled. It reboots the gateway, so it is never switched on for you.**

The hardware watchdog armed by `S25watchdog` only catches a stopped CPU: the
feeder is a userspace process, so a kernel hang stops the kicks and the chip
resets the board. It cannot see the other way a gateway disappears — userspace
alive, network path dead. The kicks keep coming, the chip stays quiet, and the
box is simply gone from the LAN until someone power-cycles it.

That blindness costs more than the outage. The power cycle is itself the
evidence-destroying step: it clears the watchdog reset-reason latch and wipes
the reserved DRAM page holding any panic record, so `S26panicrec` never gets
the chance. A remote installation is left with an outage of unknown duration
and no cause at all.

`netwatch` probes targets with ICMP echo from the only vantage point that still
works in that state — the box itself. After a long continuous failure **with
the link carrier still up**, it writes a snapshot to
`/userdata/netwatch/incidents.log` and reboots. The snapshot is the point; the
reboot is the by-product. It is on JFFS2, so it survives the reboot, a later
power cycle, and everything else: reason, action taken, uptime, carrier, the
probe targets, `operstate`, `/proc/net/dev` counters, routes, the ARP table,
and the last 8 KB of the kernel ring — which is where the driver's own
`last reset:` line and any TX-timeout or switch-reset messages live.

**Enabling it.** Create `/userdata/etc/netwatch.conf` — the file is not shipped,
so its absence is what keeps the daemon inert:

```sh
cat > /userdata/etc/netwatch.conf <<'EOF'
ENABLED=1
TARGETS="192.168.1.1 192.168.1.10"
EOF
/userdata/etc/init.d/S80netwatch start
```

| Key | Default | Meaning |
|---|---|---|
| `ENABLED` | `0` | `1` arms the daemon; anything else leaves it installed and inert |
| `TARGETS` | default route | extra probe targets, space separated, max 8 |
| `FAIL_MIN` | `15` | continuous unreachable **minutes** before acting |
| `POLL_SEC` | `30` | probe period |
| `MAX_REBOOT` | `3` | reboots allowed per window |
| `WINDOW_H` | `24` | window for that budget |
| `DRY_RUN` | `0` | `1` records the decision and never reboots — same snapshot, no reboot |

**Who should enable it.** A gateway you cannot power-cycle yourself: remote,
unattended, or somewhere an unattended reboot is the lesser evil. On a box
within arm's reach, `DRY_RUN=1` gives the same forensic record without ever
rebooting.

Three behaviours worth knowing before you rely on it:

- **Carrier must be up.** Carrier down means the cable is out or the peer switch
  is off — a physical fault no reboot fixes, and one a human may have caused
  deliberately. The streak is held at zero so a maintenance unplug can never
  accumulate into a reboot.
- **The reboot budget fails closed.** It is persisted in `/userdata` so it
  survives the reboot it bounds. If it cannot be written, no reboot happens: an
  unbounded loop on unreachable hardware would be worse than the outage.
- **Every reset reads as `power-on / pin reset`,** never `watchdog timeout`.
  The driver only reports the latter when `WDIND` is set, and that bit does not
  survive the reset it flags — checked on the bench for both a panic-armed
  reset and a natural counter overflow. A netwatch reboot, a power cycle and a
  real watchdog bite all look alike on that line; only a panic record proves a
  watchdog recovery.

It only catches an isolated *network path*. A radio that stops answering while
Ethernet stays up is invisible to it — that is what the `keepalive` supervisor
around `otbr-agent` is for.

Source: `netwatch/src/netwatch.c`.

## Adding Custom terminfo

To add support for additional terminal types:

1. On a Linux system with the desired terminfo, locate the file:
   ```bash
   find /usr/share/terminfo /lib/terminfo -name "myterm"
   ```

2. Copy it to the gateway:
   ```bash
   cat /lib/terminfo/m/myterm | ssh root@<LINUX_IP> "cat > /userdata/usr/share/terminfo/m/myterm"
   ```

3. Set the terminal type:
   ```bash
   export TERM=myterm
   ```

## Adding Your Own Applications

Cross-compiling applications for the gateway is straightforward. Use the existing `build_*.sh` scripts as templates:

1. **Cross-compile** your application using the toolchain:
   ```bash
   # From project root:
   export PATH="$(pwd)/x-tools/mips-lexra-linux-musl/bin:$PATH"
   export CC=mips-lexra-linux-musl-gcc
   ./configure --host=mips-linux
   make
   ```

2. **Strip** the binary to reduce size:
   ```bash
   mips-lexra-linux-musl-strip myapp
   ```

3. **Transfer** to the gateway via SSH:
   ```bash
   cat myapp | ssh root@<LINUX_IP> "cat > /userdata/usr/bin/myapp"
   ssh root@<LINUX_IP> "chmod +x /userdata/usr/bin/myapp"
   ```

The application is immediately available — no reboot required.

## Adding Custom Init Scripts

To add a new service that starts at boot:

1. Create a script in `/userdata/etc/init.d/` with a name like `S70myservice`
2. Make it executable: `chmod +x S70myservice`
3. The script should accept `start`, `stop`, and optionally `restart` arguments

Example:
```bash
#!/bin/sh
case "$1" in
start)
    echo "Starting myservice..."
    /userdata/usr/bin/myservice &
    ;;
stop)
    echo "Stopping myservice..."
    killall myservice
    ;;
restart)
    $0 stop
    $0 start
    ;;
esac
```

The number prefix (S70) determines execution order — lower numbers run first.

## Mount Point

The userdata partition is mounted at `/userdata` by the rootfs bootstrap. The rootfs `/etc` symlinks point here, making this partition the source of truth for all configuration.
