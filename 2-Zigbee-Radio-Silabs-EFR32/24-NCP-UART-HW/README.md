# NCP-UART-HW Firmware

Network Co-Processor (NCP) firmware for the EFR32 radio: EFR32MG1B232F256GM48 on the Lidl Silvercrest Smart Home Gateway (`BOARD=lidl`, the default), EFR32MG13P732F512IM32 on the Sengled Smart Hub G4 (`BOARD=sengled-e39-g8c`). Both ship prebuilt.

This firmware enables communication with Zigbee coordinators like **Zigbee2MQTT** and **Home Assistant ZHA** via EZSP (EmberZNet Serial Protocol).

> **Multi-board:** this firmware also builds for other RTL8196E hubs via `BOARD=`
> (default `lidl`); e.g. `BOARD=sengled-e39-g8c` for the Sengled Smart Hub G4,
> whose prebuilt is committed and **validated end-to-end on real hardware**
> (#130) — see [`../boards/README.md`](../boards/README.md).

## Features

- **EZSP v13** - EmberZNet 7.5.1 serial protocol
- **Zigbee PRO R22** - Full Zigbee 3.0 support
- **Green Power** - Support for battery-free devices
- **Source routing** - 100-entry route table for large networks
- **Up to 32 children** - Sleepy end-devices supported
- **Hardware flow control** - RTS/CTS for reliable TCP operation

## Hardware

| Component | Specification |
|-----------|---------------|
| Zigbee SoC | EFR32MG1B232F256GM48 |
| Flash | 256KB |
| RAM | 32KB |
| Radio | 2.4GHz IEEE 802.15.4 |
| UART | PA0 (TX), PA1 (RX), PA4 (RTS), PA5 (CTS) @ 115200 baud |

---

## Option 1: Flash Pre-built Firmware (Recommended)

Pre-built firmware is available in the `firmware/` directory. From the
repository root:

```bash
# Lidl Silvercrest (default board)
./flash_efr32.sh -y ncp                 # default baud 115200, gateway from gateway.env
./flash_efr32.sh -y ncp 460800          # 460800 baud (faster, max-tested 892857)
./flash_efr32.sh -y -g 10.0.0.5 ncp     # custom gateway IP

# Sengled Smart Hub G4: validated NCP image, default baud 115200
./flash_efr32.sh -y --board sengled-e39-g8c -g 10.0.0.6 ncp

./flash_efr32.sh --help                 # full CLI reference
```

The script handles everything: pulse `nRST` for a clean chip state, switch
the in-kernel UART bridge to flash mode, run the Xmodem upload, write the
matching `FIRMWARE_BAUD=<baud>` to `/userdata/etc/radio.conf` so the bridge
arms at the right speed on next boot, then reboot.

Supported NCP bauds (pre-built GBLs): 115200, 230400, 460800, 691200, 892857.
For a custom baud, see [Option 2](#option-2-build-from-source) below.

> **Legacy env-var interface** (deprecated, kept for v3.0.x compat):
> `FW_CHOICE=2 BAUD_CHOICE=460800 CONFIRM=y ./flash_efr32.sh` still works
> with a deprecation warning. Prefer the flag form above.

> **Need other formats (.s37, .hex, .bin)?** Build from source (Option 2),
> they will be in `build/debug/`.

### Gateway state after flash

`flash_efr32.sh` writes the matching baud to `/userdata/etc/radio.conf` so
the gateway-side init scripts arm the bridge correctly on next boot. For
NCP at baud `<B>`:

```
FIRMWARE=ncp           # what's in the EFR32 application slot
FIRMWARE_VERSION=7.5.1 # EmberZNet version embedded in the GBL
FIRMWARE_BAUD=<B>      # chip-side UART baud — S50uart_bridge reads this and
                       # arms TCP:8888 at <B> (no MODE= line; otbr-agent off)
```

`FIRMWARE_BAUD` is the single source of truth (chip-side baud =
host-side baud, since both ends of the UART link must agree); the
`FIRMWARE*` companion keys are informational. See
[`3-Main-SoC-Realtek-RTL8196E/34-Userdata/README.md`](../../3-Main-SoC-Realtek-RTL8196E/34-Userdata/README.md#radioconf-keys-full-reference)
for the full key reference.

The init script `S50uart_bridge` reads this on boot. Z2M/ZHA then connect
to `tcp://<gw>:8888` — no baud setting needed on the client side, the
bridge handles it.

---

## Option 2: Build from Source

For users who want to customize network parameters or use a different EmberZNet version.

### Prerequisites

Install Silicon Labs tools (see `1-Build-Environment/12-silabs-toolchain/`):

```bash
cd 1-Build-Environment/12-silabs-toolchain
./install_silabs.sh
```

Or use Docker (see `1-Build-Environment/` for setup):

```bash
docker run --rm -v $(pwd):/workspace rtl8196e-gateway-builder \
    /workspace/2-Zigbee-Radio-Silabs-EFR32/24-NCP-UART-HW/build_ncp.sh
```

### Build

```bash
cd 2-Zigbee-Radio-Silabs-EFR32/24-NCP-UART-HW
./build_ncp.sh                  # default baud 115200
./build_ncp.sh 460800           # custom baud (any value; warns if outside tested set)
./build_ncp.sh --help           # show baud options + defaults
```

### Output

The output filename embeds the EmberZNet version and the chosen baud:

```
firmware/
├── ncp-uart-hw-7.5.1-115200-hw.gbl   # default (-hw = RTS/CTS flow, #145)
├── ncp-uart-hw-7.5.1-230400-hw.gbl   # if you ran ./build_ncp.sh 230400
├── ncp-uart-hw-7.5.1-460800-hw.gbl
├── ncp-uart-hw-7.5.1-691200-hw.gbl
└── ncp-uart-hw-7.5.1-892857-hw.gbl
```

`flash_efr32.sh` resolves the right file via a glob — no need to keep
filenames in sync manually.

> **Other formats (.s37, .hex, .bin)** are available in `build/debug/`
> after compilation. Use these for J-Link/SWD flashing or debugging.

### Clean

```bash
./build_ncp.sh clean
```

### Flash

**Via network (same as Option 1):**
```bash
./flash_efr32.sh -y ncp 460800     # baud must match what you built above
```

**Via J-Link/SWD** (if you have physical access to the SWD pads):
```bash
commander flash firmware/ncp-uart-hw-7.5.1-460800-hw.gbl \
    --device EFR32MG1B232F256GM48
```

---

## Usage

### Architecture

```
+-------------------+    UART     +-------------------+   Ethernet   +---------------------+
|  EFR32MG1B (NCP)  |   115200    |  RTL8196E         |    TCP/IP    |  Host (x86/ARM)     |
|                   |    baud     |  (Gateway SoC)    |              |                     |
|  EmberZNet Stack  |<----------->|  in-kernel        |<------------>|  Zigbee2MQTT        |
|  + EZSP Protocol  |   ttyS1     |  UART<->TCP bridge|   port 8888  |       or            |
|                   |             |  (rtl8196e-uart-  |              |  Home Assistant ZHA |
|  HW Flow Control  |             |   bridge)         |              |                     |
+-------------------+             +-------------------+              +---------------------+
```

The RTL8196E kernel bridges the EFR32's UART to TCP:8888 via the in-kernel
`rtl8196e-uart-bridge` driver (replaces the former `serialgateway` daemon).
See [34-Userdata](../../3-Main-SoC-Realtek-RTL8196E/34-Userdata/README.md) for gateway setup.

### Zigbee2MQTT Configuration

Edit `configuration.yaml`:

```yaml
serial:
  port: tcp://192.168.1.88:8888
  adapter: ember
```

### Home Assistant ZHA Configuration

Add integration with:
- **Serial port path:** `socket://192.168.1.88:8888`

> **Note:** Baudrate and flow control are handled by the in-kernel UART
> bridge on the gateway side, not by the client application. Change the
> rate via `echo <baud> > /sys/module/rtl8196e_uart_bridge/parameters/baud`.

---

## Customization

### Changing Baudrate

Default is **115200**. With the in-kernel UART bridge on kernel 6.18,
rates up to **892857** are supported (115200, 230400, 460800, 691200,
892857 tested).

#### Why 892857 and not 921600

The RTL8196E UART has a **fixed 16× oversampling** with integer-only
divisors and a 200 MHz bus clock, so the achievable baud is
`200000000 / (16 × N)` for integer N. For 921600 the divisor falls at
13.56 — neither 13 nor 14 gives acceptable error (−3.1% / +4.3%).

**892857** = 200000000 / (16 × 14) hits an exact integer divisor: **0% baud
error** on the RTL side. The EFR32 reaches 893023 with its fractional divider
(0.02% mismatch). That is 7.7× the original 115200 and within 3% of 921600.
The full divisor investigation is in
[`POST-MORTEM-6.18.md`](../../3-Main-SoC-Realtek-RTL8196E/32-Kernel/POST-MORTEM-6.18.md).

```bash
# 1. Build the GBL at the desired baud
cd 2-Zigbee-Radio-Silabs-EFR32/24-NCP-UART-HW && ./build_ncp.sh 460800

# 2. Flash — radio.conf FIRMWARE_BAUD is updated automatically
./flash_efr32.sh -y ncp 460800
```

NCP/EZSP doesn't have cpcd's POSIX baud restriction, so all 5 tested
values work end-to-end (Z2M ember adapter, ZHA).

### Network parameters

The build process applies patches to optimize the firmware for these
gateways. See [patches/README.md](https://github.com/jnilo1/rtl8196e-gateway/blob/main/2-Zigbee-Radio-Silabs-EFR32/24-NCP-UART-HW/patches/README.md) for details.

### Network Parameters

Edit `patches/apply_config.sh` to modify. For a complete guide, see the [Zigbee Network Sizing Guide](https://github.com/jnilo1/slc-projects/blob/main/ncp-uart-hw/ZIGBEE_NETWORK_SIZING_GUIDE.md).

| Parameter | Default | Range | RAM/Entry |
|-----------|---------|-------|-----------|
| `EMBER_MAX_END_DEVICE_CHILDREN` | 32 | 0-64 | ~40 bytes |
| `EMBER_PACKET_BUFFER_COUNT` | 255 | 20-255 | ~36 bytes |
| `EMBER_SOURCE_ROUTE_TABLE_SIZE` | 100 | 2-255 | ~12 bytes |
| `EMBER_BINDING_TABLE_SIZE` | 32 | 1-127 | ~20 bytes |
| `EMBER_ADDRESS_TABLE_SIZE` | 12 | 1-256 | ~16 bytes |
| `EMBER_NEIGHBOR_TABLE_SIZE` | 26 | 16/26 | ~32 bytes |
| `EMBER_KEY_TABLE_SIZE` | 12 | 1-127 | ~32 bytes |

### Network Size Presets

| Preset | Devices | Children | Buffers | Routes | Bindings |
|--------|---------|----------|---------|--------|----------|
| **Small** | <20 | 10 | 75 | 20 | 10 |
| **Medium** | 20-50 | 20 | 150 | 50 | 20 |
| **Large** (default) | 50-100 | 32 | 255 | 100 | 32 |
| **Very Large** | 100-150 | 48 | 255 | 150 | 48 |

> **Warning**: The EFR32MG1B has only 32KB RAM. Current configuration uses ~27KB. Increasing parameters beyond presets may cause instability.

---

## Technical Details

### Memory Usage

| Resource | Used | Available |
|----------|------|-----------|
| Flash | ~200 KB (78%) | 256 KB |
| RAM | ~24 KB (75%) | 32 KB |
| NVM3 | 36 KB | Network data storage |

### Optimizations Applied

To fit in 256KB flash, the following were removed:

| Component | Savings | Reason |
|-----------|---------|--------|
| Debug print | ~12 KB | No debug output needed |
| ZLL / touchlink | ~4 KB | Not used |
| Virtual UART | ~1 KB | Not needed |
| PTI (Packet Trace) | ~2 KB | No debug probe |

### Features

- **RTL8196E Boot Delay:** 1-second delay for host UART initialization
- **Hardware Flow Control:** RTS/CTS required for reliable TCP operation
- **NVM3 Storage:** 36KB for network credentials and tokens

---

## Troubleshooting

### No response from NCP

1. Verify TCP connection: `nc -zv <gateway-ip> 8888`
2. Check baud rate matches (115200) on firmware side and on the gateway
   (`cat /sys/module/rtl8196e_uart_bridge/parameters/baud`)
3. Verify hardware flow control is on:
   `cat /sys/module/rtl8196e_uart_bridge/parameters/flow_control` → `1`

### EZSP communication errors

1. Ensure Z2M/ZHA is configured for `ember` adapter
2. Check for EZSP version mismatch (this firmware is EZSP v13)
3. Re-enable hardware flow control if left off by a failed flash:
   `echo 1 > /sys/module/rtl8196e_uart_bridge/parameters/flow_control`

### Device won't pair

1. Enable permit join in Z2M/ZHA
2. Factory reset the device
3. Move device closer to coordinator for initial pairing

---

## Files

```
24-NCP-UART-HW/
├── build_ncp.sh                 # Build script
├── README.md                    # This file
├── firmware/                    # Pre-built firmware files
│   ├── ncp-uart-hw-7.5.1.gbl
└── patches/
    ├── README.md                # Patch documentation
    ├── apply_config.sh          # Configuration script
    ├── ncp-uart-hw.slcp         # Project config
    ├── main.c                   # Entry point (1s delay)
    └── sl_*.h                   # Configuration headers
```

---

## Related Projects

- `25-RCP-UART-HW/` - RCP firmware (CPC protocol, for cpcd + zigbeed)
- `26-OT-RCP/` - OpenThread RCP (for zigbee-on-host)
- `27-Router/` - Autonomous Zigbee router (no host needed)

## References

- [EZSP Protocol Reference (UG100)](https://www.silabs.com/documents/public/user-guides/ug100-ezsp-reference-guide.pdf)
- [EmberZNet NCP Guide (UG115)](https://www.silabs.com/documents/public/user-guides/ug115-ncp-user-guide.pdf)
- [AN1233: Zigbee Stack Configuration](https://www.silabs.com/documents/public/application-notes/an1233-zigbee-stack-config.pdf)

## License

Educational and personal use. Silicon Labs SDK components under their respective licenses.
