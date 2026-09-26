#!/bin/bash
# flash_kernel.sh — Flash kernel partition via TFTP
#
# The device must be in download mode (<RealTek> prompt) before running.
# WARNING: Flashing the kernel triggers an automatic reboot.
#
# Usage:
#   ./flash_kernel.sh                         # flash lidl / 6.18 (default)
#   KERNEL=7.2 ./flash_kernel.sh              # flash the 7.2 line
#   BOARD=sengled-e39-g8c ./flash_kernel.sh   # flash the Sengled G4 image
#   ./flash_kernel.sh -i <file>               # flash an explicit file
#   ./flash_kernel.sh 192.168.1.6             # override target IP (positional)
#
# Options:
#   -i, --image FILE    Explicit image (overrides BOARD/KERNEL resolution)
#
# Environment variables (optional, for non-interactive use):
#   BOARD=<name>          Board image (default: lidl; also sengled-e39-g8c)
#   KERNEL=<line>         Kernel line (default: 6.18; also 7.2)
#   CONFIRM=y             Skip the "Proceed?" prompt
#   BOOT_IP=<addr>        Bootloader-mode address when no positional IP is
#                         given. Falls back to gateway.env, then to an address
#                         on this host's own segment (see lib/gwconf.sh).
#
# J. Nilo - December 2025, unified April 2026

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# Shared safe-retry TFTP upload helper (probe_tftp_wrq, tftp_put_safe).
. "$SCRIPT_DIR/../../lib/flash_tftp.sh"
# Shared (board, kernel) → pre-built image resolver (resolve_kernel_image).
. "$SCRIPT_DIR/../../lib/kernel_image.sh"
# Host-side gateway address resolution — see lib/gwconf.sh.
# shellcheck disable=SC1091
. "$SCRIPT_DIR/../../lib/gwconf.sh"

# ── Argument parsing ──────────────────────────────────────────────────────

IMAGE_OVERRIDE=""
POS_ARGS=()
while [ $# -gt 0 ]; do
    case "$1" in
        -i|--image)
            IMAGE_OVERRIDE="$2"; shift 2 ;;
        --image=*)
            IMAGE_OVERRIDE="${1#*=}"; shift ;;
        -h|--help)
            sed -n '2,16p' "$0" | sed 's|^# \{0,1\}||'
            exit 0 ;;
        *)
            POS_ARGS+=("$1"); shift ;;
    esac
done

TARGET_IP="${POS_ARGS[0]:-${BOOT_IP:-$(gwconf_cold_boot_ip)}}"

# Resolve image path. Precedence: --image override > BOARD/KERNEL resolution.
# BOARD (default lidl) and KERNEL (default 6.18) pick the pre-built image from
# kernel-img/<board>/kernel-<kernel>.img (see lib/kernel_image.sh) — so a Lidl
# user who sets neither gets exactly the old kernel-6.18.img behaviour.
BOARD="${BOARD:-lidl}"
KERNEL="${KERNEL:-6.18}"
if [ -n "$IMAGE_OVERRIDE" ]; then
    case "$IMAGE_OVERRIDE" in
        /*) IMAGE="$IMAGE_OVERRIDE" ;;
        *)  IMAGE="${SCRIPT_DIR}/${IMAGE_OVERRIDE}" ;;
    esac
else
    IMAGE="$(resolve_kernel_image "$BOARD" "$KERNEL")" || exit 1
fi

# Check prerequisites
tftp_usage="$(tftp --help 2>&1 || true)"
if ! command -v tftp >/dev/null 2>&1 || ! echo "$tftp_usage" | grep -q '\-c'; then
    echo "Error: tftp-hpa client not found (need the -c flag)." >&2
    echo "Install it with: sudo apt install tftp-hpa" >&2
    exit 1
fi
if ! command -v nc >/dev/null 2>&1; then
    echo "Error: netcat (nc) not found." >&2
    echo "Install it with: sudo apt install netcat-openbsd" >&2
    exit 1
fi

if [ ! -f "$IMAGE" ]; then
    echo "Error: $(basename "$IMAGE") not found at ${IMAGE}"
    echo "Run ./build_kernel.sh first"
    exit 1
fi

IMAGE_BASENAME="$(basename "$IMAGE")"

SIZE=$(stat -c%s "$IMAGE" 2>/dev/null || stat -f%z "$IMAGE")

if [ "${BOOTLOADER_CONFIRMED:-}" != "1" ]; then
    echo "Checking if gateway is in boot mode..."

    IFACE="$(ip route get "$TARGET_IP" 2>/dev/null | awk '{for(i=1;i<=NF;i++) if($i=="dev"){print $(i+1); exit}}')"
    if [ -z "${IFACE:-}" ]; then
        echo "Error: cannot determine outgoing interface to ${TARGET_IP} (ip route get failed)." >&2
        exit 1
    fi

    if ip route get "$TARGET_IP" 2>/dev/null | grep -qE '\svia\s'; then
        echo "Error: ${TARGET_IP} is reached via a gateway (routed). Must be same L2 segment." >&2
        exit 1
    fi

    TRIES="${TRIES:-10}"
    PORT="${PORT:-69}"
    SLEEP_BETWEEN="${SLEEP_BETWEEN:-0.2}"

    ok=0
    for _ in $(seq 1 "$TRIES"); do
        bash -c 'echo -n X > /dev/udp/'"$TARGET_IP"'/'"$PORT"'' >/dev/null 2>&1 || true
        sleep 0.2
        LINE="$(ip neigh show "$TARGET_IP" dev "$IFACE" 2>/dev/null || true)"
        if echo "$LINE" | grep -qiE 'lladdr [0-9a-f]{2}(:[0-9a-f]{2}){5}'; then
            ok=1
            break
        fi
        sleep "$SLEEP_BETWEEN"
    done

    if [ "$ok" -ne 1 ]; then
        echo "Error: ${TARGET_IP} unreachable — check cable and that device is in download mode." >&2
        exit 1
    fi
    # A MAC in the neighbour table does not prove the bootloader is there (a
    # stale entry, or another host on the address): require a TFTP server
    # that ACKs a WRQ. Probed before the flash-result listener starts.
    if ! probe_tftp_wrq "$TARGET_IP"; then
        echo "Error: no TFTP server at ${TARGET_IP}: the gateway is not in bootloader mode." >&2
        exit 1
    fi
fi

echo ""
if [ -z "$IMAGE_OVERRIDE" ]; then
    echo "Flashing kernel: BOARD=${BOARD} KERNEL=${KERNEL} (${IMAGE_BASENAME}, ${SIZE} bytes) → ${TARGET_IP}"
else
    echo "Flashing ${IMAGE_BASENAME} (${SIZE} bytes) to ${TARGET_IP}..."
fi
echo ""
if [ "${CONFIRM:-}" != "y" ]; then
    # In bootloader mode Linux is not running, so the board cannot be read from
    # /proc/device-tree/model (as flash_remote.sh does over SSH). The interactive
    # confirmation is the safeguard here — show the selection and let the operator vet it.
    echo "Note: in bootloader mode the board cannot be auto-verified — confirm this image matches your gateway."
    read -r -p "Proceed? [y/N] " confirm
    if [[ ! "$confirm" =~ ^[yY]$ ]]; then
        echo "Aborted."
        echo "To flash manually: tftp -m binary ${TARGET_IP} -c put ${IMAGE}"
        exit 0
    fi
fi

NOTIFY_PORT=9999
NOTIFY_TMO=60

# cd to the image's directory so tftp's basename upload finds it: the image now
# lives under kernel-img/<board>/, not at SCRIPT_DIR.
cd "$(dirname "$IMAGE")"
if ! tftp_put_safe "$TARGET_IP" "$IMAGE_BASENAME" 3 30 >/dev/null; then
    echo "Error: transfer failed after retries." >&2
    exit 1
fi
# Listen for the flash result only now: every TFTP upload the bootloader
# completes, including tftp_put_safe's 1-byte probe between retries, makes it
# send OK or FAIL on UDP:9999, so a listener started before the upload could
# read a probe's FAIL as the result. The bootloader sends the result only
# after erasing, programming and verifying flash, well after nc is listening.
notify_file=$(mktemp)
(timeout "$NOTIFY_TMO" nc -u -l -p "$NOTIFY_PORT" > "$notify_file" 2>/dev/null) &
nc_pid=$!
echo "Uploaded. Waiting for flash write..."
while kill -0 "$nc_pid" 2>/dev/null; do
    [ -s "$notify_file" ] && { kill "$nc_pid" 2>/dev/null; break; }
    sleep 0.5
done
wait "$nc_pid" 2>/dev/null || true
result=$(tr -d '\0' < "$notify_file")
rm -f "$notify_file"

if [ "$result" = "OK" ]; then
    echo "Flash Write Succeeded."
elif [ "$result" = "FAIL" ]; then
    echo "Error: flash write FAILED on gateway." >&2
    exit 1
else
    echo "Warning: no notification received (timeout ${NOTIFY_TMO}s)." >&2
    echo "Check the serial console for status."
fi
echo ""
echo "Done."
echo "Bootloader V2.5+ reboots automatically."
echo "Older versions: J BFC00000 (serial console) or hard reset."
