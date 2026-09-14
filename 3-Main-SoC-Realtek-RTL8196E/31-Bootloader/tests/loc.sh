#!/bin/bash
# loc.sh — lines of code of the bootloader, comments and blank lines excluded
#
# Counts, per category, what the preprocessor leaves once comments are
# stripped (-fpreprocessed -dD -E -P: no macro expansion, no #include
# resolution), so two trees are counted the same way.  Also prints the raw
# line count and the number of files.
#
# Usage: tests/loc.sh [31-Bootloader dir]   (default: the tree this script is in)

set -euo pipefail
ROOT="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
CC="${CROSS:-mips-lexra-linux-musl-}gcc"
command -v "$CC" >/dev/null || CC=gcc

count() { # count <lang> files...
    local lang="$1"; shift
    local files=0 raw=0 code=0 f n
    for f in "$@"; do
        [ -f "$f" ] || continue
        files=$((files + 1))
        raw=$((raw + $(wc -l < "$f")))
        n=$("$CC" -x "$lang" -fpreprocessed -dD -E -P "$f" 2>/dev/null | grep -c '[^[:space:]]' || true)
        code=$((code + n))
    done
    printf '%-44s %5d files %7d lines %7d code\n' "$LABEL" "$files" "$raw" "$code"
}

cd "$ROOT"
shopt -s nullglob globstar
LABEL="stage-2 C sources (boot/*.c, boot/net/*.c)";   count c boot/*.c boot/net/*.c
LABEL="stage-2 assembler (boot/*.S)";                 count assembler-with-cpp boot/*.S
LABEL="stage-1 decompressor C (btcode/*.c)";          count c btcode/*.c
LABEL="stage-1 assembler (btcode/*.S)";               count assembler-with-cpp btcode/*.S
LABEL="stage-2 headers (boot/include/**/*.h)";        count c boot/include/**/*.h
LABEL="stage-1 + board headers (btcode/*.h, boards)"; count c btcode/*.h boards/*/board.h
