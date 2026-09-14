# Install and Upgrade Reference

This page is the detailed command and compatibility reference for
`flash_install_rtl8196e.sh`, full-flash assembly, and developer partition
updates.

For a guided workflow, start elsewhere:

- [First installation from stock firmware](../../docs/getting-started.md)
- [Upgrade an existing installation](../../docs/upgrading.md)
- [Troubleshooting](../../docs/troubleshooting.md)

## Choose the entry point

| Gateway state | Recommended command | Serial required? | Configuration preserved? |
| --- | --- | --- | --- |
| Stock gateway at `<RealTek>` prompt | `./flash_install_rtl8196e.sh` | Yes | No; new config is prompted |
| Stock Tuya Linux with known root access | `./backup_gateway.sh`, then enter serial bootloader and run installer without IP | Yes | Original backup only |
| Custom Linux running | `./flash_install_rtl8196e.sh -y <LINUX_IP>` | Normally no | Yes |
| Custom bootloader already waiting | `./flash_install_rtl8196e.sh` | Only to reach/observe prompt | No automatic import |
| One component under development | `3-Main-SoC-Realtek-RTL8196E/flash_remote.sh` | No with supported custom bootloader | Depends on component |

Commands in this page assume the repository root unless a `cd` is shown.

## Full installer

```bash
./flash_install_rtl8196e.sh [OPTIONS] [LINUX_IP]
```

Important options:

| Option | Purpose |
| --- | --- |
| `-y`, `--yes` | Skip interactive confirmations; intended for known upgrade flows |
| `--boot-ip <IP>` | Select the bootloader/TFTP address instead of `192.168.1.6` |
| `--force` | Override a clear board mismatch; dangerous and rarely appropriate |
| `--help` | Show current script syntax and environment variables |

The two top-level image selectors are:

| Variable | Values | Default |
| --- | --- | --- |
| `BOARD` | `lidl`, `sengled-e39-g8c` | `lidl` |
| `KERNEL` | `6.18`, `7.2` | `6.18` |

Examples:

```bash
# First install, Lidl, gateway already at the serial bootloader prompt
./flash_install_rtl8196e.sh

# Normal unattended upgrade
./flash_install_rtl8196e.sh -y 192.168.1.88

# Sengled G4 first install
BOARD=sengled-e39-g8c ./flash_install_rtl8196e.sh

# Sengled G4 upgrade
BOARD=sengled-e39-g8c ./flash_install_rtl8196e.sh -y <g4-ip>

# Alternate kernel line
KERNEL=7.2 ./flash_install_rtl8196e.sh -y <gateway-ip>
```

Do not select another board casually. A full image includes the bootloader, and
the bootloader's DRAM initialization is board-specific. On the SSH upgrade
path, the installer reads `/proc/device-tree/model` and refuses a known
mismatch unless `--force` is supplied.

## What the installer does

### First install or bootloader-only path

With no `LINUX_IP`, the script expects the bootloader TFTP service to be ready:

1. validates host packages and selected pre-built images;
2. verifies direct L2 reachability to `BOOT_IP`;
3. checks that a TFTP server is actually present;
4. prompts for DHCP/static networking;
5. builds a verified `fullflash.bin`;
6. uploads it over TFTP;
7. detects auto-flash behaviour or prints the stock/manual `FLW` command;
8. observes the available success channels and waits for reboot.

No running Linux configuration can be imported in this path. Back up first and
answer the network prompt deliberately. Radio selection is a separate second
step: once Linux is reachable, `flash_efr32.sh` installs the chosen EFR32
application and writes the matching `radio.conf` automatically.

### SSH upgrade path

With `LINUX_IP`, the script probes SSH and tests for the `boothold` capability:

1. authenticates once and reuses the SSH connection;
2. verifies the live board when the devicetree model is readable;
3. saves persistent configuration and additional userdata files;
4. injects them into a temporary copy of the new userdata skeleton;
5. builds and confirms the full image while Linux is still healthy;
6. calls `boothold <BOOT_IP>` and reboots;
7. waits for SSH to stop and for a real TFTP server to appear;
8. uploads, flashes, and waits for the new Linux image.

The saved set includes network configuration, MAC address, password database,
SSH keys and host keys, timezone/hostname, radio state, Thread credentials, and
files under `/userdata` that are not supplied by the new skeleton.

If `boothold` is unavailable — stock Tuya or very old custom firmware — the
script does not pretend it can enter the bootloader. It stops with serial
instructions. Nothing is written at that point.

## Authentication modes

An SSH public key is the simplest recurring setup:

```bash
ssh-copy-id root@<gateway-ip>
```

Interactive password or encrypted-key prompts also work. The script opens one
SSH ControlMaster session, so it should prompt only once.

For non-interactive password authentication:

```bash
sudo apt install sshpass
SSH_PASSWORD='<password>' ./flash_install_rtl8196e.sh -y <gateway-ip>
```

Avoid putting a real password in shell history, process listings, committed
files, or shared logs.

## LAN not on the 192.168.1.x subnet

The bootloader/TFTP address is `192.168.1.6` on a first install: the gateway is
already sitting at a bootloader, which answers on its compiled-in address and
cannot be told to move. That address does not follow your LAN. The computer must
reach it directly on the same L2 segment; a route through another gateway is not
sufficient.

On an upgrade the picture differs: `flash_remote.sh` and `flash_install_rtl8196e.sh`
reboot the gateway into the bootloader themselves and hand it an address, which
by default is derived from the subnet of the machine running them — so no extra
address is needed on a LAN that is not `192.168.1.x`.

With custom bootloader V2.7 or newer, choose an unused address on the local
subnet:

```bash
./flash_install_rtl8196e.sh -y \
  --boot-ip 192.168.0.6 192.168.0.88
```

`BOOT_IP=192.168.0.6` is the equivalent environment form; the command-line
flag takes precedence.

For a first install already at the stock bootloader prompt:

```bash
./flash_install_rtl8196e.sh --boot-ip 192.168.0.6
```

The old loader may still need this serial command before the script can reach
it:

```text
IPCONFIG 192.168.0.6
```

Pre-V2.7 `boothold` ignores the IP argument and returns at the compiled
`192.168.1.6`. Put the host on that subnet and retry without a custom address.

## Pre-v3.0 → v3.x: non-default radio configurations { #pre-v30--v3x--non-default-radio-configurations }

The upgrader preserves `/userdata/etc/radio.conf` when it exists. Firmware
before v3.0 normally used the old `serialgateway` daemon and may not have this
file. In that case, the migration seeds the historical default:

```text
FIRMWARE=ncp
FIRMWARE_BAUD=115200
```

That is correct for the normal v2.x NCP installation. If the old gateway runs
OT-RCP, RCP, or a custom NCP baud, create the truthful file before upgrading.

OT-RCP at 115200 with native OTBR:

```bash
ssh root@<gateway-ip> 'cat > /userdata/etc/radio.conf <<EOF
FIRMWARE=otrcp
FIRMWARE_BAUD=115200
MODE=otbr
EOF'
```

RCP at 115200:

```bash
ssh root@<gateway-ip> 'cat > /userdata/etc/radio.conf <<EOF
FIRMWARE=rcp
FIRMWARE_BAUD=115200
EOF'
```

Custom NCP at 460800:

```bash
ssh root@<gateway-ip> 'cat > /userdata/etc/radio.conf <<EOF
FIRMWARE=ncp
FIRMWARE_BAUD=460800
EOF'
```

From v3.0 onward, `radio.conf` is already present. New init scripts accept
legacy `BRIDGE_BAUD` / `OTBR_BAUD` as fallbacks, while the next successful
`flash_efr32.sh` run converges the file on `FIRMWARE_BAUD`.

## Non-interactive first-install variables

The following variables answer image-configuration prompts:

| Variable | Values / meaning |
| --- | --- |
| `NET_MODE` | `static` or `dhcp` |
| `IPADDR` | Static Linux address. Unset: an address in the subnet of the machine running the install, host part 88 |
| `NETMASK` | Static netmask. Unset: the netmask of that machine |
| `GATEWAY` | Static default gateway. Unset: that machine's default route |
| `CONFIRM=y` | Equivalent to `-y` |

Example:

```bash
NET_MODE=static IPADDR=192.168.1.88 \
  ./flash_install_rtl8196e.sh -y
```

Use this only after the gateway is deliberately placed at the bootloader prompt
and the chosen values have been reviewed. Automation does not remove the first
install's physical UART requirement.

## `build_fullflash.sh`

The installer calls this automatically. Direct use is mainly for inspecting or
distributing an assembled image:

```bash
# Lidl / Linux 6.18 (defaults)
./build_fullflash.sh

# Sengled G4 / Linux 6.18
BOARD=sengled-e39-g8c ./build_fullflash.sh

# Sengled G4 / alternate Linux 7.2 line
BOARD=sengled-e39-g8c KERNEL=7.2 ./build_fullflash.sh
```

| Partition | Offset | Input | Header handling |
| --- | --- | --- | --- |
| boot + config | `0x000000` | `31-Bootloader/boot-img/<board>/boot.bin` | Strip outer `cvimg` header |
| kernel | `0x020000` | `32-Kernel/kernel-img/<board>/kernel-<line>.img` | Keep kernel header |
| rootfs | `0x200000` | `33-Rootfs/rootfs.bin` | Strip outer `cvimg` header |
| userdata | `0x400000` | `34-Userdata/userdata.bin` | Strip outer `cvimg` header |

The builder verifies component sizes and the final 16 MiB layout.

## `flash_remote.sh` for developers

This script updates one RTL8196E partition through SSH, `boothold`, and TFTP:

```bash
cd 3-Main-SoC-Realtek-RTL8196E
./flash_remote.sh -y [--boot-ip <IP>] \
  <bootloader|kernel|rootfs|userdata> <LINUX_IP>

# Concrete example: update the Sengled G4 kernel with its 6.18 image
./flash_remote.sh -y --board sengled-e39-g8c kernel <g4-ip>
```

It requires a custom firmware/bootloader combination with `boothold`; it does
not convert stock firmware.

The userdata path preserves known configuration and carries forward additional
files that are not supplied by the new skeleton. A direct
`34-Userdata/flash_userdata.sh` does not provide the same deployed-system safety
and can wipe live configuration.

Individual `flash_*.sh` scripts expect the gateway to be in bootloader mode
already. Read the component guide before using them.

## `flash_efr32.sh` — Silabs EFR32 radio (OTA via SSH)

The separate radio is flashed after Linux is running:

```bash
./flash_efr32.sh -y -g <gateway-ip> ncp
```

The script handles chip reset, UART bridge flash mode, protocol probing,
Gecko-bootloader upload, persistent `radio.conf` state, and reboot. Firmware
selection belongs in [Choose a radio mode](../../docs/radio-options.md); the
low-level implementation is documented in the
[EFR32 backup/flash guide](../../2-Zigbee-Radio-Silabs-EFR32/22-Backup-Flash-Restore/README.md).

## Host prerequisites

For pre-built full-system images on Ubuntu:

```bash
sudo apt install fakeroot gcc mtd-utils squashfs-tools tftp-hpa \
  netcat-openbsd iproute2 iputils-ping openssh-client
```

`flash_efr32.sh` additionally needs `python3`, `python3-venv`, and `patch`; it
creates a pinned virtual environment and applies the repository's probe-methods
patch when needed. `xxd` is not required: both image and radio verification use
`od` from the base `coreutils` package.

The complete cross-compilation environment is separate and documented in
[Build Environment](../../1-Build-Environment/README.md).

## Failure and rollback rules

- A host prerequisite, board check, image build, confirmation, or TFTP-path
  failure before reboot leaves the running system untouched.
- A `boothold` failure before flash can normally be escaped with a power cycle.
- Do not cut power once a flash write has begun.
- A first boot loop after replacing a pre-V2.9 bootloader requires one cold
  power cycle.
- Restore a verified `fullflash.bin` from the serial bootloader when the new
  system cannot boot.

See [Backup and restore](../30-Backup-Restore/README.md) and the central
[troubleshooting guide](../../docs/troubleshooting.md).
