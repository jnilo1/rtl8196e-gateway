#!/bin/bash
# Decode one bounded profile generation and produce the exact TX selection.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
KERNEL_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
LINE="${1:-}"
CAPTURES="${2:-}"
case "$LINE" in
	6.18) VERSION=6.18.51 ;;
	7.2) VERSION=7.2.5 ;;
	*) echo "usage: $0 <6.18|7.2> <capture-directory>" >&2; exit 2 ;;
esac
[ -d "$CAPTURES" ] || { echo "capture directory not found: $CAPTURES" >&2; exit 2; }
CAPTURES="$(realpath "$CAPTURES")"
REFERENCE="$KERNEL_DIR/imem-work/$VERSION/profile-reference"
BUILD="$KERNEL_DIR/linux-$LINE-imem-profile-rtl8196e"
SYMBOLS="$CAPTURES/vmlinux.symbols.txt"
MANIFEST="$CAPTURES/selection-manifest.json"

mips-lexra-linux-musl-nm -n "$REFERENCE/vmlinux" >"$SYMBOLS"
for direction in tx rx; do
	for capture in 1 2; do
		python3 "$SCRIPT_DIR/profile_decode.py" \
			--load "$CAPTURES/$direction$capture.bin" \
			--idle "$CAPTURES/idle1.bin" --idle "$CAPTURES/idle2.bin" \
			--symbols "$SYMBOLS" --duration 180 \
			--label "$direction$capture" \
			--out "$CAPTURES/$direction$capture.json"
	done
done

python3 "$SCRIPT_DIR/select_profile.py" \
	--build-dir "$BUILD" --reference "$REFERENCE" \
	--tx "$CAPTURES/tx1.json" --tx "$CAPTURES/tx2.json" \
	--rx "$CAPTURES/rx1.json" --rx "$CAPTURES/rx2.json" \
	--release "$VERSION" --replicates 200 --out "$MANIFEST"

python3 - "$MANIFEST" <<'PY'
import json, sys
m = json.load(open(sys.argv[1]))
if not m["bootstrap"]["pass"]:
    raise SystemExit("bootstrap retention is below 80%; take exactly two more TX captures")
print(f"selection manifest: {sys.argv[1]}")
PY
