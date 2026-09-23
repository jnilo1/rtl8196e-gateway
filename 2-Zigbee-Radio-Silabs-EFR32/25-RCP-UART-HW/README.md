# RCP: modern Zigbee and Zigbee + Thread on a Series 1 radio

This directory turns the gateway's EFR32 into a **Radio Co-Processor (RCP)** and
provides the host software that runs the protocol stacks on a PC or Raspberry Pi:

- **Zigbee with EmberZNet 8.2.2 / EZSP v18**, although Silicon Labs stopped
  shipping on-chip stacks for this Series 1 chip at EmberZNet 7.5.1;
- **experimental same-channel Zigbee + Thread** (multi-PAN) on the same radio.

The EFR32 only handles the 802.15.4 radio. The gateway forwards its UART to
TCP port 8888, and the Zigbee stack (`zigbeed`) runs on your host behind the
CPC daemon (`cpcd`).

## Choose your use case

| Use case | Status | Host stack |
|---|---|---|
| **Zigbee** — Zigbee2MQTT / ZHA with EZSP v18 | Stable | [`docker/docker-compose-zigbee.yml`](./docker/README.md) |
| **Zigbee + Thread on the same channel** | Experimental (Lidl only) | [`docker/docker-compose-multipan.yml`](./docker/cpcd-zigbeed-otbr/README.md) |

Not what you need?

- A Zigbee coordinator with the fewest moving parts (EZSP v13, stack on the
  chip): [NCP firmware](../24-NCP-UART-HW/README.md).
- A production Thread Border Router: [OT-RCP firmware](../26-OT-RCP/README.md).
- The full comparison of radio modes: [Choose a radio mode](../../docs/radio-options.md).

## How it fits together

```
 EFR32 (RCP)           Gateway (RTL8196E)         Host (x86 / ARM)
+--------------+ UART +-------------------+ TCP  +----------------------------------------+
| 802.15.4     |<---->| in-kernel         |<---->| cpcd                                   |
| PHY/MAC      |      | UART<->TCP bridge | 8888 |  |- zigbeed (IID 1)                    |
| CPC protocol |      |                   |      |  |    '- TCP 9999 -> Zigbee2MQTT / ZHA |
|              |      |                   |      |  '- otbr-agent (IID 2, multi-PAN only) |
+--------------+      +-------------------+      +----------------------------------------+
```

Every link is TCP: `cpcd` dials the gateway bridge directly (native TCP bus),
and `zigbeed` listens for its single EZSP client on port 9999 — no socat, no
PTY, with Docker as with the native [`rcp-stack`](./rcp-stack/README.md).

Because the stack lives on the host, updating Zigbee means swapping `zigbeed`;
the EFR32 firmware stays untouched.

## Step 1 — Flash the RCP firmware

From the repository root:

```bash
./flash_efr32.sh -y rcp                 # Lidl, 460800 baud, gateway from gateway.env
./flash_efr32.sh -y -g 10.0.0.5 rcp     # explicit gateway address
```

The script puts the EFR32 into its bootloader, uploads the image over the
gateway, records `FIRMWARE=rcp` and `FIRMWARE_BAUD=<baud>` in
`/userdata/etc/radio.conf`, and reboots. At boot, `S50uart_bridge` arms the
TCP:8888 bridge at that baud; nothing else needs configuring on the gateway.
See the [`radio.conf` key reference](../../3-Main-SoC-Realtek-RTL8196E/34-Userdata/README.md#radioconf-keys-full-reference).

> **Sengled G4:** no G4 RCP image is shipped, because none has ever run on a
> G4. The G4 has no RTS/CTS wiring and CPC has no software flow control, so the
> image must run without flow control at 230400, the rate measured as reliable
> on that board. Build it, then flash it:
>
> ```bash
> BOARD=sengled-e39-g8c ./2-Zigbee-Radio-Silabs-EFR32/25-RCP-UART-HW/build_rcp.sh 230400
> BOARD=sengled-e39-g8c ./flash_efr32.sh -y rcp
> ```
>
> Board details: [`../boards/README.md`](../boards/README.md).

## Step 2 — Start the host stack

### With Docker (recommended)

```bash
cd 2-Zigbee-Radio-Silabs-EFR32/25-RCP-UART-HW/docker
# set RCP_HOST to your gateway's address in the compose file, then:
docker compose -f docker-compose-zigbee.yml up -d
```

Zigbee2MQTT's web UI comes up on port 8080. The image
`ghcr.io/jnilo1/cpcd-zigbeed` is published for amd64 and arm64.
[`docker/README.md`](./docker/README.md) covers both compose files, the
transport options and troubleshooting.

For Zigbee + Thread, follow the
[multi-PAN guide](./docker/cpcd-zigbeed-otbr/README.md): it needs a native
Linux Docker host, a few sysctls, and both networks on the **same channel**.

### Without Docker

[`rcp-stack/`](./rcp-stack/README.md) runs the same chain as rootless systemd
user services, with `rcp-stack up | down | status | doctor`. It needs `cpcd`
and `zigbeed` installed on the host — see [Build from source](#build-from-source).

### Zigbee2MQTT settings

```yaml
serial:
  port: tcp://cpcd-zigbeed:9999   # Docker; tcp://localhost:9999 for a native zigbeed
  adapter: ember
```

## Baud rate

| Board | Baud | Image |
|---|---|---|
| Lidl (RTS/CTS) | **460800** | `firmware/rcp-uart-802154-460800-hw.gbl` (prebuilt) |
| Sengled G4 (no flow control) | **230400** | build it yourself (see Step 1) |

`cpcd` accepts only standard POSIX rates, so 460800 is the ceiling for RCP. The
higher rates the gateway supports (691200, 892857) remain available to the NCP
and OT-RCP firmwares, which do not use `cpcd`. A Zigbee network sustains at
most about 25 KB/s over the air; 230400 already carries more than that.

To check the link, read the error counters on the gateway:
`cat /proc/tty/driver/serial` (`oe:` = overruns, `fe:` = framing errors).

## Network

CPC is sensitive to latency: connect the host to the gateway over wired
Ethernet, and avoid Wi-Fi bridges and congested switches between them.

## Troubleshooting

| Problem | Check |
|---|---|
| Flash fails or times out | `nc -zv <gateway-ip> 8888`; close other clients of port 8888 first (it accepts one) |
| `cpcd` does not connect | `tcp_server_address` / `tcp_server_port` in `cpcd.conf` (Docker: `RCP_HOST`) point at the gateway |
| `cpcd` reports a protocol mismatch | the RCP image is built from GSDK 4.5.0 (CPC protocol v5); reflash it with `flash_efr32.sh` |
| Frequent disconnects | wired Ethernet, no Wi-Fi bridge |
| `zigbeed` does not start | `cpcd` must be up first: `rcp-stack status`, or the `cpcd-zigbeed` container logs |

A wrong firmware is always recoverable: the bootloader is never overwritten by
an application flash.

## Build from source

Most users never need this — the firmware and the Docker image are prebuilt.

- **RCP firmware:** `./build_rcp.sh [baud]` (default 460800; 115200 and
  230400 also build) writes `firmware/rcp-uart-802154-<baud>-<flow>[-<board>].gbl`. It needs the Silicon
  Labs toolchain from
  [`1-Build-Environment/`](../../1-Build-Environment/README.md) (`12-silabs-toolchain/install_silabs.sh`).
  The UART pins, the 1-second boot delay for the gateway and the disabled CPC
  security (saves ~45 KB of flash) live in `patches/`. `BOARD=` selects the
  target board.
- **cpcd:** [`cpcd/`](./cpcd/README.md) — cpc-daemon 4.5.3 plus the
  native TCP bus patch.
- **zigbeed:** [`zigbeed/`](./zigbeed/README.md) — Simplicity SDK 2025.6.3,
  with the project's PTY / TCP transport.
- **Docker image:** `docker/cpcd-zigbeed/Dockerfile.multiarch`, built by CI.

Why a GSDK 4.5.0 radio image works with an EmberZNet 8.2.2 host stack is
explained in [`EMBERZNET-8.x-GUIDE.md`](./EMBERZNET-8.x-GUIDE.md).

## Directory layout

```
25-RCP-UART-HW/
├── firmware/            prebuilt RCP image (Lidl, 460800)
├── build_rcp.sh         RCP firmware build
├── patches/             RCP project overlay (slcp, UART pins, boot delay, CPC security)
├── docker/              Docker stacks
│   ├── docker-compose-zigbee.yml      Zigbee (stable)
│   ├── docker-compose-multipan.yml    Zigbee + Thread (experimental)
│   ├── cpcd-zigbeed/                  cpcd + zigbeed image
│   └── cpcd-zigbeed-otbr/             multi-PAN OTBR image and guide
├── rcp-stack/           native (non-Docker) service manager
├── cpcd/                cpcd build + native TCP bus patch
├── zigbeed/             zigbeed build + project EZSP transport
├── EMBERZNET-8.x-GUIDE.md   background: EmberZNet 8.x on a Series 1 radio
└── experimental/        research only — BLE HCI prototype for the G4, never run on hardware
```

## References

- [cpc-daemon](https://github.com/SiliconLabs/cpc-daemon)
- [rtl8196e-uart-bridge](../../3-Main-SoC-Realtek-RTL8196E/32-Kernel/files-6.18/drivers/net/rtl8196e-uart-bridge/README.md) — the gateway's UART↔TCP bridge

## License

Educational and personal use. Silicon Labs SDK components under their respective licenses.
