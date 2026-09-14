# Bootloader RTL8196E Toolchain Port: Notes and Post-Mortem

This document merges the porting notes and post-mortem for moving the
RTL8196E bootloader from the legacy Realtek RSDK toolchain (GCC 4.6.4) to
`mips-lexra-linux-musl` (GCC 8.5.0, crosstool-NG). It focuses on:

- the bootloader architecture,
- the failures that only appeared with the new toolchain,
- why the old toolchain appeared to work,
- and the fixes that made the new toolchain stable.

## Hardware and Environment

- SoC: Realtek RTL8196E (Lexra RLX4181 / LX4380 class, MIPS32-like)
- RAM: 32 MB DDR1
- Flash: 16 MB SPI (GD25Q128)
- Console: UART @ 38400
- Original toolchain: Realtek RSDK (GCC 4.6.4)
- New toolchain: `mips-lexra-linux-musl` (GCC 8.5.0, crosstool-NG)

## Boot Architecture (Two Stages)

```
SPI Flash
  |
  v
btcode/start.S @ 0xBFC00000
  - Reset vector, early init
  - DDR init, UART init ("Booting...")
  - Cache flush
  - Copy piggy.bin -> DRAM (0x80100000)
  - Jump to 0x80100000
  |
  v
btcode/bootload.c @ 0x80100000 (piggy = bootload + boot.img.lzma)
  - LZMA decompress boot.img -> 0x80400000
  - Cache maintenance (DWB + DInval + IInval via CP0 $20 CCTL)
  - Jump to 0x80400000
  |
  v
boot/head.S @ 0x80400000
  - CP0 init, cache ops
  - BSS clear
  - Stack init
  - init_arch -> start_kernel -> monitor
```

Linker script:
- `boot/ld.script`: base 0x80400000 (boot.img)

## RAMTEST vs Real Bootloader

RAMTEST is a special image that runs from DRAM and reuses the exact same
`bootload.c` LZMA path as the real bootloader, but skips the flash reset
vector stage. It is used to validate the new toolchain without risking a
brick.

Main differences:

- RAMTEST does NOT execute `btcode/start.S` from flash (so DDR init and
  early reset vector behavior are not exercised).
- RAMTEST is loaded via TFTP into DRAM (typically 0x80100000) and then
  jumped to with the boot monitor.
- RAMTEST still runs `bootload.c`, LZMA decompression, cache flush, and
  the jump into `head.S` exactly like the real bootloader.

This makes RAMTEST a safe way to validate codegen, LZMA, cache handling,
varargs fixes, and the transition into the main boot stage.

## Post-Mortem: Why Old Toolchain Worked and New One Failed

The new toolchain exposed latent assumptions that the old toolchain happened
not to violate. These issues are normal in bare-metal code where the ABI,
codegen, and object layout must be tightly controlled.

### 1) `$gp`-relative small data in `btcode` (critical hang)

Symptom: hang immediately after "Booting..." when entering `bootload.c`.

Root cause: GCC 8.5 places small globals in `.sdata` and accesses them
relative to `$gp`. `btcode/start.S` never initializes `$gp`, so these
accesses hit garbage.

Why old toolchain worked:
- GCC 4.6.4 / RSDK defaults either did not use `.sdata` for this code or
  used a different `-G` threshold that avoided `$gp`.

Fix:
- Disable small-data sections in `btcode`:
  `-G 0 -ffreestanding -fno-common`

### 2) Inline semantics (GNU89 vs C99)

Symptom: missing symbols or link errors.

Root cause: GCC 8 defaults to C99 inline semantics. Code assumed GNU89.

Why old toolchain worked:
- GCC 4.6.4 defaulted to GNU89 inline.

Fix:
- Initially worked around with `-fgnu89-inline`; later removed in favor of
  proper C99 inline semantics.

### 3) `.MIPS.abiflags` corrupting raw binaries

Symptom: `objcopy -Obinary` produced shifted/corrupted images.

Root cause: GCC 8 emits `.MIPS.abiflags`, which can be appended to the raw
binary output.

Why old toolchain worked:
- GCC 4.6.4 did not emit this section.

Fix:
- Strip it in `objcopy`:
  `objcopy -Obinary -R .MIPS.abiflags <elf> <bin>`

### 4) LZMA decompression cache coherency

Symptom: LZMA completes, then hang when jumping into decompressed code.

Root cause: LZMA writes via cached KSEG0. I-cache still contained stale
instructions.

Why old toolchain worked:
- Older codegen/behavior likely avoided the stale I-cache path or happened
  to flush it implicitly.

Fix:
- Explicit D-cache writeback + invalidate, then I-cache invalidate before
  the jump.

### 5) BSS clear loop timing (Lexra sensitivity)

Symptom: hang during BSS clear in `head.S`.

Root cause: tight store loop plus new scheduling triggered a hardware timing
corner case on Lexra.

Why old toolchain worked:
- GCC 4.6.4 emitted a slightly different sequence that avoided the issue.

Fix:
- Slow down the loop with a readback and small delay (NOPs).

### 6) `prom_printf` / `vsprintf` ABI and NULL-buffer crash (critical)

Symptom: exceptions at the first `prom_printf` under GCC 8.

Root cause:
- Legacy `vsprintf` used a K&R varargs hack that assumes stack-passed args.
- GCC 8 uses the standard MIPS ABI (args in registers).
- The legacy implementation also wrote a '\0' terminator even when
  `buf == NULL`, causing a write to address 0.

Why old toolchain worked:
- Old ABI and codegen made the K&R hack appear to work.
- The NULL write may have been masked by timing or layout.

Fix:
- Replace `vsprintf/prom_printf/dprintf` with a freestanding,
  `va_list`-correct implementation.
- Do not write a terminator when `buf == NULL`.

### 7) Toolchain prefix and `-march` flags

Symptom: wrong toolchain or invalid flags.

Root cause: RSDK used `mips-linux-` prefix and non-standard
`-march=4181/5281`.

Why old toolchain worked:
- It accepted the RSDK-specific flags.

Fix:
- Use `CROSS ?= mips-lexra-linux-musl-`.
- Remove hard-coded `-march` when the toolchain default matches Lexra.

## Notes on Critical Flags

- `-G 0`: disables `.sdata/.sbss` and avoids `$gp` usage in btcode.
- `-ffreestanding`: disables hosted assumptions in the compiler. Since V3.1 it
  is also what keeps `<stdint.h>`/`<stddef.h>` the compiler's own
  (`stdint-gcc.h`) rather than the sysroot's: the loader owns every other
  header it uses, and `build_bootloader.sh` fails on any sysroot path in the
  `-MD` dependency files.
- `-fno-common`: avoids implicit common symbols; forces real definitions.
- `-fgnu89-inline`: was used early in the port; later removed in favor of
  C99 inline semantics.

## Validation Strategy (Why We Trust It Now)

- Added a RAMTEST image to validate LZMA, cache flush, and jump without
  flashing.
- Confirmed normal boot output after removing debug markers.
- Built and flashed a new bootloader with the new toolchain and verified
  full boot sequence.

## Build Order and Reproducibility

Recommended build order:

1. Build `boot` to generate `boot.img`.
2. Build `btcode` to integrate `boot.img` and generate `boot.bin`.

Reproducibility:
- `BOOT_CODE_TIME_OVERRIDE` can be used to pin the banner timestamp and
  produce byte-identical outputs.

## Headers (V3.1)

The Linux 2.4 `asm/` and `linux/` headers the RSDK shipped were pruned and then
removed (2026-09-12). The loader's own headers
carry everything: `boot_asm.h` (CPU — also included by `btcode/start.S` and
`piggy.S`), `boot_soc.h` (SoC register map and accessors), `boot_irq.h`
(`cli`/`sti`, `irqaction`), `boot_common.h` (gcc's `<stdint.h>`/`<stddef.h>`),
`stdlib.h` (what `libc.c` and `calloc.c` provide), `swcore_regs.h` / `swcore.h`
(switch registers and driver API) and `boot_net.h` (frame formats, eth/TFTP API).

## File Locations of Key Logic

- Stage 1: `btcode/start.S`
- Stage 2: `btcode/bootload.c`
- LZMA: `btcode/LzmaDecode.c`
- Core boot: `boot/head.S`
- `init_arch`: `boot/arch.c`
- `start_kernel`: `boot/main.c`
