#!/bin/bash
# flash_bootloader.sh — Upload bootloader via TFTP to device in recovery mode
#
# The device must be in download mode (<RealTek> prompt) before running.
#
# Usage: ./flash_bootloader.sh [IP] [IMAGE]
#   IP    - Target IP in bootloader mode. Defaults to BOOT_IP, then gateway.env,
#           then an address on this host's own segment (see lib/gwconf.sh).
#   IMAGE - Image file (default: boot-img/$BOARD/boot.bin)
#
# Environment variables (optional overrides):
#   BOARD          - "lidl" (default) or "sengled-e39-g8c" — selects the
#                    pre-built boot-img/<board>/boot.bin (ignored when an
#                    explicit IMAGE argument is given). The bootloader carries
#                    the board's DRAM bring-up: flashing the wrong board's
#                    binary bricks the gateway.
#   CONFIRM        - Set to "y" to skip the "Proceed?" prompt
#   TRIES          - ARP probe attempts (default: 10)
#   PORT           - UDP port used to trigger ARP (default: 69)
#   SLEEP_BETWEEN  - Pause between ARP probes in seconds (default: 0.2)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# Shared safe-retry TFTP upload helper (probe_tftp_wrq, tftp_put_safe).
. "$SCRIPT_DIR/../../lib/flash_tftp.sh"
# Per-board pre-built image resolver (resolve_boot_image).
. "$SCRIPT_DIR/../../lib/kernel_image.sh"
# Host-side gateway address resolution — see lib/gwconf.sh.
# shellcheck disable=SC1091
. "$SCRIPT_DIR/../../lib/gwconf.sh"

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

TARGET_IP="${1:-${BOOT_IP:-$(gwconf_cold_boot_ip)}}"
BOARD="${BOARD:-lidl}"
if [ -n "${2:-}" ]; then
    IMAGE="$2"
else
    # resolve_boot_image validates BOARD and that the pre-built exists.
    IMAGE="$(resolve_boot_image "$BOARD")" || exit 1
fi

TRIES="${TRIES:-10}"
PORT="${PORT:-69}"
SLEEP_BETWEEN="${SLEEP_BETWEEN:-0.2}"

if [ ! -f "$IMAGE" ]; then
    echo "Error: $IMAGE not found"
    echo "Run ./build_bootloader.sh first"
    exit 1
fi

SIZE=$(stat -c%s "$IMAGE" 2>/dev/null || stat -f%z "$IMAGE")
NAME=$(basename "$IMAGE")

# --- helpers ---------------------------------------------------------------

get_iface_for_ip() {
    local ip="$1"
    ip route get "$ip" 2>/dev/null \
        | awk '/ dev /{for (i=1;i<=NF;i++) if ($i=="dev") {print $(i+1); exit}}' || true
}

neigh_has_lladdr() {
    local line="$1"
    echo "$line" | grep -Eqi 'lladdr [0-9a-f]{2}(:[0-9a-f]{2}){5}'
}

trigger_kernel_arp_via_udp() {
    local ip="$1"
    local port="${2:-69}"
    (
        echo -n X >"/dev/udp/$ip/$port" 2>/dev/null || true
    ) &
    local pid=$!
    sleep 0.3
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
}

check_bootloader_reachable() {
    local ip="$1"
    local iface="$2"

    # Flush any stale ARP entry to force a fresh resolution
    ip neigh del "$ip" dev "$iface" 2>/dev/null || true

    for _ in $(seq 1 "$TRIES"); do
        trigger_kernel_arp_via_udp "$ip" "$PORT"

        local nei
        nei="$(ip neigh show "$ip" dev "$iface" 2>/dev/null || true)"

        if [ -n "$nei" ] && neigh_has_lladdr "$nei"; then
            return 0
        fi

        sleep "$SLEEP_BETWEEN"
    done
    return 1
}

# --- main ------------------------------------------------------------------

if [ "${BOOTLOADER_CONFIRMED:-}" != "1" ]; then
    echo "Checking if gateway is in boot mode..."

    IFACE="$(get_iface_for_ip "$TARGET_IP")"
    if [ -z "$IFACE" ]; then
        echo "Error: cannot determine outgoing interface to ${TARGET_IP} (ip route get failed)." >&2
        exit 1
    fi

    if ip route get "$TARGET_IP" 2>/dev/null | grep -qE '\svia\s'; then
        echo "Error: ${TARGET_IP} is reached via a gateway (routed). Must be on the same L2 segment." >&2
        exit 1
    fi

    if ! check_bootloader_reachable "$TARGET_IP" "$IFACE"; then
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
echo "Flashing ${IMAGE} (${SIZE} bytes) to ${TARGET_IP}..."
echo ""
if [ "${CONFIRM:-}" != "y" ]; then
    read -r -p "Proceed? [y/N] " confirm
    if [[ ! "$confirm" =~ ^[yY]$ ]]; then
        echo "Aborted."
        exit 0
    fi
fi

NOTIFY_PORT=9999
NOTIFY_TMO=30

# tftp put sends a name relative to the cwd — go where the image lives
# (boot-img/<board>/ for resolved images, or wherever IMAGE points).
cd "$(dirname "$IMAGE")"
if ! tftp_put_safe "$TARGET_IP" "$NAME" 3 15 >/dev/null; then
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
