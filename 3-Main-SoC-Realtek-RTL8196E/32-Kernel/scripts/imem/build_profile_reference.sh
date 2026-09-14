#!/bin/bash
# Build an isolated, genuinely empty-I-MEM profiling reference.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
KERNEL_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
KERNEL_LINE="${1:-}"

case "$KERNEL_LINE" in
    6.18) KERNEL_VERSION=6.18.51 ;;
    7.2)  KERNEL_VERSION=7.2.5 ;;
    *)
        echo "usage: $0 {6.18|7.2}" >&2
        exit 2
        ;;
esac

OUT_DIR="${KERNEL_DIR}/imem-work/${KERNEL_VERSION}/profile-reference"
BUILD_DIR="${KERNEL_DIR}/linux-${KERNEL_LINE}-imem-profile-rtl8196e"
IMAGE="${OUT_DIR}/kernel.img"

mkdir -p "$OUT_DIR"

build_args=(clean)
if [ "${IMEM_PROFILE_REUSE:-0}" = "1" ]; then
    build_args=()
fi

env KERNEL="$KERNEL_LINE" \
    BOARD=lidl \
    BUILD_TAG=imem-profile \
    IMEM_PROFILE=1 \
    VMLINUX_LINK_MAP=1 \
    IMAGE_OUT="$IMAGE" \
    "${KERNEL_DIR}/build_kernel.sh" "${build_args[@]}"

cp "$BUILD_DIR/vmlinux" "$OUT_DIR/vmlinux"
cp "$BUILD_DIR/System.map" "$OUT_DIR/System.map"
cp "$BUILD_DIR/.config" "$OUT_DIR/config"
cp "$BUILD_DIR/vmlinux.unstripped.map" "$OUT_DIR/link.map"

read -r iram_addr iram_tail < <(
    "${CROSS_COMPILE:-mips-lexra-linux-musl-}nm" -n "$OUT_DIR/vmlinux" |
        awk '$3 == "__iram" {a=$1} $3 == "__iram_tail" {t=$1} END {print a, t}'
)

if [ -z "$iram_addr" ] || [ -z "$iram_tail" ]; then
    echo "ERROR: the profiling reference has no complete I-MEM bounds" >&2
    exit 1
fi
if [ "$iram_addr" != "$iram_tail" ]; then
    echo "ERROR: profiling reference I-MEM is not empty: ${iram_addr}-${iram_tail}" >&2
    exit 1
fi

python3 "${SCRIPT_DIR}/scan_dynamic_code.py" \
    --reference "$OUT_DIR" --config "$OUT_DIR/config" \
    --cross "${CROSS_COMPILE:-mips-lexra-linux-musl-}" --gate-window

sha256sum "$IMAGE" "$OUT_DIR/vmlinux" "$OUT_DIR/System.map" \
    "$OUT_DIR/config" "$OUT_DIR/link.map" > "$OUT_DIR/SHA256SUMS"

echo ""
echo "I-MEM profiling reference ready: $OUT_DIR"
echo "  Linux : $KERNEL_VERSION"
echo "  I-MEM : empty at 0x$iram_addr"
echo "  Image : $IMAGE"
