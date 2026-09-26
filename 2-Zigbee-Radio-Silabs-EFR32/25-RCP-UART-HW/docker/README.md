# Docker stacks: Zigbee (EZSP 18) and same-channel Zigbee + Thread

The stable path runs Zigbee alone. An experimental second path runs Zigbee and
Thread concurrently on a Lidl Series 1 radio, provided both networks use the
same 802.15.4 channel. See
[`cpcd-zigbeed-otbr/README.md`](./cpcd-zigbeed-otbr/README.md).

## Use Cases at a Glance

| # | Use case | Compose file | EFR32 firmware | Status |
|---|----------|-------------|----------------|--------|
| 1 | **Zigbee** (EmberZNet 8.2.2 / EZSP 18) | `docker-compose-zigbee.yml` | `./flash_efr32.sh rcp` | Tested, stable |
| 2 | **Multi-PAN** (same-channel Zigbee + Thread) | `docker-compose-multipan.yml` | `./flash_efr32.sh rcp` | Experimental on Lidl EFR32MG1B ([limits](./cpcd-zigbeed-otbr/README.md)) |

```
                          Use case 1: Zigbee
                        ┌──────────────────────┐
                        │  Zigbee2MQTT         │
         Docker host    │  + Mosquitto         │
                        │  Web UI :8080        │
                        └───────┬──────────────┘
                                │
                        ┌───────┴──────────────┐
                        │  cpcd-zigbeed        │
                        │  ├── cpcd            │
                        │  └── zigbeed (IID=1) │
                        └───────┬──────────────┘
                                │ TCP :8888
                        ┌───────┴──────────────┐
         Gateway        │  rtl8196e-uart-      │
         (RTL8196E)     │  bridge (kernel)     │
                        └───────┬──────────────┘
                                │ UART 460800
                        ┌───────┴──────────────┐
         EFR32          │  RCP (IID 1 active)  │
                        └──────────────────────┘
```

For a production single-protocol Matter-over-Thread deployment, reflash the
EFR32 with the standalone OT-RCP firmware (`./flash_efr32.sh otrcp` from the
repo root) and use the Thread Border Router compose at
`../../26-OT-RCP/docker/docker-compose-otbr-host.yml`.

---

## Requirements

### On the Gateway

- **EFR32 flashed with RCP firmware** (via `./flash_efr32.sh rcp`)
- **Gateway running kernel 6.18 or newer** with the in-kernel UART bridge
  (`rtl8196e-uart-bridge`) armed on TCP:8888 (automatic via `S50uart_bridge`
  at boot)

### On Your Computer

- Docker and Docker Compose
- Wired Ethernet to the gateway (recommended — cpcd is latency-sensitive)

---

## Use Case 1: Zigbee — EmberZNet 8.2.2 / EZSP 18

Runs Zigbee2MQTT with the `ember` adapter. The Zigbee stack (`zigbeed`,
EmberZNet 8.2.2 / EZSP 18, built from Simplicity SDK 2025.6.3) runs in the
`cpcd-zigbeed` container:

```text
EFR32 RCP -> TCP :8888 -> cpcd -> CPC socket -> zigbeed -> native TCP :9999 -> Zigbee2MQTT
```

`cpcd` uses its native `bus_type: TCP` to dial the gateway bridge directly, and
`zigbeed` listens natively on `:9999` (`tcp-listen://0.0.0.0:9999`) — no socat,
no PTY. Zigbee2MQTT connects with:

```yaml
serial:
  port: tcp://cpcd-zigbeed:9999
  adapter: ember
```

The listener serves **one** EZSP client at a time: a second connection is
closed without disturbing the active one, and when the client disconnects
(Zigbee2MQTT restart) `zigbeed` and `cpcd` keep running and the next client
reconnects to the same stack. The health check never connects to `:9999`; it
checks the `cpcd` / `zigbeed` processes and the listening socket instead.

### Quick Start

1. Edit `docker-compose-zigbee.yml` — set your gateway IP:
   ```yaml
   environment:
     - RCP_HOST=192.168.1.88
   ```

2. Start:
   ```bash
   docker compose -f docker-compose-zigbee.yml up -d
   ```

3. Wait ~60 seconds for the stack to initialize. Check:
   ```bash
   docker compose -f docker-compose-zigbee.yml logs -f cpcd-zigbeed
   # should show: "Connected to Secondary", "Secondary CPC vX.Y.Z"
   ```

4. Open http://localhost:8080

### Files

| File | Description |
|------|-------------|
| `docker-compose-zigbee.yml` | Mosquitto + cpcd-zigbeed + Zigbee2MQTT |
| `z2m/configuration.yaml` | Z2M config (adapter, MQTT) |
| `mosquitto/mosquitto.conf` | MQTT broker (anonymous, ports 1883/9001) |
| `cpcd-zigbeed/` | Dockerfile and configs for the cpcd+zigbeed container |

### Pre-built Image

```
ghcr.io/jnilo1/cpcd-zigbeed:latest
```

| Tag | cpcd | EmberZNet | EZSP |
|-----|------|-----------|------|
| `latest`, `cpcd4.5.3-ezsp18` | 4.5.3 | 8.2.2 | v18 |
| `<release>` (e.g. `4.5.2`) | 4.5.3 | 8.2.2 | v18 |

`latest` and `cpcd4.5.3-ezsp18` follow the latest release that changed the
image; the release tags are immutable. A release that leaves the image alone
publishes no new tag: use the most recent one.

### Services

| Port | Service |
|------|---------|
| 8080 | Zigbee2MQTT Web UI |
| 1883 | Mosquitto MQTT |
| 9001 | Mosquitto WebSocket |

---

## Use Case 2: Experimental same-channel multi-PAN

Runs `zigbeed` on IID 1 and OTBR on IID 2 through one `cpcd`, with IID 0
reserved for broadcast. The OTBR image is built with `OT_MULTIPAN_RCP=ON`;
without that host option, the OpenThread URL parser rejects the IID list before
contacting the RCP.

Series 1 supports this arrangement only when Zigbee and Thread share a channel.
Independent-channel Concurrent Listening remains a Series 2 requirement. The
test so far was short and used no real Thread device, so this remains
experimental. Run this compose file with a rootful native Linux Docker engine;
OTBR needs host networking, `/dev/net/tun`, IPv4/IPv6 forwarding, mDNS, and
host firewall access. Follow the
[multi-PAN guide](./cpcd-zigbeed-otbr/README.md) for the Linux host setup.

---

## Commands Reference

```bash
# Zigbee stack
docker compose -f docker-compose-zigbee.yml up -d
docker compose -f docker-compose-zigbee.yml down
docker compose -f docker-compose-zigbee.yml logs -f cpcd-zigbeed

# Experimental same-channel Zigbee + Thread stack
docker compose -f docker-compose-multipan.yml pull otbr-agent
docker compose -f docker-compose-multipan.yml up -d
docker compose -f docker-compose-multipan.yml logs -f cpcd-zigbeed otbr-agent

# Optional: reproduce the pinned OTBR image locally from the shipped sources
docker compose -f docker-compose-multipan.yml build otbr-agent

# Full reset (deletes all Zigbee data, Z2M database)
docker compose -f docker-compose-zigbee.yml down -v
```

## Troubleshooting

### "Cannot reach RCP endpoint"

1. Check the IP is correct in the compose file
2. Test connectivity: `nc -zv <gateway-ip> 8888`
3. Check the in-kernel UART bridge is armed on the gateway:
   `cat /sys/module/rtl8196e_uart_bridge/parameters/armed` → `1`

### "EZSP protocol version not supported"

Requires **Zigbee2MQTT 2.7.2 or newer** (for EZSP v18 support).

### "zigbeed entered FATAL state"

Common causes: network instability (use Ethernet, not Wi-Fi), or a gateway
bridge armed at a baud that does not match the RCP firmware (re-run
`flash_efr32.sh`, which records the right baud in `radio.conf`).

---

## References

- [cpc-daemon (Silabs)](https://github.com/SiliconLabs/cpc-daemon)
- [zigbeed / EmberZNet](https://github.com/SiliconLabs/simplicity_sdk)
- [Forum HACF thread](https://forum.hacf.fr/t/passerelle-lidl-silvercrest-firmware-open-source-zigbee-thread-pour-home-assistant/77310)
