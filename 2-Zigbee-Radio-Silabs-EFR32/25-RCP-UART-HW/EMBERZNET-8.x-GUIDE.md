# Running EmberZNet 8.x on a Series 1 radio

## Background: The Series 1 Deprecation Problem

Every board this firmware supports carries a **Silicon Labs Series 1** radio — an **EFR32MG1B** on the Lidl Silvercrest Gateway, an **EFR32MG13P** on the Sengled Smart Hub G4.

In 2024, Silicon Labs discontinued support for Series 1 chips in newer versions of their Zigbee stack:

| SDK | EmberZNet | Series 1 Support |
|-----|-----------|------------------|
| Gecko SDK 4.x | 7.5.x | ✅ Last supported |
| Simplicity SDK 2025.x | 8.x | ❌ Series 2 only |

This means users of Series 1 devices (which is every gateway here) are stuck on EmberZNet 7.5.x if running the Zigbee stack directly on the chip using the traditional NCP (Network Co-Processor) architecture.

## The Solution: RCP Architecture

We found a workaround using the **RCP (Radio Co-Processor)** architecture:

```
Traditional NCP:  Zigbee Stack runs ON the EFR32 chip
RCP Architecture: Zigbee Stack runs ON a Linux host (x86 PC, Raspberry Pi, etc.)
```

In RCP mode, the EFR32 chip only handles the 802.15.4 radio PHY/MAC layer. The EmberZNet stack runs on a separate Linux host via **zigbeed**, communicating with the RCP through **cpcd** (Co-Processor Communication daemon).

**Key insight:** The RCP firmware for Series 1 (built from Gecko SDK 4.5.0) is compatible with zigbeed from Simplicity SDK 2025.x running EmberZNet 8.x!

```
+-------------------+    UART    +-------------------+   Ethernet   +---------------------+
|  EFR32MG1B (RCP)  |   460800   |  Lidl Gateway     |    TCP/IP    |  Host (x86/ARM)     |
|                   |   baud     |  (RTL8196E)       |              |                     |
|  802.15.4 PHY/MAC |<---------->|                   |<------------>|  cpcd               |
|  + CPC Protocol   |   ttyS0    |  in-kernel UART   |   port 8888  |    |                |
|                   |            |  bridge (kernel)  |              |    v                |
|  Gecko SDK 4.5.0  |            |                   |              |  zigbeed 8.2.2      |
|                   |            |                   |              |  (EmberZNet 8.x)    |
+-------------------+            +-------------------+              |    |                |
                                                                    |    v                |
                                                                    |  Zigbee2MQTT        |
                                                                    +---------------------+
```

> **CPC transport — native TCP bus.** `cpcd` is built with a native
> `bus_type: TCP` (carried as `cpcd/tcp-bus.patch`, applied automatically by
> `build_cpcd.sh`). With it, cpcd connects directly to the gateway's in-kernel
> UART↔TCP bridge on `TCP:8888` and reconnects on its own — no intermediate
> `socat` PTY in front of cpcd. The **Docker stack** (`docker/cpcd-zigbeed/`)
> uses this directly. The **native `rcp-stack`** flow documented below still
> front-ends cpcd with a `socat` PTY (`socat-cpc-rcp`) and `uart_device_*`
> keys; moving it onto the TCP bus is a follow-up. To use the TCP bus with a
> hand-rolled cpcd.conf, set `bus_type: TCP` plus `tcp_server_address` /
> `tcp_server_port` instead of `uart_device_file` / `uart_device_baud`.

## Installation Guide

### Prerequisites

- **Host machine**: x86_64 PC, Raspberry Pi 4/5, or any Linux host
- **Lidl gateway**: Already flashed with RCP firmware; kernel 6.18 with `rtl8196e-uart-bridge` armed (default via S50uart_bridge)
- **Network**: Direct Ethernet cable between host and gateway (strongly recommended)

### Step 1: Install Build Dependencies

```bash
sudo apt install cmake build-essential socat git
```

### Step 2: Install Silicon Labs Tools

Follow the instructions in [1-Build-Environment](../../1-Build-Environment/README.md) to install slc-cli and ARM GCC toolchain:

```bash
cd 1-Build-Environment
sudo ./install_deps.sh
```

### Step 3: Build and Install cpcd

```bash
cd 2-Zigbee-Radio-Silabs-EFR32/25-RCP-UART-HW/cpcd
./build_cpcd.sh         # Clone, build, install to /usr/local
```

### Step 4: Build and Install zigbeed 8.2.2

```bash
cd 2-Zigbee-Radio-Silabs-EFR32/25-RCP-UART-HW/zigbeed-8.2.2
./build_zigbeed.sh      # Downloads SDK automatically, build, install
```

> **Note:** On first run, the script automatically downloads Simplicity SDK 2025.6.3 from GitHub (~1.5 GB).

### Step 5: Setup rcp-stack Manager

The `rcp-stack` script needs its companion files (scripts/, systemd/, examples/), so it must be run from the cloned repository.

**Option A: Create an alias** (recommended)

Add to your `~/.bashrc`:
```bash
alias rcp-stack='/path/to/rtl8196e-gateway/2-Zigbee-Radio-Silabs-EFR32/25-RCP-UART-HW/rcp-stack/bin/rcp-stack'
```

Then reload:
```bash
source ~/.bashrc
```

**Option B: Run directly from the repo**
```bash
./2-Zigbee-Radio-Silabs-EFR32/25-RCP-UART-HW/rcp-stack/bin/rcp-stack up
```

### Step 6: Configure

First run creates the config file:
```bash
rcp-stack up
# -> Creates ~/.config/rcp-stack/rcp-stack.env
# -> Copies helper scripts to ~/.config/rcp-stack/bin/
# -> Links systemd units to ~/.config/systemd/user/
# -> Will fail, prompting you to edit the config
```

Edit the configuration:
```bash
nano ~/.config/rcp-stack/rcp-stack.env
```

Set your gateway IP and commands:
```bash
RCP_ENDPOINT=tcp://192.168.1.88:8888
CPCD_COMMAND='cpcd -c "$HOME/.config/rcp-stack/cpcd.conf"'
ZIGBEED_COMMAND='zigbeed -r "spinel+cpc://$CPC_INSTANCE_NAME?iid=1&iid-list=0" -p "$ZIGBEED_PTY" -d 1 -v 0'
```

Copy the cpcd config:
```bash
cp /path/to/rtl8196e-gateway/2-Zigbee-Radio-Silabs-EFR32/25-RCP-UART-HW/rcp-stack/examples/cpcd.conf.example ~/.config/rcp-stack/cpcd.conf
```

**Important:** Edit `cpcd.conf` and set the correct baudrate (must match RCP firmware):
```yaml
uart_device_baud: 460800
```

### Step 7: Start the Stack

```bash
rcp-stack up
```

This starts the complete chain:
1. `socat-cpc-rcp` — TCP to PTY bridge
2. `cpcd` — CPC daemon
3. `zigbeed` — EmberZNet 8.2.2 stack
4. Zigbee2MQTT (if configured)

### Step 8: Configure Zigbee2MQTT

In `configuration.yaml`:
```yaml
serial:
  port: /tmp/ttyZ2M
  adapter: ember
```

## Verification

Check the rcp-stack logs:
```bash
journalctl --user -u zigbeed.service -f
```

Then start Zigbee2MQTT. You should see EmberZNet 8.2.2 [GA], build 532 (EZSP 18):
```
$ pnpm start

> zigbee2mqtt@2.7.2 start /opt/zigbee2mqtt
> node index.js

Starting Zigbee2MQTT without watchdog.
[2026-01-22 16:49:34] info:     z2m: Logging to console, file (filename: log.log)
[2026-01-22 16:49:35] info:     z2m: Starting Zigbee2MQTT version 2.7.2 (commit #3a49c957)
[2026-01-22 16:49:35] info:     z2m: Starting zigbee-herdsman (8.0.1)
[2026-01-22 16:49:35] info:     zh:ember: Using default stack config.
[2026-01-22 16:49:35] info:     zh:ember: ======== Ember Adapter Starting ========
[2026-01-22 16:49:35] info:     zh:ember:ezsp: ======== EZSP starting ========
[2026-01-22 16:49:35] info:     zh:ember:uart:ash: ======== ASH Adapter reset ========
[2026-01-22 16:49:35] info:     zh:ember:uart:ash: RTS/CTS config is off, enabling software flow control.
[2026-01-22 16:49:35] info:     zh:ember:uart:ash: Serial port opened
[2026-01-22 16:49:35] info:     zh:ember:uart:ash: ======== ASH starting ========
[2026-01-22 16:49:40] info:     zh:ember:uart:ash: ======== ASH Adapter reset ========
[2026-01-22 16:49:40] info:     zh:ember:uart:ash: ======== ASH starting ========
[2026-01-22 16:49:45] info:     zh:ember:uart:ash: ======== ASH Adapter reset ========
[2026-01-22 16:49:45] info:     zh:ember:uart:ash: ======== ASH starting ========
[2026-01-22 16:49:47] info:     zh:ember:uart:ash: ======== ASH connected ========
[2026-01-22 16:49:47] info:     zh:ember:uart:ash: ======== ASH started ========
[2026-01-22 16:49:47] info:     zh:ember:ezsp: ======== EZSP started ========
[2026-01-22 16:49:47] info:     zh:ember: Adapter version info: {"ezsp":18,"revision":"8.2.2 [GA]","build":532,"major":8,"minor":2,"patch":2,"special":0,"type":170}
[2026-01-22 16:49:47] info:     zh:ember: [STACK STATUS] Network up.
[2026-01-22 16:49:47] info:     zh:ember: [INIT TC] Adapter network matches config.
[2026-01-22 16:49:47] info:     zh:ember: [CONCENTRATOR] Started source route discovery.
[2026-01-22 16:49:47] info:     z2m: zigbee-herdsman started (resumed)
[2026-01-22 16:49:47] info:     z2m: Coordinator firmware version: '{"meta":{"build":532,"ezsp":18,"major":8,"minor":2,"patch":2,"revision":"8.2.2 [GA]","special":0,"type":170},"type":"EmberZNet"}'
```

Key indicators of success:
- `EZSP started` with protocol version **18**
- `Adapter version info` showing **8.2.2 [GA]**
- `[STACK STATUS] Network up.`
- `Coordinator firmware version` confirming **EmberZNet 8.2.2**

## Quick Reference

| Command | Description |
|---------|-------------|
| `rcp-stack up` | Start the complete chain |
| `rcp-stack down` | Stop cleanly |
| `rcp-stack status` | Show service status |
| `rcp-stack doctor` | Full diagnostics |

## Troubleshooting

### Diagnostic Commands

```bash
# Quick status of all services
rcp-stack status

# Full diagnostics (services, sockets, PTYs, config files)
rcp-stack doctor

# Detailed logs for a specific service
journalctl --user -u cpcd-bringup.service -n 50 --no-pager
journalctl --user -u zigbeed.service -n 50 --no-pager

# Follow logs in real-time
journalctl --user -u cpcd-bringup.service -f
```

### cpcd fails with "Baudrate mismatch"

**Symptom:**
```
FATAL: Baudrate mismatch (230400) on the daemon versus (460800) on the secondary
```

**Cause:** The baudrate in `cpcd.conf` doesn't match the RCP firmware.

**Solution:** Edit `~/.config/rcp-stack/cpcd.conf` and set the correct baudrate:
```yaml
uart_device_baud: 460800
```

Then restart:
```bash
rcp-stack down
rcp-stack up
```

### cpcd fails with "Start request repeated too quickly"

**Symptom:** systemd stops retrying after multiple failures.

**Solution:** Check the actual error in the logs:
```bash
journalctl --user -u cpcd-bringup.service -n 50 --no-pager
```

Then fix the underlying issue and restart:
```bash
rcp-stack down
rcp-stack up
```

### Cannot connect to RCP endpoint

**Symptom:**
```
Cannot connect to RCP endpoint 192.168.1.88:8888
```

**Checks:**
1. Gateway is powered on
2. In-kernel UART bridge is armed on the gateway
   (`cat /sys/module/rtl8196e_uart_bridge/parameters/armed` → `1`)
3. Network connectivity: `nc -zv 192.168.1.88 8888`
4. Correct IP in `~/.config/rcp-stack/rcp-stack.env`

### CPC sockets not created

**Solution:** Clean up and restart:
```bash
rcp-stack down
rm -rf /dev/shm/cpcd/cpcd_bringup
rcp-stack up
```

### zigbeed fails with token error

**Symptom:** Token version mismatch (v1 vs v2) when upgrading from EmberZNet 7.x to 8.x.

**Solution:**
```bash
rm ~/.local/state/rcp-stack/zigbeed/host_token.nvm
rcp-stack down
rcp-stack up
```

### Checking service health

A healthy stack looks like this:
```
● socat-cpc-rcp.service      active (running)
● cpcd-bringup.service       active (running)
● zigbeed.service            active (running)
● socat-zigbeed-pty.service  active (running)
```

And the logs should show:
```
Zigbeed started
RCP version: SL-OPENTHREAD/2.4.7.0_GitHub-...; EFR32; ...
Zigbeed Version: GSDK 8.2.2 - ...
```

## Important Notes

1. **Direct Ethernet connection**: The CPC protocol is sensitive to latency. Avoid WiFi or congested networks.

2. **Token migration**: If upgrading from EmberZNet 7.x to 8.x, delete the old token:
   ```bash
   rm ~/.local/state/rcp-stack/zigbeed/host_token.nvm
   ```

3. **Baud rate**: 460800 baud is the pre-built firmware default. All
   bauds up to 460800 (POSIX-capped by cpcd) run through the in-kernel
   UART bridge on kernel 6.18 (see
   [baudrate guide](README.md#baudrate-and-network-considerations)).

---

This workaround allows you to run the latest EmberZNet 8.x stack on your Lidl gateway despite Silicon Labs' Series 1 deprecation!
