# rcp-stack - Rootless Zigbee Stack Manager

Systemd --user manager for the complete RCP chain:
```
RCP (EFR32) ←kernel UART bridge (TCP:8888)→ cpcd ←CPC→ zigbeed ←TCP:9999→ Z2M
```

TCP end to end, no socat: cpcd dials the gateway bridge with its native
`bus_type: TCP` (see [`../cpcd/README.md`](../cpcd/README.md)), and zigbeed
listens for Zigbee2MQTT on `127.0.0.1:9999` (see [`../zigbeed/README.md`](../zigbeed/README.md)).

## Architecture

```
                       HOST (PC / Raspberry Pi)

   ┌──────────────┐  CPC sockets    ┌──────────────┐
   │     cpcd     │────────────────▶│   zigbeed    │
   │  (TCP bus)   │  /dev/shm/cpcd/ │ (EmberZNet)  │
   └──────┬───────┘                 └──────┬───────┘
          │                                │ TCP 127.0.0.1:9999
          │                                ▼  (one client)
          │                       ┌──────────────────┐
          │                       │   Zigbee2MQTT    │
          │                       └──────────────────┘
          │
          │ TCP :8888 (in-kernel UART bridge)
          ▼
   ┌──────────────────┐
   │ RTL8196E Gateway │
   │  (RCP firmware)  │
   └──────────────────┘
```

## Prerequisites

1. **cpcd** installed (`/usr/local/bin/cpcd`) - see `../cpcd/`
2. **zigbeed** installed (`/usr/local/bin/zigbeed`) - see `../zigbeed/`
3. **In-kernel UART bridge** on the gateway (kernel 6.18 — exposes the RCP via TCP:8888, armed by S50uart_bridge at boot)
4. **Direct Ethernet cable** between host and gateway (strongly recommended)

> **Network Quality:** The CPC protocol is sensitive to latency and packet loss.
> For reliable operation, connect the gateway directly to the host with an Ethernet
> cable. Avoid WiFi, congested switches, or multiple network hops.

## Installation

```bash
# 1. Copy the main script
sudo cp bin/rcp-stack /usr/local/bin/
sudo chmod +x /usr/local/bin/rcp-stack

# 2. First run (creates config)
rcp-stack up
# -> Creates ~/.config/rcp-stack/rcp-stack.env
# -> Expected error: "Edit it with your paths, then rerun"

# 3. Edit configuration
nano ~/.config/rcp-stack/rcp-stack.env
```

## Configuration

Edit `~/.config/rcp-stack/rcp-stack.env`:

```bash
# TCP endpoint of the RCP (in-kernel UART bridge on the gateway)
RCP_ENDPOINT=tcp://192.168.1.100:8888

# Commands for each service
CPCD_COMMAND='cpcd -c "$HOME/.config/rcp-stack/cpcd.conf"'
ZIGBEED_COMMAND='zigbeed -r "spinel+cpc://$CPC_INSTANCE_NAME?iid=1&iid-list=0" -p "$ZIGBEED_LISTEN"'
Z2M_COMMAND='zigbee2mqtt'

# Optional (default values)
# ZIGBEED_LISTEN=tcp-listen://127.0.0.1:9999
# CPC_INSTANCE_NAME=cpcd_bringup
# CPC_SOCKET_DIR=/dev/shm/cpcd/cpcd_bringup
# RCP_ENDPOINT_TIMEOUT=5
```

Also copy the cpcd.conf file:
```bash
cp examples/cpcd.conf.example ~/.config/rcp-stack/cpcd.conf
# Edit if needed (binding_key_file, etc.)
```

## Usage

```bash
# Start the complete chain (checks TCP connectivity first)
rcp-stack up

# Stop cleanly
rcp-stack down

# Show status
rcp-stack status

# Full diagnostics
rcp-stack doctor
```

The `up` command verifies the RCP endpoint is reachable before starting services:
```
Checking RCP endpoint: 192.168.1.126:8888 ...
RCP endpoint 192.168.1.126:8888 is reachable
```

## Systemd Services

The `rcp-stack up` command installs and starts these services in order:

| Service | Description | Dependencies |
|---------|-------------|--------------|
| `cpcd-bringup.service` | CPC daemon (native TCP bus) | - |
| `zigbeed.service` | Zigbee daemon, EZSP listener on `$ZIGBEED_LISTEN` | cpcd |
| `zigbee2mqtt.service` | Zigbee2MQTT, started once zigbeed listens | zigbeed |

`up` waits for zigbeed's listening socket by reading `/proc/net/tcp`: it never
connects to port 9999, which serves a single client.

### Manual Service Management

```bash
# View logs
journalctl --user -u zigbeed.service -f

# Restart a service
systemctl --user restart zigbeed.service

# Enable at boot (optional)
systemctl --user enable cpcd-bringup.service \
  zigbeed.service zigbee2mqtt.service
loginctl enable-linger $USER
```

## File Structure

```
~/.config/rcp-stack/
├── rcp-stack.env          # Main configuration
├── cpcd.conf              # cpcd config
└── bin/                   # Helper scripts (auto-installed)
    ├── rcp-check-cpcd-conf
    ├── rcp-check-endpoint
    ├── rcp-check-zigbeed-conf
    ├── rcp-cleanup
    ├── rcp-ensure-dirs
    ├── rcp-run-command
    ├── rcp-wait-active
    ├── rcp-wait-cpcd
    └── rcp-wait-listen

~/.local/state/rcp-stack/
└── zigbeed/
    └── host_token.nvm     # zigbeed token (persistent)

/dev/shm/cpcd/cpcd_bringup/
├── cpcd.sock              # Main CPC socket
└── ctrl.cpcd.sock         # Control socket
```

## Zigbee2MQTT Configuration

In `zigbee2mqtt/data/configuration.yaml`:

```yaml
serial:
  port: tcp://localhost:9999
  adapter: ember
```

Zigbee2MQTT can restart freely: zigbeed and cpcd keep running and accept the
new connection. A second simultaneous client is refused, so do not point
another tool at port 9999 while Zigbee2MQTT is connected.

## Troubleshooting

### "Cannot connect to RCP endpoint"
The gateway is not reachable. Check:
- Gateway is powered on
- in-kernel UART bridge is armed on the gateway
  (`cat /sys/module/rtl8196e_uart_bridge/parameters/armed` → `1`)
- Network connectivity (ping the gateway IP)
- Correct port number in `RCP_ENDPOINT`

### "RCP_ENDPOINT is not set"
Edit `~/.config/rcp-stack/rcp-stack.env` and set `RCP_ENDPOINT`.

### "cpcd.conf not found"
Copy the example file:
```bash
cp examples/cpcd.conf.example ~/.config/rcp-stack/cpcd.conf
```

### CPC sockets not created
```bash
rcp-stack down
rm -rf /dev/shm/cpcd/cpcd_bringup
rcp-stack up
```

### Incompatible zigbeed token (v1 vs v2)
EmberZNet 8.2+ requires a v2 token:
```bash
rm ~/.local/state/rcp-stack/zigbeed/host_token.nvm
rcp-stack up
```

### Root-owned files in config directories
```bash
sudo chown -R $USER:$USER ~/.config/rcp-stack ~/.local/state/rcp-stack ~/.cpcd
```

### Upgrading from the PTY version

Earlier rcp-stack versions ran socat between zigbeed and Zigbee2MQTT. `rcp-stack up`
stops and unlinks the old `socat-zigbeed-pty.service` by itself, but refuses to
start while `ZIGBEED_COMMAND` still passes the PTY. Edit
`~/.config/rcp-stack/rcp-stack.env`:

```bash
# before
ZIGBEED_COMMAND='zigbeed ... -p "$ZIGBEED_PTY"'
# after
ZIGBEED_COMMAND='zigbeed ... -p "$ZIGBEED_LISTEN"'
```

and set Zigbee2MQTT's `serial.port` to `tcp://localhost:9999`.
