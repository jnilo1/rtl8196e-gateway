# Build Environment

This environment is for developers who want to rebuild the RTL8196E Linux
system or EFR32 radio firmware from source.

> **Installing pre-built firmware does not require this toolchain.** Follow the
> [first-install guide](../docs/getting-started.md#3-prepare-the-computer) for
> the much smaller host-package set. The complete environment below takes about
> 45 minutes and several gigabytes.

## Choose a build method

| | Native Ubuntu / WSL2 | Docker |
| --- | --- | --- |
| Best for | Regular development, fastest I/O | Reproducible isolated setup |
| Host | Ubuntu 22.04 or Ubuntu 22.04 under WSL2 | Any **x86_64** Docker host (see below) |
| Setup time | About 45 minutes | About 45 minutes |
| Disk use | About 4 GB in the project | About 8 GB Docker image |
| Output ownership | Normal project user | Bind-mounted project files |

For Windows development, native Ubuntu 22.04 under WSL2 is normally faster and
simpler than Docker Desktop. For occasional or CI builds, Docker provides the
more reproducible boundary.

### Architecture: what actually pins the EFR32 half to x86_64

Silicon Labs publishes its tools for several platforms — Commander and `slc-cli`
both have macOS archives, and the Linux Commander download contains `aarch64`
and `aarch32` builds beside the x86_64 one. The constraint here is narrower than
"Silabs is x86-only", and worth stating precisely.

| Component | Linux x86_64 | Linux aarch64 | Note |
|---|---|---|---|
| ARM GCC 12.2 | yes | yes | `install_silabs.sh` already interpolates `uname -m` into the download URL |
| Simplicity Commander | yes | yes | `Commander_linux_aarch64_*.tar.bz` ships inside the same `SimplicityCommander-Linux.zip` we download |
| `slc-cli` | yes | **no** | it bundles its own CPython 3.10, built for x86_64 |
| Gecko SDK | — | — | source, architecture-neutral |

So on an arm64 Linux host `slc-cli` is the one real blocker for *building* EFR32
firmware. Two caveats sit next to it. `install_silabs.sh` extracts
`Commander_linux_x86_64_*` by name, so it would take the wrong build on arm64
even though the right one is in the archive it just downloaded. And its
architecture guard accepts `aarch64`, which lets that host start an install it
cannot finish.

Note also that Commander carries a **32-bit** x86 helper,
`commander/resources/cct_linux` — that is why `install_deps.sh` and the
`Dockerfile` enable the `i386` architecture, and why running the x86_64 build
under emulation on Apple Silicon does not help: Docker Desktop's x86 translation
does not cover 32-bit binaries.

The RTL8196E half carries no architecture constraint at all, because nothing
prebuilt for the host is shipped: the Lexra toolchain is built from source by
crosstool-NG and the Realtek image tools are compiled by the build scripts. An
arm64 Linux host therefore builds the complete gateway system — bootloader,
kernel, rootfs, userdata — and can flash the radio too, since `flash_efr32.sh`
drives a Python virtualenv (`universal-silabs-flasher`) rather than Commander,
and the EFR32 firmware images are committed pre-built under
`2-Zigbee-Radio-Silabs-EFR32/*/firmware/`.

For a macOS host, all of this runs in a Linux virtual machine, natively on an
Intel Mac. Silicon Labs' own macOS tools exist and can be used directly for
EFR32 work, but they are outside these build scripts, and they do not help with
the RTL8196E half: that one requires Linux whatever the architecture, because
the userdata image is built with `mkfs.jffs2`. Flashing a gateway from a VM
needs its network adapter in **bridged** mode; see the
[upgrade guide](../docs/upgrading.md#host-prerequisites).

## Native setup

```bash
git clone https://github.com/jnilo1/rtl8196e-gateway.git
cd rtl8196e-gateway/1-Build-Environment
sudo ./install_deps.sh
```

The script:

1. installs Ubuntu host packages and i386 compatibility libraries;
2. builds the patched Lexra MIPS toolchain;
3. builds the Realtek image tools;
4. downloads ARM GCC, `slc-cli`, Gecko SDK 4.5.0, and Commander;
5. writes the tools under the project directory where build scripts discover
   them automatically.

Created directories:

```text
<project>/x-tools/mips-lexra-linux-musl/
<project>/silabs-tools/
<project>/1-Build-Environment/11-realtek-tools/bin/
```

The installer must be invoked with `sudo`; it runs per-user build steps as the
invoking user so the generated trees retain useful ownership.

## Docker setup

From `1-Build-Environment`:

```bash
docker build -t rtl8196e-gateway-builder .
```

Open an interactive build shell:

```bash
docker run -it --rm \
  -v "$(pwd)/..:/workspace" \
  rtl8196e-gateway-builder
```

Or run a complete build directly:

```bash
# RTL8196E side
docker run --rm \
  -v "$(pwd)/..:/workspace" \
  rtl8196e-gateway-builder \
  /workspace/3-Main-SoC-Realtek-RTL8196E/build_rtl8196e.sh

# EFR32 side
docker run --rm \
  -v "$(pwd)/..:/workspace" \
  rtl8196e-gateway-builder \
  /workspace/2-Zigbee-Radio-Silabs-EFR32/build_efr32.sh
```

The bind mount is bidirectional, so built images appear in the normal project
directories. The container entrypoint links its internal toolchains into the
same `/workspace/x-tools` and `/workspace/silabs-tools` locations expected by
native build scripts.

If you still have the pre-v4 image name, retag it rather than rebuilding:

```bash
docker tag lidl-gateway-builder rtl8196e-gateway-builder
```

## What is installed

| Tool set | Used by | Version / source | Project location |
| --- | --- | --- | --- |
| Lexra MIPS GCC/binutils/musl | Bootloader, kernel, rootfs, userdata | GCC 15.2.0, binutils 2.45.1, musl 1.2.6 | `x-tools/mips-lexra-linux-musl/` |
| Realtek `cvimg`, `lzma`, `flash_erase` | Image packaging and device tools | Built from source | `1-Build-Environment/11-realtek-tools/bin/` |
| ARM GCC | EFR32 applications | 12.2 | `silabs-tools/arm-gnu-toolchain/` |
| Silabs `slc-cli` + Gecko SDK | EFR32 generation/build | SLC 5.11, GSDK 4.5.0 | `silabs-tools/slc_cli/`, `silabs-tools/gecko_sdk/` |
| Simplicity Commander | EFR32 inspection/recovery | Silabs pre-built tool | `silabs-tools/commander/` |

The RTL8196E uses a Lexra core without standard MIPS unaligned-access
instructions. Do not replace the supplied compiler with a generic
`mips-linux-gnu-gcc`.

EFR32 Series 1 is no longer supported by recent Silabs SDK lines. The on-chip
firmware intentionally builds against Gecko SDK 4.5.0; the modern EmberZNet 8.x
path moves the stack to host-side `zigbeed` rather than compiling 8.x for the
radio.

## Build targets

### Complete RTL8196E system

```bash
./3-Main-SoC-Realtek-RTL8196E/build_rtl8196e.sh
BOARD=sengled-e39-g8c ./3-Main-SoC-Realtek-RTL8196E/build_rtl8196e.sh
KERNEL=7.2 ./3-Main-SoC-Realtek-RTL8196E/build_rtl8196e.sh
```

Reference: [RTL8196E Linux system](../3-Main-SoC-Realtek-RTL8196E/README.md).

### EFR32 firmware

```bash
./2-Zigbee-Radio-Silabs-EFR32/build_efr32.sh
./2-Zigbee-Radio-Silabs-EFR32/build_efr32.sh ncp rcp
BOARD=sengled-e39-g8c ./2-Zigbee-Radio-Silabs-EFR32/build_efr32.sh ncp
```

Reference: [EFR32 radio firmware](../2-Zigbee-Radio-Silabs-EFR32/README.md).

Each top-level builder supports targeted work; run it with `--help` before
assuming a target name or environment variable.

## Directory map

| Path | Purpose |
| --- | --- |
| `10-lexra-toolchain/` | crosstool-ng configuration and Lexra compiler patches |
| `10-lexra-toolchain/TOOLCHAIN_UPDATE.md` | Toolchain version history and bump procedure |
| `11-realtek-tools/` | `cvimg`, legacy LZMA, and device-side flash utilities |
| `12-silabs-toolchain/` | Silabs tool download and environment setup |
| `Dockerfile` | Reproducible Ubuntu 22.04 environment |
| `install_deps.sh` | Native installation orchestrator |

## Troubleshooting

### Downloads or Docker build fail

Toolchain setup downloads several upstream archives and can fail transiently.
Retry the same command first. For a deliberately clean Docker retry:

```bash
docker build --no-cache -t rtl8196e-gateway-builder .
```

### `slc-cli` or Gecko SDK is missing

Confirm `silabs-tools/env.sh` exists and that the SDK download completed. Source
it for manual tool use:

```bash
. ./silabs-tools/env.sh
```

Normal EFR32 build scripts source it automatically.

### Toolchain not found

Keep the generated directories at their default project-relative locations or
put the compiler explicitly on `PATH`. An error that a valid MIPS binary cannot
execute may indicate missing i386 compatibility libraries rather than a missing
file.

### Git LFS content is incomplete

The Gecko SDK needs Git LFS. Verify `git lfs version`, then fetch the missing
objects in the relevant checkout. Both supported setup methods install Git LFS.
