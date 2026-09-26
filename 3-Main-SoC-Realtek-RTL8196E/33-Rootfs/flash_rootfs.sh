#!/bin/bash
# flash_rootfs.sh — Flash rootfs partition via TFTP
#
# The device must be in download mode (<RealTek> prompt) before running.
#
# Usage: ./flash_rootfs.sh [IP]
#   IP - Target IP in bootloader mode. Defaults to BOOT_IP, then gateway.env,
#        then an address on this host's own segment (see lib/gwconf.sh).
#
# Environment variables (optional, for non-interactive use):
#   CONFIRM=y  - Skip the "Proceed?" prompt
#
# J. Nilo - December 2025

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# Shared safe-retry TFTP upload helper (probe_tftp_wrq, tftp_put_safe).
. "$SCRIPT_DIR/../../lib/flash_tftp.sh"
# Host-side gateway address resolution — see lib/gwconf.sh.
# shellcheck disable=SC1091
. "$SCRIPT_DIR/../../lib/gwconf.sh"
TARGET_IP="${1:-${BOOT_IP:-$(gwconf_cold_boot_ip)}}"
IMAGE="${SCRIPT_DIR}/rootfs.bin"

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

# Always rebuild rootfs from skeleton to avoid stale images
echo "Building rootfs.bin from skeleton..."
"${SCRIPT_DIR}/build_rootfs.sh" -q

SIZE=$(stat -c%s "$IMAGE" 2>/dev/null || stat -f%z "$IMAGE")

# Skip boot mode check if caller already confirmed bootloader is up
if [ "${BOOTLOADER_CONFIRMED:-}" != "1" ]; then
    echo "Checking if gateway is in boot mode..."

    # Determine the local outgoing interface toward the target
    IFACE="$(ip route get "$TARGET_IP" 2>/dev/null | awk '{for(i=1;i<=NF;i++) if($i=="dev"){print $(i+1); exit}}')"
    if [ -z "${IFACE:-}" ]; then
        echo "Error: cannot determine outgoing interface to ${TARGET_IP} (ip route get failed)." >&2
        exit 1
    fi

    # Reject if routed — ARP would resolve the gateway, not the target
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
echo "Flashing rootfs.bin (${SIZE} bytes) to ${TARGET_IP}..."
echo ""
if [ "${CONFIRM:-}" != "y" ]; then
    read -r -p "Proceed? [y/N] " confirm
    if [[ ! "$confirm" =~ ^[yY]$ ]]; then
        echo "Aborted."
        echo "To flash manually: tftp -m binary ${TARGET_IP} -c put rootfs.bin"
        exit 0
    fi
fi

NOTIFY_PORT=9999
NOTIFY_TMO=60

cd "$SCRIPT_DIR"
if ! tftp_put_safe "$TARGET_IP" rootfs.bin 3 30 >/dev/null; then
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
