#!/bin/bash
# Build the exact production-layout reference and its local-hole I-MEM candidate.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
KERNEL_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
LINE="${1:-}"
MANIFEST="${2:-}"

case "$LINE" in
	6.18) VERSION=6.18.51 ;;
	7.2) VERSION=7.2.5 ;;
	*) echo "usage: $0 <6.18|7.2> <selection-manifest.json>" >&2; exit 2 ;;
esac
[ -f "$MANIFEST" ] || { echo "manifest not found: $MANIFEST" >&2; exit 2; }
MANIFEST="$(realpath "$MANIFEST")"
OUT="$KERNEL_DIR/imem-work/$VERSION/production"
REFERENCE="$OUT/reference"
CANDIDATE="$OUT/candidate"
BUILD="$KERNEL_DIR/linux-$LINE-imem-optimized-rtl8196e"
mkdir -p "$REFERENCE" "$CANDIDATE"

python3 - "$MANIFEST" "$VERSION" <<'PY'
import json, sys
m = json.load(open(sys.argv[1]))
if m.get("release") != sys.argv[2]:
    raise SystemExit(f"manifest release {m.get('release')} != {sys.argv[2]}")
if not m.get("bootstrap", {}).get("pass"):
    raise SystemExit("manifest bootstrap gate did not pass")
PY

echo "=== empty production-layout reference: Linux $VERSION ==="
( cd "$KERNEL_DIR" && \
	BUILD_TAG=imem-optimized IMEM_EMPTY=1 VMLINUX_LINK_MAP=1 \
	IMAGE_OUT="$REFERENCE/kernel.img" BOARD=lidl KERNEL="$LINE" \
	./build_kernel.sh clean )
cp "$BUILD/vmlinux" "$BUILD/System.map" "$BUILD/.config" "$REFERENCE/"
cp "$BUILD/vmlinux.unstripped.map" "$REFERENCE/link.map"

echo "=== applying local holes ==="
python3 "$SCRIPT_DIR/apply_local_holes.py" \
	--manifest "$MANIFEST" --build-dir "$BUILD"
count="$(python3 - "$MANIFEST" <<'PY'
import json, sys
print(sum(bool(x.get("selected")) for x in json.load(open(sys.argv[1]))["candidates"]))
PY
)"
roots=""
index=0
while [ "$index" -lt "$count" ]; do
	roots="$roots $(printf '__imem_hole_%04d' "$index")"
	index=$((index + 1))
done

echo "=== candidate relink: Linux $VERSION ==="
( cd "$KERNEL_DIR" && \
	BUILD_TAG=imem-optimized IMEM_EMPTY=1 VMLINUX_LINK_MAP=1 \
	IMEM_HOLE_ROOTS="$roots" \
	IMAGE_OUT="$CANDIDATE/kernel.img" BOARD=lidl KERNEL="$LINE" \
	./build_kernel.sh )
cp "$BUILD/vmlinux" "$BUILD/System.map" "$BUILD/.config" "$CANDIDATE/"
cp "$BUILD/vmlinux.unstripped.map" "$CANDIDATE/link.map"
cp "$MANIFEST" "$CANDIDATE/selection-manifest.json"

python3 "$SCRIPT_DIR/check_local_holes.py" \
	--manifest "$MANIFEST" --reference "$REFERENCE" --candidate "$CANDIDATE" \
	--build-dir "$BUILD" --out "$CANDIDATE/invariants.json"
python3 "$SCRIPT_DIR/scan_dynamic_code.py" \
	--reference "$CANDIDATE" --config "$CANDIDATE/.config" --gate-window
sha256sum "$REFERENCE"/kernel.img "$REFERENCE"/vmlinux \
	"$CANDIDATE"/kernel.img "$CANDIDATE"/vmlinux \
	>"$OUT/SHA256SUMS"
echo "production candidate ready: $CANDIDATE/kernel.img"
