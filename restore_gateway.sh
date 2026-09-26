#!/bin/bash
# restore_gateway.sh — Restore a fullflash.bin backup to the gateway
#
# Uploads a 16 MiB flash image to the gateway via TFTP and guides the user
# through the bootloader commands to write it to flash.
#
# The gateway must be in bootloader mode (<RealTek> prompt).
#
# For V2 bootloader (responds to ping): automated TFTP upload, the bootloader
# flashes automatically on receiving a file named "fullflash.bin".
#
# For older bootloaders (Tuya/V1.2): serial console required. The script
# guides you through LOADADDR + tftp put + FLW commands.
#
# Usage: ./restore_gateway.sh [-y] <fullflash.bin> [--boot-ip ADDRESS]
#
# -y (or CONFIRM=y) skips the "Proceed?" confirmation. The serial-guided
# paths still wait for the operator, so they need an interactive terminal.
#
# J. Nilo - March 2026

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# Host-side gateway address resolution — see lib/gwconf.sh.
# shellcheck disable=SC1091
. "${SCRIPT_DIR}/lib/gwconf.sh"
# CRC trailer check before sending — see lib/fullflash_crc.sh.
. "${SCRIPT_DIR}/lib/fullflash_crc.sh"
# TFTP WRQ probe that confirms a bootloader — see lib/flash_tftp.sh.
. "${SCRIPT_DIR}/lib/flash_tftp.sh"
# Bootloader-mode address: flag > BOOT_IP env > gateway.env > the bootloader's
# compiled default. NOT derived from this host's LAN: the gateway is already at a
# bootloader prompt and cannot be told to move (lib/gwconf.sh, gwconf_cold_boot_ip).
BOOT_IP="${BOOT_IP:-$(gwconf_cold_boot_ip)}"
IMAGE=""
CONFIRM="${CONFIRM:-}"

# --- argument parsing --------------------------------------------------------

while [ $# -gt 0 ]; do
    case "$1" in
        --boot-ip|--ip) shift; BOOT_IP="$1" ;;
        -y|--yes) CONFIRM="y" ;;
        --help|-h)
            echo "Usage: $0 [-y] <fullflash.bin> [--boot-ip ADDRESS]"
            echo ""
            echo "Restores a full flash backup to the gateway."
            echo "The gateway must be in bootloader mode (<RealTek> prompt)."
            echo ""
            echo "Options:"
            echo "  --boot-ip ADDR   Gateway IP in bootloader mode (default: ${BOOT_IP})"
            echo "  -y, --yes        Skip the confirmation prompt"
            echo ""
            echo "Environment variables: BOOT_IP, CONFIRM (y = same as -y)"
            exit 0
            ;;
        -*) echo "Unknown option: $1. Use --help for usage."; exit 1 ;;
        *)
            if [ -z "$IMAGE" ]; then
                IMAGE="$1"
            else
                echo "Error: unexpected argument: $1" >&2
                exit 1
            fi
            ;;
    esac
    shift
done

if [ -z "$IMAGE" ]; then
    echo "Usage: $0 [-y] <fullflash.bin> [--boot-ip ADDRESS]"
    echo "Error: no image file specified." >&2
    exit 1
fi

# --- prerequisites -----------------------------------------------------------

FLASH_SIZE=$((16 * 1024 * 1024))  # 16 MiB

# Check image file
if [ ! -f "$IMAGE" ]; then
    echo "Error: file not found: $IMAGE" >&2
    exit 1
fi

img_size=$(stat -c%s "$IMAGE" 2>/dev/null || echo 0)
if [ "$img_size" -ne "$FLASH_SIZE" ]; then
    echo "Error: $IMAGE is ${img_size} bytes (expected ${FLASH_SIZE} = 16 MiB)." >&2
    exit 1
fi

# Check tftp-hpa client
tftp_usage="$(tftp --help 2>&1 || true)"
if ! command -v tftp >/dev/null 2>&1 || ! echo "$tftp_usage" | grep -q '\-c'; then
    echo "Error: tftp-hpa client not found (need the -c flag)." >&2
    echo "Install it with: sudo apt install tftp-hpa" >&2
    exit 1
fi

# Check netcat
if ! command -v nc >/dev/null 2>&1; then
    echo "Error: netcat (nc) not found." >&2
    echo "Install it with: sudo apt install netcat-openbsd" >&2
    exit 1
fi

# Resolve network interface
IFACE="$(ip route get "$BOOT_IP" 2>/dev/null \
    | awk '{for(i=1;i<=NF;i++) if($i=="dev"){print $(i+1); exit}}' || true)"
if [ -z "${IFACE:-}" ]; then
    echo "Error: cannot determine outgoing interface to ${BOOT_IP}." >&2
    exit 1
fi
if ip route get "$BOOT_IP" 2>/dev/null | grep -qE '\svia\s'; then
    echo "Error: ${BOOT_IP} is reached via a gateway (routed). Must be same L2 segment." >&2
    exit 1
fi

# --- bootloader detection ----------------------------------------------------

# ARP probe: send a UDP poke, check if MAC resolves
ip neigh del "$BOOT_IP" dev "$IFACE" 2>/dev/null || true
bash -c "echo -n X >/dev/udp/$BOOT_IP/69" 2>/dev/null || true
sleep 0.3

nei="$(ip neigh show "$BOOT_IP" dev "$IFACE" 2>/dev/null || true)"
if ! echo "$nei" | grep -Eqi 'lladdr [0-9a-f]{2}(:[0-9a-f]{2}){5}'; then
    echo "Error: bootloader not detected at ${BOOT_IP}." >&2
    echo "Make sure the gateway is in download mode (<RealTek> prompt)." >&2
    exit 1
fi

# A MAC in the neighbour table does not prove the bootloader is there: the
# `ip neigh del` above needs root and fails silently without it, so an entry
# left from an earlier bootloader session still shows a MAC for a while after
# the gateway has gone back to Linux. Require what the restore needs, a TFTP
# server that ACKs a WRQ (same test as flash_install_rtl8196e.sh).
if ! probe_tftp_wrq "$BOOT_IP"; then
    echo "Error: no TFTP server at ${BOOT_IP}: the gateway is not in bootloader mode." >&2
    echo "Make sure the gateway is in download mode (<RealTek> prompt)." >&2
    exit 1
fi

# --- detect bootloader type --------------------------------------------------
# Custom (V1.2/V2) responds to ping; Tuya does not.

BOOTLOADER_TYPE="old"
if ping -c 1 -W 2 "$BOOT_IP" >/dev/null 2>&1; then
    BOOTLOADER_TYPE="custom"
fi

# --- main --------------------------------------------------------------------

echo "========================================="
echo "  GATEWAY RESTORE"
echo "========================================="
echo ""
echo "Image:       $IMAGE ($(numfmt --to=iec-i --suffix=B "$img_size"))"
echo "MD5:         $(md5sum "$IMAGE" | awk '{print $1}')"
# The loader would refuse a bad trailer only after the 16 MiB upload; say so
# now.  A mismatch means the file changed after its trailer was written,
# foreign data means the tail is neither blank nor a trailer: take a fresh
# backup, or `lib/fullflash_crc.sh write FILE` if you trust the file.
# No trailer (1) is still sent: loaders older than V3.1 accept it, and the
# host cannot tell the loader version; the message tells a V3.1 user to sign.
# (if/else, not $?: the script runs under set -e.)
if ff_note=$(ffcrc_check "$IMAGE"); then ff_rc=0; else ff_rc=$?; fi
echo "CRC trailer: $ff_note"
if [ "$ff_rc" -eq 2 ] || [ "$ff_rc" -eq 3 ]; then
    echo "Error: the bootloader would refuse this image; not sending it." >&2
    exit 1
fi
echo "Boot IP:     $BOOT_IP"
echo "Bootloader:  $BOOTLOADER_TYPE"
echo ""
echo "WARNING: This will overwrite the ENTIRE flash chip (16 MiB)."
echo "All data on the gateway will be replaced."
echo ""
if [ "$CONFIRM" != "y" ]; then
    read -r -p "Proceed? [y/N] " confirm
    if [[ ! "$confirm" =~ ^[yY]$ ]]; then echo "Aborted."; exit 0; fi
fi

IMAGE_DIR="$(cd "$(dirname "$IMAGE")" && pwd)"
IMAGE_NAME="$(basename "$IMAGE")"

check_tftp_error() {
    local out="$1"
    echo "$out" | grep -qiE \
        "error|timeout|timed out|refused|failed|unknown host|access denied|disk full|illegal|not connected|unknown transfer"
}

if [ "$BOOTLOADER_TYPE" = "custom" ]; then
    # ---- Custom bootloader: try V2 automatic full-flash restore ----
    # V2 bootloader: receiving a 16 MB file writes it to flash automatically.
    # V1.2: no such feature, but we upload to LOADADDR then FLW manually.

    echo ""
    echo "Uploading image via TFTP..."
    cd "$IMAGE_DIR"
    out=$(timeout 300 tftp "${TFTP_PIN[@]}" -m binary "$BOOT_IP" -c put "$IMAGE_NAME" 2>&1) || true
    cd "$SCRIPT_DIR"

    if check_tftp_error "$out"; then
        echo "Error: TFTP transfer failed: $out" >&2
        exit 1
    fi

    # Check for V2 UDP notification
    NOTIFY_PORT=9999
    echo "Upload complete. Checking for V2 auto-flash notification..."

    # Try to get a UDP notification (V2 sends OK/FAIL on port 9999)
    notify_file=$(mktemp)
    (timeout 180 nc -u -l -p "$NOTIFY_PORT" > "$notify_file" 2>/dev/null) &
    nc_pid=$!

    # Wait for notification (V2 flashes on receive and sends OK/FAIL)
    while kill -0 "$nc_pid" 2>/dev/null; do
        [ -s "$notify_file" ] && { kill "$nc_pid" 2>/dev/null; break; }
        sleep 0.5
    done
    wait "$nc_pid" 2>/dev/null || true
    result=$(tr -d '\0' < "$notify_file" 2>/dev/null || true)
    rm -f "$notify_file"

    if [ "$result" = "OK" ]; then
        echo "Flash Write Succeeded (V2 bootloader)."
        echo ""
        echo "Restore complete. Gateway will reboot."
    elif [ "$result" = "FAIL" ]; then
        echo "Error: flash write FAILED on gateway." >&2
        exit 1
    else
        # V1.2 or no notification — need serial console for FLW
        echo ""
        echo "No auto-flash notification received (V1.2 bootloader)."
        echo ""
        echo "The image was uploaded to the default LOADADDR."
        echo "On the serial console (<RealTek> prompt), verify LOADADDR and type:"
        echo ""
        echo "    LOADADDR 80500000"
        echo ""
        read -r -p "Press Enter when done... "
        echo ""
        echo "Now upload the image again:"
        cd "$IMAGE_DIR"
        out=$(timeout 300 tftp "${TFTP_PIN[@]}" -m binary "$BOOT_IP" -c put "$IMAGE_NAME" 2>&1) || true
        cd "$SCRIPT_DIR"
        if check_tftp_error "$out"; then
            echo "Error: TFTP transfer failed: $out" >&2
            exit 1
        fi
        echo "Upload complete."
        echo ""
        echo "On the serial console, type:"
        echo ""
        echo "    FLW 00000000 80500000 01000000 0"
        echo ""
        echo "Wait for 'Flash Write Succeeded!' before continuing."
        read -r -p "Flash Write Succeeded on serial console? [y/N] " r
        if [[ ! "$r" =~ ^[yY]$ ]]; then echo "Aborted."; exit 1; fi
        echo ""
        echo "Restore complete. Reboot the gateway (type 'reboot' on serial console)."
    fi
else
    # ---- Tuya bootloader: serial console required ----
    if [ ! -t 0 ]; then
        echo "Error: serial console required for Tuya bootloader restore." >&2
        echo "Run this script from an interactive terminal with serial access." >&2
        exit 1
    fi

    echo ""
    echo "Original Tuya bootloader — serial console required."
    echo ""
    echo "On the serial console (<RealTek> prompt), type:"
    echo ""
    echo "    LOADADDR 80500000"
    echo ""
    read -r -p "Press Enter when done... "

    echo ""
    echo "Uploading image via TFTP..."
    cd "$IMAGE_DIR"
    out=$(timeout 300 tftp "${TFTP_PIN[@]}" -m binary "$BOOT_IP" -c put "$IMAGE_NAME" 2>&1) || true
    cd "$SCRIPT_DIR"

    if check_tftp_error "$out"; then
        echo "Error: TFTP transfer failed: $out" >&2
        exit 1
    fi
    echo "Upload complete."

    echo ""
    echo "On the serial console, type:"
    echo ""
    echo "    FLW 00000000 80500000 01000000 0"
    echo ""
    echo "Wait for 'Flash Write Succeeded!' before continuing."
    read -r -p "Flash Write Succeeded on serial console? [y/N] " r
    if [[ ! "$r" =~ ^[yY]$ ]]; then echo "Aborted."; exit 1; fi

    echo ""
    echo "Restore complete. Reboot the gateway (type 'reboot' on serial console)."
fi
