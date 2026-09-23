# Silabs EFR32 Radio Firmware

This section covers the gateway's IEEE 802.15.4 coprocessor: an EFR32MG1B on
the Lidl board and an EFR32MG13P on the Sengled Smart Hub G4.

The EFR32 is independent of the RTL8196E Linux processor. It has its own
bootloader, application, UART baud, and flow-control requirements. Normal users
flash it over Ethernet after Linux is installed; SWD is a recovery/development
interface, not part of the standard installation.

## Choose before flashing

| Firmware | Use it for | Recommended audience |
| --- | --- | --- |
| **NCP-UART-HW** | Zigbee2MQTT or ZHA coordinator | Most users |
| **RCP-UART-HW** | `cpcd` + host-side `zigbeed` / EmberZNet 8.2, optionally with Thread on the same channel | Advanced users |
| **OT-RCP** | Thread Border Router, external OTBR, or Zigbee-on-Host | Thread/advanced users |
| **Z3 Router** | Standalone Zigbee mesh extender | Users who do not need a coordinator |
| **Gecko Bootloader** | Radio application updates and specialist recovery | Normally managed automatically |

If those terms are unfamiliar, use the plain-language
[radio selection guide](../docs/radio-options.md). **NCP is the default choice
for a new Zigbee2MQTT or ZHA installation.**

## Flash a pre-built radio image

Run the repository-root script against a gateway already running this project's
Linux firmware:

```bash
# Interactive selection
./flash_efr32.sh -g <gateway-ip>

# Recommended Zigbee coordinator
./flash_efr32.sh -y -g <gateway-ip> ncp

# Thread Border Router radio
./flash_efr32.sh -y -g <gateway-ip> otrcp
```

For Sengled, always select the board:

```bash
BOARD=sengled-e39-g8c ./flash_efr32.sh -y -g <gateway-ip> ncp
```

The script checks the live devicetree model, resolves the board-specific image
and safe default baud, obtains exclusive control of the UART bridge, enters the
Gecko bootloader, flashes the application, updates
`/userdata/etc/radio.conf`, and reboots.

Stop Zigbee2MQTT, ZHA, `cpcd`, or any other TCP:8888 client before flashing.
Use `./flash_efr32.sh --help` for the complete firmware and baud matrix.

## Firmware architecture

```text
Application client or host stack
             |
       TCP on Ethernet
             |
RTL8196E Linux / UART bridge or otbr-agent
             |
      board-specific UART
             |
EFR32 application + two-stage Gecko bootloader
             |
      IEEE 802.15.4 radio
```

### NCP

The complete EmberZNet 7.5.1 Zigbee stack runs on the EFR32 and exposes EZSP
v13. Zigbee2MQTT or ZHA connects directly through the gateway's TCP bridge.

Reference: [NCP-UART-HW](./24-NCP-UART-HW/README.md).

### RCP

The EFR32 runs only the 802.15.4 radio layer and speaks CPC. `cpcd` and
`zigbeed` run on a host, allowing EmberZNet 8.2.2 / EZSP v18 on Series-1 radio
hardware. On the Lidl radio the same image can also carry Thread, experimentally
and only on the Zigbee channel (multi-PAN). This path is intentionally more
complex than NCP. The only prebuilt RCP image is the Lidl one (460800); a
Sengled G4 user builds it first.

References: [RCP-UART-HW](./25-RCP-UART-HW/README.md),
[multi-PAN guide](./25-RCP-UART-HW/docker/cpcd-zigbeed-otbr/README.md) and
[EmberZNet 8.x background](./25-RCP-UART-HW/EMBERZNET-8.x-GUIDE.md).

### OT-RCP

The EFR32 speaks OpenThread Spinel/HDLC. It can serve native `otbr-agent` on the
gateway, an OTBR on another host, or Zigbee2MQTT's Zigbee-on-Host adapter. The
application image is the same; the RTL8196E-side service selection changes.

References: [OT-RCP](./26-OT-RCP/README.md) and
[Thread/Matter primer](./26-OT-RCP/THREAD-MATTER-PRIMER.md).

### Standalone router

The EFR32 runs a complete Zigbee 3.0 router application and joins another
coordinator's mesh. The gateway no longer presents a coordinator endpoint.

Reference: [Z3 Router](./27-Router/README.md).

## Gateway-side runtime configuration

`flash_efr32.sh` records the chip-side identity and the service-routing state in
`/userdata/etc/radio.conf`:

| Application | Important host-side state | Service that owns UART1 |
| --- | --- | --- |
| NCP | `FIRMWARE=ncp`, baud, flow mode, no `MODE=otbr` | `S50uart_bridge`, TCP:8888 |
| RCP | `FIRMWARE=rcp`, baud, flow mode, no `MODE=otbr` | `S50uart_bridge`, TCP:8888 |
| OT-RCP, native OTBR | `FIRMWARE=otrcp`, `MODE=otbr`, baud, flow mode | `S70otbr`, direct tty |
| OT-RCP, external host | `FIRMWARE=otrcp`, no `MODE=otbr` | `S50uart_bridge`, TCP:8888 |
| Router | `FIRMWARE=router` | Bridge remains available for maintenance, not coordinator traffic |

Do not manually change `FIRMWARE_BAUD` to a value the chip was not built for;
the two UART ends would stop communicating. The full key reference is in
[Userdata](../3-Main-SoC-Realtek-RTL8196E/34-Userdata/README.md#radioconf-keys-full-reference).

## Board differences

Every build and flash command accepts `BOARD` (`lidl` by default or
`sengled-e39-g8c`). Board data defines:

- exact EFR32 part;
- USART and pin routing;
- hardware, software, or no flow control;
- safe default baud;
- bootloader activation and version data;
- non-Lidl artifact suffixes.

The Lidl board wires RTS/CTS. The Sengled G4 does not, so it uses board-specific
software/no-flow builds and lower RCP/OT-RCP defaults. Never choose a firmware
only by filename similarity.

The [board status and porting table](./boards/README.md) distinguishes images
validated on real hardware from images that are built and shipped but not yet
field-tested.

## Gecko bootloader and recovery

The radio boot chain has two stages:

- Stage 1 is installed through SWD and cannot be updated over UART;
- Stage 2 receives application `.gbl` images over UART/Xmodem and can itself be
  updated through Stage 1.

Normal application flashes transit Stage 2 automatically. Do not reflash a
bootloader merely to update an NCP/RCP/OT-RCP application. Bootloader version
ordering is strict, and a same-version Stage-2 update can erase the application
without installing a new bootloader; `flash_efr32.sh` contains a safety guard.

References: [two-stage bootloader](./23-Bootloader-UART-Xmodem/README.md),
[backup/flash/restore](./22-Backup-Flash-Restore/README.md), and
[recovery post-mortem](./POST-MORTEM-bootloader-recovery.md).

## Build from source

Pre-built `.gbl` and `.s37` artifacts are committed for supported boards (the
Sengled G4 RCP excepted, see [per-board builds](./boards/README.md)). To
modify them, first install the complete
[build environment](../1-Build-Environment/README.md), then use:

```bash
cd 2-Zigbee-Radio-Silabs-EFR32
./build_efr32.sh
./build_efr32.sh ncp rcp
BOARD=sengled-e39-g8c ./build_efr32.sh ncp
./build_efr32.sh --help
```

Every firmware subdirectory holds the SLC project, source/patch overlay, build
script, and committed outputs. Rebuilding against a different Gecko SDK or
without the project patches can silently remove UART routing or flow control;
follow the component guide rather than invoking `slc generate` by hand.

## Reference map

| Topic | Page |
| --- | --- |
| EZSP concepts and compatibility | [EZSP reference](./20-EZSP-Reference/README.md) |
| Simplicity Studio and custom builds | [Simplicity Studio](./21-Simplicity-Studio/README.md) |
| Radio backup, flashing, and restore | [Backup / Flash / Restore](./22-Backup-Flash-Restore/README.md) |
| Gecko UART/Xmodem bootloader | [Bootloader](./23-Bootloader-UART-Xmodem/README.md) |
| NCP | [NCP-UART-HW](./24-NCP-UART-HW/README.md) |
| RCP and host-side Zigbee | [RCP-UART-HW](./25-RCP-UART-HW/README.md) |
| OpenThread RCP | [OT-RCP](./26-OT-RCP/README.md) |
| Standalone Zigbee router | [Router](./27-Router/README.md) |
