#!/bin/bash
# run_host_tests.sh — build and run the native unit tests of the bootloader
#
# Compiles tests/checks_test.c with the host compiler against the same
# boot/include/checks.h the loader uses, with the Lidl board constants.
# No cross-toolchain, no hardware.
#
# Usage: ./tests/run_host_tests.sh [BOARD]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BOOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BOARD="${1:-${BOARD:-lidl}}"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

# crc32.c is compiled on its own with the loader include path (boot/include
# carries the loader's stdlib.h, which must not shadow libc for
# the test).
cc -std=gnu99 -Wall -Wextra -Werror -O -I"$BOOT_DIR/boot/include" \
    -c -o "$OUT/crc32.o" "$BOOT_DIR/boot/crc32.c"
cc -std=gnu99 -Wall -Wextra -Werror -O \
    -I"$BOOT_DIR/boards/$BOARD" \
    -o "$OUT/checks_test" "$SCRIPT_DIR/checks_test.c" "$OUT/crc32.o"
"$OUT/checks_test"
