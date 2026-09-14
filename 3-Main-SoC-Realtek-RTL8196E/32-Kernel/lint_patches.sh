#!/bin/bash
# lint_patches.sh — from-clean dry-run of a kernel line's patch set (6.18 or 7.2).
#
# Why this exists: build_kernel.sh only feeds patches-6.18/*.patch to patch(1)
# when it extracts a *fresh* tree (`if [ ! -f "$BUILD_DIR/Makefile" ]`). Every
# incremental build reuses the already-patched linux-6.18-rtl8196e/ tree and
# rsyncs files-6.18/ over it — so a patch file that has gone malformed is never
# re-exercised locally. A bad hunk can therefore sit green in the repo until
# someone (a contributor on a clean checkout) does a from-clean build and hits
# it. That is exactly how the drivers-gpio-Kconfig.patch off-by-one (#136)
# escaped: introduced by the gpio v1.2 audit edit, invisible to every reused-tree
# build after it.
#
# This guard downloads the pinned pristine kernel and replays the build's patch
# step with --dry-run, using the SAME flags and the SAME glob order as
# build_kernel.sh. Green here ⟺ a from-clean build's "Applying patches" step
# would succeed. It catches malformed hunk headers, rejects, and context drift
# across point releases — none of which a reused-tree build can see.
#
# Usage:  ./lint_patches.sh             # 6.18 line (default)
#         KERNEL=7.2 ./lint_patches.sh  # 7.2 line
# Exit:   0 = all patches apply clean from-clean; 1 = at least one would fail.
#
# Zero project deps: only patch, wget, tar, xz. Caches the tarball next to
# build_kernel.sh so repeated local runs don't re-download.

set -euo pipefail

indent_output() {
    while IFS= read -r line; do
        printf '          %s\n' "$line"
    done
}

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD="${SCRIPT_DIR}/build_kernel.sh"

# Single source of truth: pull the version constants for the selected KERNEL
# line straight out of build_kernel.sh's `case "$KERNEL"` block, so this guard
# can never test a different kernel than the build does. (The constants are
# indented inside the case branch, hence the branch-aware parse rather than a
# line-anchored grep.)
KERNEL="${KERNEL:-6.18}"
branch_field() {
    # Print <var>'s value from inside build_kernel.sh's `${KERNEL})` case branch.
    awk -v line="$KERNEL" -v var="$1" '
        $0 ~ "^[[:space:]]*"line"\\)"     { inb=1; next }
        inb && /;;/                       { inb=0 }
        inb && $0 ~ "^[[:space:]]*"var"=" { v=$0; sub(/^[^"]*"/,"",v); sub(/".*/,"",v); print v; exit }
    ' "$BUILD"
}
KERNEL_VERSION="$(branch_field KERNEL_VERSION)"
KERNEL_MAJOR="$(branch_field KERNEL_MAJOR)"
KERNEL_MAJOR_MINOR="$(branch_field KERNEL_MAJOR_MINOR)"

if [ -z "$KERNEL_VERSION" ] || [ -z "$KERNEL_MAJOR" ]; then
    echo "ERROR: could not read KERNEL_VERSION/KERNEL_MAJOR from build_kernel.sh" >&2
    exit 1
fi

PATCHES_DIR="${SCRIPT_DIR}/patches-${KERNEL_MAJOR_MINOR}"
TARBALL="linux-${KERNEL_VERSION}.tar.xz"
KERNEL_BASE_URL="${KERNEL_MIRROR:-https://cdn.kernel.org/pub/linux/kernel}"   # KERNEL_MIRROR: see build_kernel.sh
URL="${KERNEL_BASE_URL%/}/v${KERNEL_MAJOR}/${TARBALL}"
CACHE="${SCRIPT_DIR}/${TARBALL}"

if [ ! -d "$PATCHES_DIR" ]; then
    echo "ERROR: patches dir not found: $PATCHES_DIR" >&2
    exit 1
fi

echo "==================================================================="
echo "  Patch lint — from-clean dry-run against pristine Linux ${KERNEL_VERSION}"
echo "  Patches: $(basename "$PATCHES_DIR")"
echo "==================================================================="

# ── Fetch + extract pristine tree into a throwaway dir ──────────────────────
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

if [ ! -f "$CACHE" ]; then
    echo "Downloading ${TARBALL}..."
    wget -q -O "$CACHE" "$URL" || { rm -f "$CACHE"; echo "ERROR: download failed: $URL" >&2; exit 1; }
fi

echo "Extracting pristine tree..."
tar xf "$CACHE" -C "$WORK"
TREE="${WORK}/linux-${KERNEL_VERSION}"
[ -d "$TREE" ] || { echo "ERROR: extracted tree not found: $TREE" >&2; exit 1; }

# ── Replay build_kernel.sh's patch step, dry-run ────────────────────────────
# Same `patch -p1 -f` and same shell glob order as build_kernel.sh's loop, so
# the verdict is a faithful predictor of the real build.
echo ""
echo "Dry-run applying patches (order = build_kernel.sh)..."
cd "$TREE"
shopt -s nullglob
fail=0
count=0
for patch in "$PATCHES_DIR"/*.patch; do
    [ -f "$patch" ] || continue
    count=$((count + 1))
    name="$(basename "$patch")"
    if out="$(patch -p1 -f --dry-run < "$patch" 2>&1)"; then
        if printf '%s\n' "$out" | \
                grep -Eiq 'fuzz|offset|warning|malformed|misordered|reversed|previously applied|FAILED|reject'; then
            printf '  WARN  %s\n' "$name"
            printf '%s\n' "$out" | sed 's/^/          /'
            fail=1
            continue
        fi
        printf '  ok    %s\n' "$name"
        # Apply for real (still in the throwaway tree) so later patches that
        # depend on an earlier one's result see it — mirrors the build, which
        # applies sequentially into one tree.
        if ! apply_out="$(patch -p1 -f --no-backup-if-mismatch < "$patch" 2>&1)"; then
            printf '  FAIL  %s (real apply after successful dry-run)\n' "$name"
            printf '%s\n' "$apply_out" | indent_output
            fail=1
        elif printf '%s\n' "$apply_out" | \
                grep -Eiq 'fuzz|offset|warning|malformed|misordered|reversed|previously applied|FAILED|reject'; then
            printf '  WARN  %s (real apply)\n' "$name"
            printf '%s\n' "$apply_out" | indent_output
            fail=1
        fi
    else
        printf '  FAIL  %s\n' "$name"
        printf '%s\n' "$out" | indent_output
        fail=1
    fi
done
shopt -u nullglob

echo ""
if [ "$fail" -ne 0 ]; then
    echo "RESULT: at least one patch would fail or apply with a warning, fuzz, or offset" >&2
    echo "        against Linux ${KERNEL_VERSION} (check the hunk headers / context)." >&2
    exit 1
fi
echo "RESULT: all ${count} patches apply cleanly from-clean ✓"
