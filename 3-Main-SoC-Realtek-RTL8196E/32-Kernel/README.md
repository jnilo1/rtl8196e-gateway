# Linux Kernel for RTL8196E (6.18 + 7.2)

This directory contains everything needed to build a modern Linux kernel for the Realtek RTL8196E gateway.

**Two kernel lines** are shipped and selectable with the `KERNEL` environment variable
(default `6.18`):

| `KERNEL` | Version | Sources |
|----------|---------|---------|
| `6.18` *(default)* | [Linux 6.18.51](https://cdn.kernel.org/pub/linux/kernel/v6.x/) — stable 6.18.x LTS family | `patches-6.18/`, `files-6.18/`, `config-6.18-realtek.txt` |
| `7.2` | [Linux 7.2.5](https://cdn.kernel.org/pub/linux/kernel/v7.x/) | `patches-7.2/`, `files-7.2/`, `config-7.2-realtek.txt` |

The two lines coexist — each has its own patch/overlay/config triplet and its own pre-built
images. A Lidl user who sets nothing builds and flashes the `6.18` line exactly as before.

## Why Linux 6.18?

Linux 6.18 is an **[LTS](https://www.kernel.org/category/releases.html)** release, maintained upstream for several years. It brings a modern kernel surface to the gateway while remaining practical to cross-compile against the project's Lexra MIPS toolchain. Compared to legacy 3.10 / 4.14 / 5.10 lines, 6.18 gives us current driver APIs, recent security hardening defaults, and the in-kernel UART↔TCP bridge (`rtl8196e-uart-bridge`) used for the Zigbee radio path.

The `build_kernel.sh` script pins a specific 6.18.x stable release via `KERNEL_VERSION` and applies our local `patches-6.18/` on top. Bumping to a newer point release is a one-line edit — the patches, overlay and config are keyed on the 6.18 family, not the point version.

## The Porting Challenge

Porting to modern Linux on the RTL8196E (Lexra RLX4181, MIPS-I class) was a **significant undertaking**:

| Challenge | Description |
|-----------|-------------|
| **Ethernet driver rewrite** | The Realtek SDK driver used obsolete APIs. It was completely rewritten to comply with modern kernel networking standards (NAPI, phylib, devicetree). |
| **Lexra CPU support** | The Lexra MIPS variant lacks `ll/sc` in hardware. Required kernel-side emulation (`simulate_llsc`) plus atomics/memory-barrier patches. |
| **Platform code cleanup** | Realtek SDK platform code was heavily tied to old kernel internals. Board support was rewritten around modern devicetree and platform drivers. |
| **Build system changes** | Successive Kbuild generations moved enough code around that the overlay/patch set had to be regenerated against 6.x. |

The result is a clean, maintainable kernel that can be updated to newer 6.18.x point releases with minimal friction.

## Contents

| Directory/File | Description |
|----------------|-------------|
| [`patches-6.18/`](https://github.com/jnilo1/rtl8196e-gateway/tree/main/3-Main-SoC-Realtek-RTL8196E/32-Kernel/patches-6.18) · `patches-7.2/` | Patches to apply on vanilla Linux 6.18 / 7.2 |
| [`files-6.18/`](https://github.com/jnilo1/rtl8196e-gateway/tree/main/3-Main-SoC-Realtek-RTL8196E/32-Kernel/files-6.18) · `files-7.2/` | New files to add to the kernel tree (Realtek platform support, custom drivers) |
| [`config-6.18-realtek.txt`](https://github.com/jnilo1/rtl8196e-gateway/blob/main/3-Main-SoC-Realtek-RTL8196E/32-Kernel/config-6.18-realtek.txt) · `config-7.2-realtek.txt` | Kernel configuration (one per line) |
| `kernel-img/<board>/kernel-<line>.img` | Pre-built flashable images, one per (board, kernel) pair |
| [`build_kernel.sh`](https://github.com/jnilo1/rtl8196e-gateway/blob/main/3-Main-SoC-Realtek-RTL8196E/32-Kernel/build_kernel.sh) | Build script |
| [`tools/`](tools/README.md) | Optional on-gateway kernel diagnostic tools |

## Building

```bash
./build_kernel.sh [clean|menuconfig|olddefconfig|vmlinux]   # 6.18 / lidl (defaults)
KERNEL=7.2 ./build_kernel.sh                                # build the 7.2 line
BOARD=sengled-e39-g8c ./build_kernel.sh                     # build for the Sengled G4
```

### Options

| Option | Description |
|--------|-------------|
| *(none)* | Normal build: download sources if needed, apply patches, compile, package |
| `clean` | Remove kernel source directory and rebuild from scratch |
| `menuconfig` | Run kernel menuconfig interactively |
| `olddefconfig` | Update `.config` non-interactively against the current Kconfig |
| `vmlinux` | Build `vmlinux` only (skip packaging) |

### Board and kernel selection

Two environment variables pick what `build_kernel.sh` produces; both default to
the Lidl 6.18 build, and the output lands in `kernel-img/<board>/kernel-<line>.img`:

| Variable | Default | Values |
|----------|---------|--------|
| `BOARD`  | `lidl`  | `lidl`, `sengled-e39-g8c` (Sengled Smart Hub G4) |
| `KERNEL` | `6.18`  | `6.18`, `7.2` |

```bash
./build_kernel.sh                                  # lidl / 6.18  → kernel-img/lidl/kernel-6.18.img
KERNEL=7.2 ./build_kernel.sh                       # lidl / 7.2
BOARD=sengled-e39-g8c ./build_kernel.sh            # G4 / 6.18
BOARD=sengled-e39-g8c KERNEL=7.2 ./build_kernel.sh # G4 / 7.2
```

`BOARD` selects the device tree built into the image. Porting to another RTL8196E
board means adding an `rtl8196e-<board>.dts`, one Kconfig entry and one Makefile
line — the recipe is documented in `files-<line>/arch/mips/boot/dts/realtek/Makefile`
and in the devicetree Kconfig choice (`files-<line>/arch/mips/realtek/Kconfig`).

### Build process

The script will:
1. Download Linux 6.18.x source (if not present)
2. Apply all patches from `patches-6.18/`
3. Overlay Realtek platform files from `files-<line>/`
4. Compile the kernel
5. Apply the exact point release's versioned I-MEM policy, when present
6. Verify its local holes, SRAM budget and runtime-patching safety
7. Package the compressed kernel image (zboot) into `kernel-img/<board>/kernel-<line>.img`, ready to flash

The shipped 6.18.51 and 7.2.5 policies live under `scripts/imem/policies/`.
`IMEM_POLICY_DISABLE=1` is an experimental escape hatch and requires a clean build tree;
normal production builds always apply and verify the matching policy.

**Requirements**: [Toolchain](../../1-Build-Environment/README.md) must be built first.

## Output

- `kernel-img/<board>/kernel-<line>.img` — Flashable kernel image with Realtek
  header (~1.4 MB). The four shipped pre-built images are
  `kernel-img/{lidl,sengled-e39-g8c}/kernel-{6.18,7.2}.img`.

## Technical Details

### Build process (zboot)

The kernel uses the in-tree `arch/mips/boot/compressed/` (zboot) decompressor — no external LZMA tool or loader is needed.

1. Compile kernel → `vmlinux`
2. zboot compresses the kernel with LZMA and prepends a small decompressor → `vmlinuz` (ELF)
3. Strip to raw binary → `vmlinuz.bin`
4. Add Realtek header (cvimg) → `kernel-img/<board>/kernel-<line>.img`

### Key patches

- Lexra MIPS support (kernel-side `ll/sc`/`sync` emulation, atomics, memory barriers)
- RTL8196E SoC and board support (devicetree, platform drivers)
- SPI flash driver, GPIO bank, LEDs (gpio-leds + PWM brightness modes)
- From-scratch Ethernet driver (`rtl8196e-eth`) targeting modern APIs
- In-kernel UART↔TCP bridge (`rtl8196e-uart-bridge`) for the Zigbee radio path
- 8250-compatible UART (`8250_rtl819x`), hardware timer clocksource (`timer-rtl819x`)

## 🙏 Credits

The kernel platform support traces back to early work by [Gaspare Bruno](https://github.com/ggbruno) on the Realtek target:
- [ggbruno/openwrt — Realtek branch](https://github.com/ggbruno/openwrt/tree/Realtek/target/linux/realtek)

Those patches (originally for Linux 4.14) were heavily reworked for 5.10, then regenerated against 6.18 as part of the v3.0 rebase.

## 🔗 References

- [Linux 6.18](https://kernel.org/)
- [Lexra processors](https://en.wikipedia.org/wiki/Lexra)
