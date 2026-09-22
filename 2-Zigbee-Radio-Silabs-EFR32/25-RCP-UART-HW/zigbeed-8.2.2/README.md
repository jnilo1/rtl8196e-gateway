# zigbeed - SiSDK 2025.6.3 / EZSP 18

Build script for zigbeed from Simplicity SDK 2025.6.3.
Portable: works on x86_64, ARM64 (Raspberry Pi 4/5), ARM32.

## Why This Version?

The Gecko SDK 4.5.0 produces zigbeed with EmberZNet 7.5.1 (EZSP 13).
The Simplicity SDK 2025.6.3 produces zigbeed with runtime EmberZNet 8.2.2 [GA], build 532 (EZSP 18).

| Directory | SDK | EmberZNet | EZSP |
|-----------|-----|-----------|------|
| `../zigbeed/` | Gecko SDK 4.5.0 | 7.5.1 | 13 |
| `zigbeed-8.2.2/` (legacy directory name) | Simplicity SDK 2025.6.3 | 8.2.2 [GA], build 532 | 18 |

## Prerequisites

1. **slc-cli** installed in `silabs-tools/slc_cli` or in PATH (via `1-Build-Environment/`)
2. **cpcd** installed (provides libcpc)

```bash
# Install cpcd first
cd ../cpcd && ./build_cpcd.sh
```

> **Note:** The Simplicity SDK 2025.6.3 is downloaded automatically from GitHub on first build.

## Build and Install

```bash
./build_zigbeed.sh         # Build and install to /usr/local/bin
./build_zigbeed.sh clean   # Clean build directory
```

**Note**: Stop any running zigbeed before installing:
```bash
pkill zigbeed
# or via systemd
systemctl --user stop zigbeed.service
```

## Architecture Support

| Architecture | `uname -m` | SDK libs |
|--------------|------------|----------|
| PC 64-bit | x86_64 | x86-64 |
| Raspberry Pi 4/5 64-bit | aarch64 | arm64v8 |
| Raspberry Pi 32-bit | armv7l | arm32v7 |

The script auto-detects your architecture and patches the Makefile accordingly.

## How It Works

1. **Download SDK** from GitHub if not present (shallow clone, ~1.5 GB)
2. **Generate project** using `slc generate` with architecture-specific components
3. **Replace SDK copy** with symlink (slc copies partial headers)
4. **Build** using the generated Makefile
5. **Install** to `/usr/local/bin/`

### slc generate command

The script uses the same approach as [Nerivec's multiprotocol-builder](https://github.com/Nerivec/silabs-multiprotocol-builder):

```bash
slc generate zigbeed.slcp \
    --with=zigbee_x86_64,linux_arch_64 \
    --without=zigbee_recommended_linux_arch
```

The `--with` parameter tells slc to include architecture-specific libraries automatically.

## Adapter provenance

The PTY and native TCP transport implementation in `serial_adapter_posix.c`
belongs to this project. Its OpenThread mainloop integration is adapted from
OpenThread's BSD-3-Clause licensed
[`src/posix/main.c`](https://github.com/openthread/openthread/blob/e04ff192755a22b36825e3411c6a4d02e26a2860/src/posix/main.c);
the applicable OpenThread copyright and license notice is retained in the
source file.

`THIRD_PARTY_NOTICES` carries the same notice with binary distributions. It is
installed under `/usr/share/doc/zigbeed/` in the Debian package and container
image (and under `/usr/local/share/doc/zigbeed/` for local installations).

The Zigbeed serial adapter ABI comes from Simplicity SDK headers available at
build time. `install_serial_adapter.sh` substitutes the project adapter into
the generated build tree and removes the SDK source path from the generated
makefile. Silicon Labs' `serial_adapter.c` is not shipped in this repository.

## Usage

```bash
# With CPC daemon (recommended)
zigbeed -r "spinel+cpc://cpcd_bringup?iid=1&iid-list=0" -p /tmp/ttyZigbeed

# Native TCP listener (local-only)
zigbeed -r "spinel+cpc://cpcd_bringup?iid=1&iid-list=0" -p tcp-listen://127.0.0.1:9999
```

## EZSP transports

PTY remains supported with `-p /tmp/ttyZigbeed`. Native TCP uses the explicit
`tcp-listen://HOST:PORT` form, for example `tcp-listen://127.0.0.1:9999` on a
local host or `tcp-listen://0.0.0.0:9999` in the `cpcd-zigbeed` container.
For Docker/Compose, Zigbee2MQTT uses `tcp://cpcd-zigbeed:9999`.
The `cpcd-zigbeed` image selects this native listener by default with
`ZIGBEED_TRANSPORT=tcp`; `ZIGBEED_TRANSPORT=pty` retains the PTY+socat
compatibility path.

## Zigbee2MQTT Configuration

```yaml
# Local/native host
serial:
  port: tcp://localhost:9999
  adapter: ember
  baudrate: 460800
```

```yaml
# Docker/Compose
serial:
  port: tcp://cpcd-zigbeed:9999
  adapter: ember
  baudrate: 460800
```

```yaml
# PTY
serial:
  port: /tmp/ttyZ2M
  adapter: ember
  baudrate: 460800
```

## Expected Z2M Log

```
[2026-01-10 19:49:06] info: zh:ember: ======== EZSP started ========
[2026-01-10 19:49:06] info: zh:ember: Adapter EZSP protocol version (18)
[2026-01-10 19:49:07] info: zh:ember: [STACK STATUS] Network up.
[2026-01-10 19:49:07] info: z2m: Coordinator firmware version: '{"meta":{"build":532,"ezsp":18,"major":8,"minor":2,"patch":2,"revision":"8.2.2 [GA]"}}'
```

## Troubleshooting

### "Text file busy" on install
Stop running zigbeed first:
```bash
pkill zigbeed
```

### Wrong architecture libraries
The `--with` parameter ensures correct architecture. If you still see errors, verify your architecture detection:
```bash
uname -m  # Should be x86_64, aarch64, or armv7l
```

### Missing headers (sl_slist.h, etc.)
The SDK symlink must point to the full SDK, not a partial copy. The script handles this by removing the slc-generated partial copy and creating a symlink.
