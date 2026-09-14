# Root Filesystem

This directory contains the root filesystem for the gateway.

> **Board- and kernel-agnostic:** the same `rootfs.bin` works for every board
> (`lidl`, `sengled-e39-g8c`) and kernel line (`6.18`, `7.2`) — board specifics
> live in the device tree and bootloader, persistent config in `/userdata`. No
> `BOARD`/`KERNEL` selection applies here.

## Overview

The root filesystem is a minimal Linux system based on:

- **BusyBox** — Essential Unix utilities in a single binary
- **Dropbear** — Lightweight SSH server
- **musl libc** — Modern, lightweight C library

The filesystem is packaged as **SquashFS** (read-only, compressed) for reliability and space efficiency.

## Key Design: Bootstrap + Symlinks

The rootfs contains only a **minimal bootstrap** in `/etc`:

| File | Type | Purpose |
|------|------|---------|
| `/etc/inittab` | Real file | Init configuration (required at boot) |
| `/etc/init.d/rcS` | Real file | Bootstrap script (mounts /userdata, runs init scripts) |
| `/etc/passwd` | Symlink | → `/userdata/etc/passwd` |
| `/etc/group` | Symlink | → `/userdata/etc/group` |
| `/etc/hostname` | Symlink | → `/userdata/etc/hostname` |
| `/etc/profile` | Symlink | → `/userdata/etc/profile` |
| `/etc/dropbear/` | Symlink | → `/userdata/etc/dropbear/` |
| `/etc/hosts` | Symlink | → `/var/hosts` (dynamic, ramfs) |
| `/etc/resolv.conf` | Symlink | → `/var/resolv.conf` (dynamic, ramfs) |

This architecture provides several benefits:

| Benefit | Description |
|---------|-------------|
| **Updatable config** | Configuration can be modified without rebuilding the read-only rootfs |
| **Persistent settings** | Changes survive rootfs updates |
| **Easy customization** | Users can tweak settings in the writable userdata partition |
| **Safe updates** | Rootfs can be reflashed without losing configuration |

## Boot Sequence

```
Kernel
  ↓
Mount SquashFS rootfs
  ↓
/sbin/init reads /etc/inittab
  ↓
/etc/init.d/rcS (bootstrap):
  1. Mount /proc, /sys, /var (ramfs), /dev/pts
  2. Create MTD device nodes if needed
  3. Mount /userdata (JFFS2 on mtdblock3)
  4. Execute /userdata/etc/init.d/S??* scripts
  ↓
System ready
```

The bootstrap script (`/etc/init.d/rcS`) is the **only** init script in the rootfs. All user services (network, SSH, applications) are started from `/userdata/etc/init.d/`.

## Contents

| Directory/File | Description |
|----------------|-------------|
| [skeleton/](https://github.com/jnilo1/rtl8196e-gateway/tree/main/3-Main-SoC-Realtek-RTL8196E/33-Rootfs/skeleton) | Base filesystem structure (etc, init scripts) |
| [busybox/](https://github.com/jnilo1/rtl8196e-gateway/tree/main/3-Main-SoC-Realtek-RTL8196E/33-Rootfs/busybox) | BusyBox build directory |
| [dropbear/](https://github.com/jnilo1/rtl8196e-gateway/tree/main/3-Main-SoC-Realtek-RTL8196E/33-Rootfs/dropbear) | Dropbear SSH build directory |
| [build_rootfs.sh](https://github.com/jnilo1/rtl8196e-gateway/blob/main/3-Main-SoC-Realtek-RTL8196E/33-Rootfs/build_rootfs.sh) | Script to assemble and package the rootfs |

## Building

```bash
# Build BusyBox
cd busybox && ./build_busybox.sh && cd ..

# Build Dropbear
cd dropbear && ./build_dropbear.sh && cd ..

# Assemble and package rootfs
./build_rootfs.sh
```

## Output

- `rootfs.bin` — Flashable SquashFS image with Realtek header (~2 MB)

## Customizing BusyBox

The BusyBox configuration can be easily customized using the interactive `menuconfig` interface:

```bash
# Default version + interactive config
./build_busybox.sh menuconfig

# Specific version + interactive config
./build_busybox.sh 1.36.1 menuconfig

# Via environment variable
BB_VER=1.36.0 ./build_busybox.sh menuconfig
```

After exiting `menuconfig`, you can choose where to save your configuration:
1. `busybox.config` — Generic base config (used as fallback)
2. `busybox-X.Y.Z.config` — Version-specific config (takes priority)
3. Both files
4. Don't save (temporary build only)

This allows you to enable/disable applets (utilities), adjust features, and optimize the binary size for your needs.

## Customizing Dropbear

Dropbear is a lightweight SSH server designed for embedded systems. The build script supports version selection:

```bash
# Default version (2026.94)
./build_dropbear.sh

# Specific version
./build_dropbear.sh 2025.89
```

The build produces a multi-call binary (`dropbearmulti`) with symlinks for `dropbear` and `dropbearkey`.

To customize Dropbear features, edit the `./configure` options in `build_dropbear.sh`. Current configuration disables zlib, utmp/wtmp logging, PAM, and shadow passwords to minimize binary size.

**Resources:**
- [Dropbear SSH](https://matt.ucc.asn.au/dropbear/dropbear.html) — Official website
- [GitHub repository](https://github.com/mkj/dropbear) — Source code and releases
- [dropbear(8)](https://man.archlinux.org/man/extra/dropbear/dropbear.8.en) — Server options and configuration
- [dropbearkey(1)](https://man.archlinux.org/man/extra/dropbear/dropbearkey.1.en) — Key generation utility

## Filesystem Structure

```
/
├── bin/                    # BusyBox symlinks
├── sbin/                   # System binaries
├── etc/                    # Configuration (minimal bootstrap + symlinks)
│   ├── inittab             # Init config (real file)
│   ├── init.d/
│   │   └── rcS             # Bootstrap script (real file)
│   ├── passwd              # -> /userdata/etc/passwd
│   ├── group               # -> /userdata/etc/group
│   ├── hostname            # -> /userdata/etc/hostname
│   ├── profile             # -> /userdata/etc/profile
│   ├── dropbear/           # -> /userdata/etc/dropbear/
│   ├── hosts               # -> /var/hosts (dynamic)
│   └── resolv.conf         # -> /var/resolv.conf (dynamic)
├── dev/                    # Device nodes
├── proc/                   # Proc filesystem (mounted at boot)
├── sys/                    # Sysfs (mounted at boot)
├── tmp/                    # -> /var/tmp
├── var/                    # Ramfs (mounted at boot)
├── userdata/               # Mount point for JFFS2 partition
└── lib/                    # Shared libraries (musl libc)
```

## Default Access

- **Root password**: Set in `/userdata/etc/passwd`
- **SSH**: Enabled on port 22 (Dropbear)
- **Serial console**: 38400 8N1 on `/dev/ttyS0`
