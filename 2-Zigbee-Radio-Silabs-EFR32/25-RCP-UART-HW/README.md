# RCP 802.15.4 Firmware

Radio Co-Processor (RCP) firmware for the EFR32 radio: EFR32MG1B232F256GM48 on the Lidl Silvercrest Smart Home Gateway (`BOARD=lidl`, the default), EFR32MG13P732F512IM32 on the Sengled Smart Hub G4 (`BOARD=sengled-e39-g8c`). Both ship prebuilt, but the G4 image has never been run on hardware — see [Per-board builds](../boards/README.md).

This firmware transforms the gateway's Zigbee chip into a **Radio Co-Processor**
that handles only the 802.15.4 PHY/MAC layer. The Zigbee stack runs host-side
in `zigbeed`, which lets us pair a Series 1 EFR32MG1B radio with a modern
**EmberZNet 8.2.2 [GA], build 532** stack — the gateway then exposes **EZSP v18** to Z2M / ZHA,
even though Silabs froze on-chip Series 1 support at EmberZNet 7.5.1.

> **Multi-board:** this firmware also builds for other RTL8196E hubs via `BOARD=`
> (default `lidl`, #143); e.g. `BOARD=sengled-e39-g8c ./build_rcp.sh 230400` for
> the Sengled Smart Hub G4 (MG13 target). A G4 prebuilt is committed at
> **230400**, the operating point measured on that board (#134, #142), and is
> what `flash_efr32.sh` picks there by default — but it has **not** been run on
> G4 hardware, so validate it on yours before trusting it. CPC supports only
> RTS/CTS or no flow control, so a
> `BOARD_UART_FLOW=sw` board is built with flow control **none**; the chip's
> flow partner is the gateway's in-kernel UART bridge (cpcd connects to it
> over TCP), and `flash_efr32.sh` records `FIRMWARE_FLOW_CTRL=none` for this
> build so the bridge arms to match. Non-lidl artefacts carry a `-<board>`
> filename suffix, resolved by `flash_efr32.sh` from the same `BOARD=`
> selector — see [`../boards/README.md`](../boards/README.md).

| Host stack | Exposes | When to use |
|------------|---------|-------------|
| `cpcd` + `zigbeed` **EmberZNet 8.2.2 [GA], build 532** (recommended) | EZSP v18 | Default — modern stack, latest Z2M/ZHA features |
| `cpcd` + `zigbeed` **7.5.1** (legacy) | EZSP v13 | Only if you need bit-for-bit parity with the NCP-UART-HW path |

> **Experimental multi-PAN:** the Lidl EFR32MG1B can serve Zigbee on IID 1 and
> Thread on IID 2 concurrently **on the same 802.15.4 channel**. The Series 1
> radio cannot operate those networks on independent channels; that requires
> Series 2 Concurrent Listening. The current validation is a short same-channel
> test only, without a real Thread end device or long mixed-traffic soak. See
> [`docker/cpcd-zigbeed-otbr/README.md`](./docker/cpcd-zigbeed-otbr/README.md)
> for the reproducible host build, explicit IID allocation, and caveats.

## About RCP Architecture

Unlike standalone firmware (like the Router), an RCP delegates the entire Zigbee protocol stack to a host computer. The EFR32 only handles the radio PHY/MAC layer and communicates with the host via the **CPC Protocol** (Co-Processor Communication).

```
+-------------------+    UART     +-------------------+   Ethernet   +---------------------+
|  EFR32MG1B (RCP)  |   460800    |  RTL8196E         |    TCP/IP    |  Host (x86/ARM)     |
|                   |    baud     |  (Gateway SoC)    |              |                     |
|  802.15.4 PHY/MAC |<----------->|  in-kernel        |<------------>|  cpcd               |
|  + CPC Protocol   |   ttyS1     |  UART<->TCP bridge|   port 8888  |    |                |
|                   |             |  (rtl8196e-uart-  |              |    v                |
|                   |             |   bridge)         |              |                     |
|  CPC Protocol v5  |             |                   |              |  zigbeed            |
|  HW Flow Control  |             |                   |              |  (Zigbee stack)     |
|                   |             |                   |              |    |                |
+-------------------+             +-------------------+              |    v                |
                                                                     |  Zigbee2MQTT        |
                                                                     +---------------------+
```

**Why use RCP instead of NCP?**

| Aspect | NCP (24-NCP-UART-HW) | RCP (this firmware) |
|--------|----------------------|---------------------|
| Stack location | On EFR32 (limited RAM) | On host (unlimited resources) |
| Protocol | EZSP (binary) | CPC (multiplexed) |
| Stack version | Locked to 7.5.1 (EZSP v13) | 7.5.1 *or* 8.2.2 (EZSP v13 / v18) |
| Stack updates | Requires reflashing the EFR32 | Swap `zigbeed` on the host, EFR32 untouched |
| Network size | Limited by EFR32 RAM | Host memory is the limit |

## Hardware

| Component | Specification |
|-----------|---------------|
| Zigbee SoC | EFR32MG1B232F256GM48 |
| Flash | 256KB |
| RAM | 32KB |
| Radio | 2.4GHz IEEE 802.15.4 |
| UART | PA0 (TX), PA1 (RX), PA4 (RTS), PA5 (CTS) @ 460800 baud |

---

## Option 1: Flash Pre-built Firmware (Recommended)

Pre-built firmware is available in the `firmware/` directory. From the
repository root:

```bash
./flash_efr32.sh -y rcp                 # default baud 460800, gateway from gateway.env
./flash_efr32.sh -y rcp 230400          # 230400 baud (cpcd POSIX-supported only)
./flash_efr32.sh -y -g 10.0.0.5 rcp     # custom gateway IP
./flash_efr32.sh --help                 # full CLI reference
```

The script handles everything: pulse `nRST` for a clean chip state, switch
the in-kernel UART bridge to flash mode, run the Xmodem upload, write the
matching `FIRMWARE_BAUD=<baud>` to `/userdata/etc/radio.conf` so the bridge
arms at the right speed on next boot, then reboot.

Supported RCP bauds (pre-built GBLs): 115200, 230400, 460800.
**`cpcd` rejects non-POSIX bauds (691200, 892857), so RCP is capped at
460800.** For a custom baud (POSIX values only), see
[Option 2](#option-2-build-from-source) below.

> **Legacy env-var interface** (deprecated, kept for v3.0.x compat):
> `FW_CHOICE=3 BAUD_CHOICE=460800 CONFIRM=y ./flash_efr32.sh` still works
> with a deprecation warning. Prefer the flag form above.

### Gateway state after flash

`flash_efr32.sh` writes the matching baud to `/userdata/etc/radio.conf`
so the gateway-side init scripts arm the bridge correctly on next boot.
For RCP at baud `<B>`:

```
FIRMWARE=rcp           # what's in the EFR32 application slot
FIRMWARE_BAUD=<B>      # chip-side UART baud — S50uart_bridge reads this and
                       # arms TCP:8888 at <B> (no MODE= line; otbr-agent off)
```

(No `FIRMWARE_VERSION` for RCP — the meaningful EmberZNet version is
host-side in `zigbeed`, not in the chip firmware.) See
[`3-Main-SoC-Realtek-RTL8196E/34-Userdata/README.md`](../../3-Main-SoC-Realtek-RTL8196E/34-Userdata/README.md#radioconf-keys-full-reference)
for the full key reference.

The init script `S50uart_bridge` reads this on boot. `cpcd` on the host
then connects to `tcp://<gw>:8888` (see [Host Software Setup](#host-software-setup)
or `docker/docker-compose-zigbee.yml`).

Then continue to [Host Software Setup](#host-software-setup) to configure
cpcd and zigbeed on your host machine.

---

## Option 2: Build from Source

For users who want to modify the CPC configuration, change baudrate, or use a different SDK version.

### Prerequisites

Install Silicon Labs tools (see `1-Build-Environment/12-silabs-toolchain/`):

```bash
cd 1-Build-Environment/12-silabs-toolchain
./install_silabs.sh
```

This installs:
- `slc-cli` - Silicon Labs Configurator
- `arm-none-eabi-gcc` - ARM GCC toolchain
- `commander` - Simplicity Commander
- Gecko SDK with EmberZNet

### Build

```bash
cd 2-Zigbee-Radio-Silabs-EFR32/25-RCP-UART-HW
./build_rcp.sh                  # default baud 460800
./build_rcp.sh 230400           # POSIX baud only — cpcd rejects 691200/892857
./build_rcp.sh --help           # show baud options + defaults
```

### Output

The output filename embeds the chosen baud:

```
firmware/
├── rcp-uart-802154-115200-hw.gbl
├── rcp-uart-802154-230400-hw.gbl
└── rcp-uart-802154-460800-hw.gbl   # default (-hw = RTS/CTS flow; CPC has no
                                    # sw mode, so a sw board builds as -none, #145)
```

`flash_efr32.sh` resolves the right file via a glob.

### Customization

Pin layout (PA0/PA1/PA4/PA5) is in `patches/sl_cpc_drv_uart_usart_vcom_config.h`.
The build script edits the `BAUDRATE` define automatically based on the
positional `BAUD` argument — no manual file editing for baud changes.

### Flash

**Via network (same as Option 1):**
```bash
./flash_efr32.sh -y rcp 460800     # baud must match what you built above
```

**Via J-Link/SWD** (if you have physical access to the SWD pads):
```bash
commander flash firmware/rcp-uart-802154-460800-hw.gbl \
    --device EFR32MG1B232F256GM48
```

> For a detailed explanation of how `universal-silabs-flasher` works (firmware detection, bootloader entry, the `-f` flag, troubleshooting), see [22-Backup-Flash-Restore](../22-Backup-Flash-Restore/README.md).

---

## Experimental research

An isolated prototype explores a combined 802.15.4 RCP and Bluetooth HCI
endpoint for the Sengled G4. It has never run on gateway hardware, ships no
pre-built image, does not fit on Lidl, and requires an unvalidated CPC/BlueZ
host chain. It is intentionally kept outside the supported RCP workflow.

See [Experimental RCP with Bluetooth HCI](experimental/rcp-ble-hci/README.md)
for its constraints, build instructions, recovery expectations, and the
explicit flash command.

---

## Host Software Setup

After flashing the RCP firmware, you need to configure the host software chain.

### Required Components

| Component | Version | Source | Description |
|-----------|---------|--------|-------------|
| cpcd | v4.5.3 | [SiliconLabs/cpc-daemon](https://github.com/SiliconLabs/cpc-daemon) | CPC daemon |
| zigbeed | EmberZNet 8.2.2 [GA], build 532 | Simplicity SDK 2025.6.3 | Zigbee stack daemon (recommended) |
| zigbeed | EmberZNet 7.5.1 | Gecko SDK 4.5.0 | Zigbee stack daemon (legacy) |

> **CPC transport — native TCP bus.** `cpcd` here is built with a native
> `bus_type: TCP` (carried as [`cpcd/tcp-bus.patch`](./cpcd/tcp-bus.patch) and
> applied automatically by `build_cpcd.sh`). cpcd connects straight to the
> gateway's in-kernel UART↔TCP bridge on `TCP:8888` and owns its own
> reconnection, so the host stack no longer needs a `socat` PTY shim in front
> of cpcd. The Docker stack config in this tree (`docker/cpcd-zigbeed/`) is
> wired for it (`bus_type: TCP`, `tcp_server_address`/`tcp_server_port`);
> rebuild the image from that directory to ship the patched cpcd. Set
> `bus_type: UART` with `uart_device_file` to fall back to the classic
> socat-PTY path.

### Build Instructions

See subdirectories for detailed build instructions:
- `cpcd/` - CPC daemon (for host)
- `zigbeed-8.2.2/` - zigbeed EmberZNet 8.2.2 (recommended)
- `zigbeed-7.5.1/` - zigbeed EmberZNet 7.5.1 (legacy)
- `rcp-stack/` - Systemd service manager for the complete chain

### Quick Start with Docker (Recommended)

A pre-built Docker image is available for **PC (amd64)** and **Raspberry Pi (arm64)**:

```bash
# Pull the image
docker pull ghcr.io/jnilo1/cpcd-zigbeed:latest

# Or use the full stack with Zigbee2MQTT
cd docker/
# Edit docker-compose-zigbee.yml: set RCP_HOST to your gateway's IP
docker compose -f docker-compose-zigbee.yml up -d
```

The Docker default is `ZIGBEED_TRANSPORT=tcp`: Zigbeed exposes native TCP
`tcp-listen://0.0.0.0:9999` directly to Zigbee2MQTT. Set
`ZIGBEED_TRANSPORT=pty` only for the retained PTY+socat compatibility path.

See `docker/README.md` for detailed instructions.

| Image | cpcd | EmberZNet | EZSP | Architectures |
|-------|------|-----------|------|---------------|
| `ghcr.io/jnilo1/cpcd-zigbeed:latest` | 4.5.3 | 8.2.2 | v18 | amd64, arm64 |

### Quick Start with rcp-stack (Native)

The `rcp-stack` tool manages the entire cpcd + zigbeed chain natively (without Docker):

```bash
# Start the stack
rcp-stack up

# Check status
rcp-stack status

# Stop the stack
rcp-stack down

# Troubleshoot
rcp-stack doctor
```

### Zigbee2MQTT Configuration

With rcp-stack (recommended):
```yaml
serial:
  port: /tmp/ttyZ2M
  adapter: ember
```

---

## Baudrate and Network Considerations

### Baudrate Options

| Baudrate | Status | Notes |
|----------|--------|-------|
| 115200 | Supported | Conservative |
| 230400 | Supported | |
| **460800** | **Default** | Max supported by cpcd (POSIX baud limit) |
| 692857 | Not usable | Non-standard — cpcd rejects it |
| 892857 | Not usable | Non-standard — cpcd rejects it |

cpcd validates baud rates against the POSIX standard list and rejects
non-standard values like 691200 or 892857 (even over a TCP/PTY bridge
where the baud is irrelevant). **460800 is the practical maximum for
RCP mode.** Higher bauds (up to 892857) are available for NCP and
OT-RCP, which don't use cpcd.

All bauds run through the in-kernel `rtl8196e-uart-bridge` driver on
kernel 6.18 — change via
`echo <baud> > /sys/module/rtl8196e_uart_bridge/parameters/baud`.

### Network Size vs Baudrate

| Liaison | Throughput |
|---------|------------|
| 802.15.4 radio | ~25 KB/s |
| UART 115200 | ~11 KB/s |
| UART 460800 | ~46 KB/s |
| UART 892857 | ~89 KB/s |

At 115200, the UART is ~2x slower than the Zigbee radio. At 460800+
it is no longer the bottleneck.

| Network size | 115200 | 230400 | 460800+ |
|--------------|--------|--------|---------|
| < 50 devices | OK | OK | OK |
| 50-100 devices | OK* | OK | Recommended |
| > 100 devices | May bottleneck | OK | Recommended |

*OK for normal use; possible latency during traffic spikes (OTA updates, large groups).

For most home installations, 115200 is sufficient.

### Maximum Baud: Why 892857 and Not 921600

The RTL8196E UART has a **fixed 16× oversampling** with integer-only
divisors and a 200 MHz bus clock. The achievable baud is
`200000000 / (16 × N)` for integer N. For 921600 the divisor falls at
13.56 — neither 13 nor 14 gives acceptable error (−3.1% / +4.3%).

**892857** = 200000000 / (16 × 14) hits an exact integer divisor,
giving **0% baud error** on the RTL side. The EFR32 reaches 893023
with its fractional divider (0.02% mismatch). This is 7.7× the
original 115200 and within 3% of 921600.

See `3-Main-SoC-Realtek-RTL8196E/32-Kernel/POST-MORTEM-6.18.md` for
the full N+1 divisor investigation.

Check UART errors on the gateway:
```bash
cat /proc/tty/driver/serial
# fe:xxx = framing errors, oe:xxx = overrun errors
```

### Changing Baudrate

```bash
# 1. Build the GBL at the desired baud (POSIX values only for RCP)
cd 2-Zigbee-Radio-Silabs-EFR32/25-RCP-UART-HW && ./build_rcp.sh 230400

# 2. Flash — radio.conf FIRMWARE_BAUD is updated automatically
./flash_efr32.sh -y rcp 230400

# 3. Nothing to change host-side: cpcd talks TCP to the gateway's bridge,
#    and the bridge arms the new baud from radio.conf on next boot.
```

The baud must be a standard POSIX value (115200, 230400, 460800) — `cpcd`
rejects anything else (the build script and the flash script will warn
but not block, in case you plan to use a non-cpcd consumer).

---

## TCP Stability Requirements

The CPC protocol is sensitive to network conditions. For reliable operation:

| Requirement | Why |
|-------------|-----|
| **Hardware flow control** | Prevents buffer overruns |
| **Direct Ethernet** | Minimizes latency and jitter |
| **No WiFi bridges** | WiFi adds unpredictable latency |
| **Avoid congested switches** | Packet delays cause CPC timeouts |

**Recommended:** Connect the gateway directly to the host with an Ethernet cable.

---

## Troubleshooting

### Flashing Issues

| Problem | Solution |
|---------|----------|
| No response from RCP | Verify TCP: `nc -zv <gateway-ip> 8888` |
| Xmodem timeout | Close all SSH sessions, use `-f` flag |
| Wrong firmware flashed | Reflash - the bootloader is always preserved |

### cpcd Connection Issues

| Problem | Solution |
|---------|----------|
| cpcd won't connect | With the native TCP bus, check `tcp_server_address`/`tcp_server_port` in cpcd.conf point at the gateway bridge; verify `nc -zv <gateway-ip> 8888` |
| CPC version mismatch | Use GSDK 4.5.0 for CPC Protocol v5 |
| Frequent disconnects | Use direct Ethernet, check for WiFi bridges |

### zigbeed Issues

| Problem | Solution |
|---------|----------|
| zigbeed won't start | Check cpcd is running: `rcp-stack status` |
| Stack version mismatch | Rebuild zigbeed with matching SDK version |

---

## Memory Usage

| Resource | Used | Available |
|----------|------|-----------|
| Flash | ~116 KB | 256 KB |
| RAM | ~22 KB | 32 KB |

---

## Features

- **RTL8196E Boot Delay:** 1-second delay for host UART initialization
- **Hardware Flow Control:** RTS/CTS required for reliable TCP operation
- **CPC Security Disabled:** Saves ~45KB flash (not needed for local network)
- **Stack flexibility:** Same `.gbl` works with `zigbeed` 7.5.1 *or* 8.2.2 — no EFR32 reflash needed to change EZSP version
- **Native TCP bus in cpcd:** cpcd dials the gateway's UART↔TCP bridge directly (`bus_type: TCP`) and reconnects on its own — no `socat` PTY shim, no stale-PTY restart cascade

---

## Project Structure

```
25-RCP-UART-HW/
├── build_rcp.sh                 # RCP firmware build script
├── README.md                    # This file
├── patches/                     # RCP firmware patches
│   ├── rcp-uart-802154.slcp                 # Project config
│   ├── main.c                               # Entry point (1s delay)
│   ├── sl_cpc_drv_uart_usart_vcom_config.h  # UART pins
│   └── sl_cpc_security_config.h             # CPC security disabled
├── firmware/                    # Output (rcp-uart-802154.gbl)
├── cpcd/                        # CPC daemon build scripts
├── zigbeed-7.5.1/               # zigbeed EmberZNet 7.5.1 (legacy)
├── zigbeed-8.2.2/               # zigbeed EmberZNet 8.2.2 (recommended)
├── docker/                      # Docker stack (cpcd + zigbeed + Z2M)
├── experimental/
│   └── rcp-ble-hci/             # Unvalidated Sengled-only research prototype
└── rcp-stack/                   # Systemd service manager
    ├── bin/rcp-stack            # Main script
    ├── scripts/                 # Helper scripts
    ├── systemd/user/            # Service units
    └── examples/                # Config examples
```

---

## Related Projects

- `24-NCP-UART-HW/` - NCP firmware (simpler, stack on EFR32)
- `27-Router/` - Router firmware (autonomous, no host needed)

## References

- [CPC Daemon](https://github.com/SiliconLabs/cpc-daemon)
- [`EMBERZNET-8.x-GUIDE.md`](./EMBERZNET-8.x-GUIDE.md) — how a Series 1 EFR32MG1B ends up exposing EZSP v18 via host-side `zigbeed`
- [rtl8196e-uart-bridge](https://github.com/jnilo1/rtl8196e-gateway/tree/main/3-Main-SoC-Realtek-RTL8196E/32-Kernel/files-6.18/drivers/net/rtl8196e-uart-bridge)

## License

Educational and personal use. Silicon Labs SDK components under their respective licenses.
