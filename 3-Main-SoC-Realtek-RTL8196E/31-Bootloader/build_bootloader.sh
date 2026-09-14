#!/bin/bash
# build_bootloader.sh — Build the RTL8196E bootloader
#
# Original bootloader sources and build flow:
#   Copyright (C) Realtek Semiconductor Corp.
#
# Adapted and simplified for RTL8196E-only builds:
#   J. Nilo - November 2025
#
# Uses the Lexra/musl toolchain from the project x-tools directory.
#
# Two variants are built, each in its own object directory:
#   - boot:    production flash image            -> */build/
#   - ramtest: RAM-test image (RAMTEST_TRACE)    -> */build-ramtest/
#
# Outputs:
#   boot-img/<board>/boot.bin     - flash image (per-board slot, committed)
#   btcode/build/boot.bin         - the same flash image, as built
#   btcode/build-ramtest/test.bin - RAM-loadable image for RAM testing
#
# Usage:
#   ./build_bootloader.sh          # build all variants
#   ./build_bootloader.sh clean    # clean all build outputs

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# Project root is 2 levels up: 31-Bootloader -> 3-Main-SoC -> project root
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

JUMP_ADDR="${JUMP_ADDR:-0x80500000}"
CROSS_PREFIX="mips-lexra-linux-musl-"

# --- Board selection ---------------------------------------------------------
# Per-board constants (DRAM size/top, DDR bring-up) live in
# boards/<board>/board.h — see boards/README.md. Default: the Lidl
# reference board. The build is reproducible: it regenerates the committed
# boot-img/<board>/boot.bin bit-for-bit.

BOARD="${BOARD:-lidl}"
if [ ! -f "$SCRIPT_DIR/boards/$BOARD/board.h" ]; then
    echo "ERROR: unknown BOARD '$BOARD' — no boards/$BOARD/board.h" >&2
    echo "Available boards: $(ls "$SCRIPT_DIR/boards" | grep -v README | tr '\n' ' ')" >&2
    exit 1
fi

# Toolchain - check project root first, then walk up the repo tree
find_toolchain() {
    # Check in the project root directory (~/rtl8196e-gateway)
    if [ -d "$PROJECT_ROOT/x-tools/mips-lexra-linux-musl/bin" ]; then
        echo "$PROJECT_ROOT/x-tools/mips-lexra-linux-musl"
        return 0
    fi
    # Fallback: walk up from script directory
    local dir="$SCRIPT_DIR"
    while [ "$dir" != "/" ]; do
        if [ -d "$dir/x-tools/mips-lexra-linux-musl/bin" ]; then
            echo "$dir/x-tools/mips-lexra-linux-musl"
            return 0
        fi
        dir="$(cd "$dir/.." && pwd)"
    done
    return 1
}

TOOLCHAIN_DIR="$(find_toolchain || true)"

# Add toolchain to PATH only if not already available (avoids GLIBC mismatch in Docker)
if ! command -v ${CROSS_PREFIX}gcc >/dev/null 2>&1 && [ -n "$TOOLCHAIN_DIR" ]; then
    export PATH="${TOOLCHAIN_DIR}/bin:$PATH"
fi
export CROSS="${CROSS_PREFIX}"

# --- Locate Realtek tools (cvimg, lzma) ------------------------------------

REALTEK_TOOLS=""
for dir in \
    "/home/builder/realtek-tools/bin" \
    "${PROJECT_ROOT}/1-Build-Environment/11-realtek-tools/bin"; do
    if [ -x "${dir}/cvimg" ] && [ -x "${dir}/lzma" ]; then
        REALTEK_TOOLS="$dir"
        break
    fi
done

# --- Checks ----------------------------------------------------------------

if [ -z "$TOOLCHAIN_DIR" ]; then
    echo "Toolchain not found in parent directories (expected x-tools/mips-lexra-linux-musl)"
    echo ""
    echo "Build the toolchain first:"
    echo "  cd ${PROJECT_ROOT}/1-Build-Environment/10-lexra-toolchain"
    echo "  ./build_toolchain.sh"
    exit 1
fi

if ! command -v "${CROSS_PREFIX}gcc" >/dev/null 2>&1; then
    echo "Compiler not found: ${CROSS_PREFIX}gcc"
    exit 1
fi

# --- Clean target -----------------------------------------------------------

do_clean() {
    echo "Cleaning all build outputs..."
    make -C "$SCRIPT_DIR/boot"   CROSS="$CROSS_PREFIX" clean 2>/dev/null || true
    make -C "$SCRIPT_DIR/btcode" CROSS="$CROSS_PREFIX" clean 2>/dev/null || true
    # boot-img/<board>/boot.bin is a committed artifact, not a build output —
    # clean leaves it alone (git restores it if a build overwrote it).
    echo "Done."
}

if [ "${1:-}" = "clean" ]; then
    do_clean
    exit 0
fi

# --- Build ------------------------------------------------------------------

echo "========================================="
echo "  BUILDING RTL8196E BOOTLOADER"
echo "========================================="
echo ""
echo "Toolchain: $TOOLCHAIN_DIR"
echo "Compiler:  $(${CROSS_PREFIX}gcc --version | head -1)"
echo "Jump addr: $JUMP_ADDR"
echo "Board:     $BOARD"
if [ -n "$REALTEK_TOOLS" ]; then
    echo "Realtek:   $REALTEK_TOOLS"
else
    echo "ERROR: Realtek tools (cvimg, lzma) not found"
    echo "  Build them: cd ${PROJECT_ROOT}/1-Build-Environment/11-realtek-tools && ./build_tools.sh"
    exit 1
fi
echo ""

# Pass tool paths to btcode Makefile (overrides hardcoded defaults)
BTCODE_VARS="CROSS=${CROSS_PREFIX} CVIMG=${REALTEK_TOOLS}/cvimg LZMA=${REALTEK_TOOLS}/lzma BOARD=${BOARD}"

# --- boot variant ---
echo "--- Building boot image (board: $BOARD) ---"
make -C "$SCRIPT_DIR/boot" CROSS="$CROSS_PREFIX" clean
make -C "$SCRIPT_DIR/boot" CROSS="$CROSS_PREFIX" boot JUMP_ADDR="$JUMP_ADDR" BOARD="$BOARD" OUTDIR=build
make -C "$SCRIPT_DIR/btcode" $BTCODE_VARS clean
make -C "$SCRIPT_DIR/btcode" $BTCODE_VARS OUTDIR=build
mkdir -p "$SCRIPT_DIR/boot-img/$BOARD"
cp -f "$SCRIPT_DIR/btcode/build/boot.bin" "$SCRIPT_DIR/boot-img/$BOARD/boot.bin"

# --- ramtest variant (separate object directory, no clean needed) ---
echo ""
echo "--- Building ramtest variant ---"
make -C "$SCRIPT_DIR/boot" CROSS="$CROSS_PREFIX" boot JUMP_ADDR="$JUMP_ADDR" BOARD="$BOARD" OUTDIR=build-ramtest RAMTEST_TRACE=1
make -C "$SCRIPT_DIR/btcode" $BTCODE_VARS OUTDIR=build-ramtest RAMTEST_TRACE=1

# --- Include trace check ----------------------------------------------------
# The loader is freestanding and owns every header it uses; the only outside
# files are the compiler's own <stdint.h>, <stddef.h> and <stdarg.h>, which
# gcc serves from lib/gcc/ under -ffreestanding.  The -MD dependency files of
# every unit must therefore name nothing under the toolchain sysroot — that
# is where a lost -ffreestanding, or a stray <string.h>, would show first.
echo ""
echo "--- Checking the include trace ---"
dep_files="$(find "$SCRIPT_DIR/boot/build" "$SCRIPT_DIR/boot/build-ramtest" \
                  "$SCRIPT_DIR/btcode/build" "$SCRIPT_DIR/btcode/build-ramtest" -name '*.d')"
n_dep=$(echo "$dep_files" | grep -c .)
outside="$(cat $dep_files | tr ' \\' '\n\n' | grep '^/' | sort -u)"
if echo "$outside" | grep -q 'sysroot'; then
    echo "ERROR: a unit includes a sysroot header (the build is freestanding):" >&2
    echo "$outside" | grep 'sysroot' >&2
    exit 1
fi
if [ -n "$outside" ] && echo "$outside" | grep -qv '/lib/gcc/'; then
    echo "ERROR: a unit includes a header from outside the tree and outside gcc's own:" >&2
    echo "$outside" | grep -v '/lib/gcc/' >&2
    exit 1
fi
echo "$n_dep dependency files, outside headers: $(echo "$outside" | sed 's|.*/||' | tr '\n' ' ')"

# --- Summary ----------------------------------------------------------------

echo ""
echo "========================================="
echo "  BUILD SUMMARY"
echo "========================================="
echo ""
[ -f "$SCRIPT_DIR/boot-img/$BOARD/boot.bin" ] && ls -lh "$SCRIPT_DIR/boot-img/$BOARD/boot.bin"
[ -f "$SCRIPT_DIR/btcode/build-ramtest/test.bin" ] && ls -lh "$SCRIPT_DIR/btcode/build-ramtest/test.bin"
echo ""
echo "Done."
