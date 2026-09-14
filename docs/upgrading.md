# Upgrade an existing installation

This guide is for a gateway already running this project's Linux firmware. A
normal upgrade uses SSH, preserves persistent configuration, reboots into the
bootloader, and flashes over Ethernet. The serial adapter is not normally
required.

For a gateway still running stock Tuya firmware, use the
[first-installation guide](./getting-started.md).

## Before upgrading

Record the current state:

```bash
ssh root@<gateway-ip> '
  cat /proc/device-tree/model
  uname -r
  cat /userdata/etc/radio.conf 2>/dev/null || true
'
```

Then make a full backup:

```bash
./backup_gateway.sh --linux-ip <gateway-ip> \
  --output /path/outside/the/repository/pre-upgrade-backup
```

Confirm that the resulting `fullflash.bin` is 16,777,216 bytes. The upgrade
script preserves normal configuration, but a full backup also covers recovery
from interrupted power, a wrong image, or an unexpected flash failure.

The backup script also writes a small CRC trailer into the image (the last
16 bytes of the bootloader partition). Bootloader V3.1 and later check it before
writing anything and refuse a corrupted or modified image; older bootloaders and
older backups are unaffected. If a restore is refused with `CRC trailer
MISMATCH`, take a fresh backup, or re-sign a file you trust with
`lib/fullflash_crc.sh write fullflash.bin`.

> **Upgrading from firmware older than v3.0 with a non-default radio?** Stop
> here and read the
> [legacy radio migration notes](../3-Main-SoC-Realtek-RTL8196E/35-Migration/README.md#pre-v30--v3x--non-default-radio-configurations).
> Older images may not have `radio.conf`, so the upgrader cannot infer a custom
> RCP or OT-RCP choice automatically.

## Host prerequisites

**The machine running these scripts must be Linux.** The gateway's userdata image
is built with `mkfs.jffs2` from mtd-utils, which exists on Linux only, and the
scripts assume GNU tool behaviour and bash 4; they refuse to start anywhere else
rather than fail half-way through an upgrade. On macOS in particular an older
release would run for a while on bash 3.2 and stop mid-flow, after reporting
backup sizes that were not real.

If your computer runs macOS, or Windows without WSL2, run the scripts from a
Linux machine on the same network as the gateway — any Linux box will do,
including a Raspberry Pi, or a virtual machine whose network adapter is set to
**bridged** rather than NAT. The reason is that the bootloader is reached by ARP
and TFTP on the same network segment, which a NATed VM and a Docker Desktop
container cannot do. See the
[first-install guide](getting-started.md#3-prepare-the-computer) for the same
requirement stated in full.

From the repository root, install the small image-build and flash tool set:

```bash
sudo apt install fakeroot gcc mtd-utils squashfs-tools tftp-hpa \
  netcat-openbsd iproute2 iputils-ping openssh-client
```

The full cross-compilation environment is not needed because pre-built
bootloader and kernel images are included.

Set up an SSH key if the gateway will be maintained regularly:

```bash
ssh-copy-id root@<gateway-ip>
```

A root password or encrypted SSH key also works interactively. For unattended
password authentication, install `sshpass` and set `SSH_PASSWORD`; avoid
putting passwords in shell history or committed files.

## Standard upgrade

For a Lidl gateway using the production Linux 6.18 line:

```bash
./flash_install_rtl8196e.sh -y <gateway-ip>
```

For a Sengled Smart Hub G4:

```bash
BOARD=sengled-e39-g8c ./flash_install_rtl8196e.sh -y <gateway-ip>
```

The script performs the risky transitions only after it has prepared the image:

1. connects over SSH and identifies the running firmware;
2. checks the live board against the requested `BOARD`;
3. saves network, password, SSH keys, radio settings, Thread credentials, and
   additional files under `/userdata`;
4. builds and verifies the replacement 16 MiB image while Linux is still up;
5. runs `boothold`, reboots, and waits for the bootloader TFTP server;
6. uploads and flashes the image;
7. waits for the new Linux system to return.

Do not use `--force` to bypass a board mismatch unless you have independently
verified the devicetree and image. The full image includes a board-specific
bootloader with DRAM settings.

## LANs outside 192.168.1.0/24

On an upgrade you normally have nothing to do here. The upgrader reboots the
gateway into its bootloader and hands it an address, and with bootloader V2.7 or
newer that address is derived from the subnet of the machine you run the command
on. On a `192.168.0.0/24` LAN it therefore uses `192.168.0.6`, and no secondary
host address is needed. The chosen address is printed before anything is
touched.

Pass `--boot-ip` only to override that choice, for instance when the derived
address is already taken:

```bash
./flash_install_rtl8196e.sh -y \
  --boot-ip 192.168.0.7 <gateway-ip>
```

A first installation is the opposite case: the gateway is already sitting at a
bootloader that nothing can move, so it answers on its compiled-in
`192.168.1.6` whatever your LAN is. See the
[first-installation guide](./getting-started.md#9-prepare-the-ethernet-path).

The bootloader TFTP service must be on the same L2 segment as the computer. If
the selected address is reached through a router, the script stops before
rebooting the gateway and explains how to add a secondary host address.

Pre-V2.7 bootloaders ignore the IP handoff and still use `192.168.1.6`. See the
[install and upgrade reference](../3-Main-SoC-Realtek-RTL8196E/35-Migration/README.md#lan-not-on-the-1921681x-subnet)
for that case.

## Alternate kernel line

Linux 6.18 is the production default. Experienced users can select Linux 7.2:

```bash
KERNEL=7.2 ./flash_install_rtl8196e.sh -y <gateway-ip>
```

For Sengled, set both `BOARD=sengled-e39-g8c` and `KERNEL=7.2`. Kernel selection
does not change the EFR32 application.

## Verify the upgrade

After SSH returns:

```bash
ssh root@<gateway-ip> '
  cat /proc/device-tree/model
  uname -r
  uptime
  ip addr show dev eth0
  cat /userdata/etc/radio.conf
'
```

Also verify the external service that owns the radio:

- start Zigbee2MQTT or ZHA and confirm the coordinator reconnects;
- for native OTBR, run `ssh root@<gateway-ip> ot-ctl state`;
- confirm custom files expected under `/userdata` are still present.

If Linux is healthy but the radio does not reconnect, do not immediately
reflash the full system. Follow
[The EFR32 radio is unresponsive](./troubleshooting.md#the-efr32-radio-is-unresponsive).

## Updating only the EFR32 radio

Radio updates are independent of full Linux upgrades:

```bash
./flash_efr32.sh -y -g <gateway-ip> ncp
```

Choose the target using [Choose a radio mode](./radio-options.md). Stop the
current radio client first because the flasher needs exclusive access to TCP
port 8888.

## Developer-only component updates

`3-Main-SoC-Realtek-RTL8196E/flash_remote.sh` can update one partition through
SSH and `boothold`. It is useful while developing a kernel, rootfs, bootloader,
or userdata image, but it is not the normal release-upgrade path.

Use the full installer for ordinary upgrades because it preserves and combines
the complete system coherently. The per-partition commands and their data-loss
rules are documented in the
[install and upgrade reference](../3-Main-SoC-Realtek-RTL8196E/35-Migration/README.md).

## Recovery and rollback

If the upgrade stops before writing, a power cycle normally returns to the old
system. If it stops during or after a flash write, use the serial bootloader and
the verified backup. See [Backup and restore](../3-Main-SoC-Realtek-RTL8196E/30-Backup-Restore/README.md)
and [Troubleshooting](./troubleshooting.md).
