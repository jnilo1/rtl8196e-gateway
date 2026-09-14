# RTL8196E Linux System

This subtree contains everything that runs on the gateway's main Realtek
RTL8196E processor: bootloader, Linux kernel, read-only root filesystem,
persistent userdata, and install/upgrade tooling.

This is a component reference, not the first-install walkthrough:

- new gateway: [First installation](../docs/getting-started.md);
- existing installation: [Upgrade guide](../docs/upgrading.md);
- day-to-day administration: [Using the gateway](../docs/using-the-gateway.md);
- failures: [Troubleshooting](../docs/troubleshooting.md).

## Role in the gateway

The RTL8196E owns Ethernet, persistent storage, Linux services, and the host
side of the EFR32 radio link:

```text
Zigbee2MQTT / ZHA / host services
                 |
              Ethernet
                 |
        RTL8196E Linux system
        |                    |
  TCP UART bridge       native otbr-agent
        |                    |
        +-------- UART1 -----+
                 |
         Silabs EFR32 radio
```

In normal Zigbee mode, the in-kernel `rtl8196e-uart-bridge` exposes UART1 on
TCP port 8888. In native Thread mode, `otbr-agent` owns UART1 directly instead.
`/userdata/etc/radio.conf`, maintained by `flash_efr32.sh`, selects the service,
baud, and flow-control state.

## Flash layout

The supported gateways use a 16 MiB SPI NOR flash:

| Offset | Size | Partition | Purpose |
| --- | --- | --- | --- |
| `0x000000` | 128 KiB | boot + config | Board-specific Realtek bootloader |
| `0x020000` | 1.875 MiB | linux | Board/kernel-specific Linux image |
| `0x200000` | 2 MiB | rootfs | Read-only SquashFS base system |
| `0x400000` | 12 MiB | userdata | Writable JFFS2 configuration and apps |

The top-level `build_fullflash.sh` assembles these four partitions into a
verified 16 MiB image. `flash_install_rtl8196e.sh` is the normal full-system
install and upgrade entry point.

## Component map

| Directory | Responsibility | Read this when |
| --- | --- | --- |
| [30-Backup-Restore](./30-Backup-Restore/README.md) | Full-flash backup, split, restore | Before flashing or during recovery |
| [31-Bootloader](./31-Bootloader/README.md) | DRAM bring-up, TFTP, auto-flash, `boothold` | Working on boot or recovery |
| [32-Kernel](./32-Kernel/README.md) | Linux 6.18/7.2, devicetree, platform drivers | Building or modifying Linux |
| [33-Rootfs](./33-Rootfs/README.md) | BusyBox, Dropbear, read-only base | Modifying core userspace |
| [34-Userdata](./34-Userdata/README.md) | Persistent config, init scripts, applications | Operating or extending services |
| [35-Migration](./35-Migration/README.md) | Script and compatibility reference | Advanced install/upgrade cases |

## Supported image matrix

The full image contains board-specific bootloader and kernel files:

| `BOARD` | Hardware | Kernels |
| --- | --- | --- |
| `lidl` (default) | Lidl Silvercrest / Tuya reference board | 6.18 default, 7.2 alternate |
| `sengled-e39-g8c` | Sengled Smart Hub G4 | 6.18 default, 7.2 alternate |

Rootfs and userdata are shared across boards and kernel lines. Never flash a
full image for the wrong board: the bootloader initializes DRAM with
board-specific values. Upgrade scripts compare `BOARD` with the live
devicetree model and refuse a clear mismatch.

Examples:

```bash
# Normal Lidl install/upgrade image
./flash_install_rtl8196e.sh <gateway-ip>

# Sengled G4
BOARD=sengled-e39-g8c ./flash_install_rtl8196e.sh <gateway-ip>

# Alternate kernel line
KERNEL=7.2 ./flash_install_rtl8196e.sh <gateway-ip>
```

Run those commands from the repository root. A stock gateway requires the
hardware and bootloader procedure described in the
[first-install guide](../docs/getting-started.md).

## Normal installation and upgrade path

### Full system

Use `flash_install_rtl8196e.sh` for ordinary work. It builds the complete image,
preserves configuration on upgrades, performs board checks, reboots through
`boothold` when available, and flashes by TFTP.

```bash
# Gateway already running custom firmware
./flash_install_rtl8196e.sh -y <gateway-ip>
```

Without a gateway IP, the command expects a first-install gateway already at
the `<RealTek>` serial bootloader prompt.

### One partition

`flash_remote.sh` and each component's `flash_*.sh` are developer tools.
`flash_remote.sh` uses SSH and `boothold`; the individual scripts expect the
gateway to be in bootloader mode already.

```bash
cd 3-Main-SoC-Realtek-RTL8196E
./flash_remote.sh -y kernel <gateway-ip>
./flash_remote.sh -y rootfs <gateway-ip>

# Sengled G4 kernel partition: selects and verifies the G4 image
./flash_remote.sh -y --board sengled-e39-g8c kernel <g4-ip>
```

A raw userdata flash can erase passwords, SSH keys, network settings, radio
state, and Thread credentials. Prefer the full installer unless deliberately
testing a partition.

The full command/environment reference is in
[Install and upgrade reference](./35-Migration/README.md).

## Runtime administration

| Surface | Default |
| --- | --- |
| Serial console | 38400 8N1 on RTL8196E UART0 |
| SSH | Dropbear on TCP 22 |
| Initial account | `root` / `root`; change immediately |
| Zigbee UART bridge | TCP 8888, one client |
| Persistent configuration | `/userdata/etc` |
| Persistent applications/data | `/userdata` |

The rootfs is read-only. Most `/etc` paths are symlinks into `/userdata`, so
normal configuration survives a rootfs or full-system upgrade. Use
[Using the gateway](../docs/using-the-gateway.md) for common operations and the
[Userdata reference](./34-Userdata/README.md) for every service/key.

The TCP:8888 radio protocol has no application authentication. Keep it on a
trusted LAN or bind the bridge to loopback and tunnel over SSH; see the
[security guide](./32-Kernel/files-6.18/drivers/net/rtl8196e-uart-bridge/SECURITY.md).

## Build from source

Pre-built files are committed for supported boards. Source builds require the
Lexra toolchain and packaging tools from
[1-Build-Environment](../1-Build-Environment/README.md).

Build the complete RTL8196E side:

```bash
cd 3-Main-SoC-Realtek-RTL8196E
./build_rtl8196e.sh
BOARD=sengled-e39-g8c ./build_rtl8196e.sh
KERNEL=7.2 ./build_rtl8196e.sh
```

Or work in a component directory:

```bash
31-Bootloader/build_bootloader.sh
32-Kernel/build_kernel.sh
33-Rootfs/build_rootfs.sh
34-Userdata/build_userdata.sh
```

Read the component guide and its local development instructions before
changing bootloader, kernel, rootfs, or userdata behaviour.

## Platform summary

- Realtek RTL8196E, 400 MHz Lexra RLX4181
- 32 MiB RAM on Lidl, 64 MiB on Sengled G4, and 16 MiB SPI NOR
- Linux 6.18 production line and Linux 7.2 alternate line
- modern devicetree platform support and custom Ethernet/UART/SPI/GPIO drivers
- BusyBox + musl read-only rootfs
- Dropbear SSH
- 12 MiB writable JFFS2 userdata
- in-kernel UART-to-TCP bridge for EFR32 protocols
- native OpenThread Border Router option

The Lexra core lacks normal MIPS unaligned-access instructions and uses
non-coherent DMA. A stock MIPS compiler or generic platform driver is not a safe
substitute for the project toolchain and platform code.
