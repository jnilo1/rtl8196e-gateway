# cpcd - CPC Daemon

Build script for cpcd v4.6.2 from Silicon Labs.
Portable: works on x86_64, ARM64 (Raspberry Pi 4/5), etc.

## Prerequisites

```bash
sudo apt install cmake build-essential
```

## Build and Install

```bash
./build_cpcd.sh              # Build + prompt (TTY) or local (non-TTY)
./build_cpcd.sh --local      # Build + install to /usr/local
./build_cpcd.sh --deb        # Build + generate .deb (/usr)
./build_cpcd.sh clean        # Remove source
```

## Configuration

`build_cpcd.sh` applies [`tcp-bus.patch`](./tcp-bus.patch), which adds a native
`bus_type: TCP` to cpcd. With it, cpcd connects **directly** to the gateway's
in-kernel UART↔TCP bridge and owns its own reconnection — no `socat` PTY shim.

Edit `/usr/local/etc/cpcd.conf`:

```yaml
bus_type: TCP
tcp_server_address: 192.168.1.88   # gateway IP
tcp_server_port: 8888              # in-kernel UART bridge port (S50uart_bridge)
disable_encryption: true           # RCP firmware ships with CPC security off
```

No baud here — the UART baud lives on the gateway side (the bridge), so the
host doesn't need to know it.

### Fallback: classic UART bus over a socat PTY

Stock cpcd can only `open()` a real serial device, so the UART bus needs a
local PTY fed by `socat` (it cannot take a `tcp://` URL directly):

```bash
socat pty,raw,echo=0,link=/tmp/ttyCpcRcp tcp:192.168.1.88:8888
```

```yaml
bus_type: UART
uart_device_file: /tmp/ttyCpcRcp
uart_device_baud: 460800           # must match the RCP firmware baud
uart_hardflow: true
```

## Usage

```bash
cpcd -c /usr/local/etc/cpcd.conf
```
