#!/bin/bash
# backup_gateway.sh — Unified backup script for Lidl Silvercrest Gateway
#
# Detects the gateway state (custom Linux, Tuya Linux, or bootloader) and
# chooses the best backup method automatically. Never modifies the system.
#
# Usage: ./backup_gateway.sh [--linux-ip IP] [--boot-ip IP] [--output DIR] [--help]
#
# J. Nilo - March 2026

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SPLIT_FLASH="${SCRIPT_DIR}/3-Main-SoC-Realtek-RTL8196E/30-Backup-Restore/split_flash.sh"
# Host-side gateway address resolution — see lib/gwconf.sh.
# shellcheck disable=SC1091
. "${SCRIPT_DIR}/lib/gwconf.sh"
# CRC trailer of the assembled fullflash.bin — see lib/fullflash_crc.sh.
. "${SCRIPT_DIR}/lib/fullflash_crc.sh"

# Addresses: flag > env > gateway.env > last install / last box reached > a
# value derived from this host's own LAN > the historic 192.168.1.x constants.
# LINUX_IP_SOURCE feeds the banner; it stays empty when the address was stated
# explicitly (env here, --linux-ip below), which is when there is nothing to
# explain. Resolved through the variable-setting form so the source survives.
LINUX_IP_SOURCE=""
if [ -z "${LINUX_IP:-}" ]; then
    gwconf_resolve_gateway
    LINUX_IP="$GWCONF_ADDR"
    LINUX_IP_SOURCE="$GWCONF_ADDR_SOURCE"
fi
BOOT_IP="${BOOT_IP:-$(gwconf_cold_boot_ip)}"
SSH_USER="${SSH_USER:-root}"
BACKUP_DIR="${BACKUP_DIR:-}"
FLASH_SIZE=$((16 * 1024 * 1024))  # 16 MiB

# --- argument parsing --------------------------------------------------------

while [ $# -gt 0 ]; do
    case "$1" in
        --linux-ip) shift; LINUX_IP="$1"; LINUX_IP_SOURCE="" ;;
        --boot-ip)  shift; BOOT_IP="$1" ;;
        --output)   shift; BACKUP_DIR="$1" ;;
        --help|-h)
            echo "Usage: $0 [--linux-ip IP] [--boot-ip IP] [--output DIR] [--help]"
            echo ""
            echo "Detects gateway state and backs up all flash partitions."
            echo ""
            echo "Options:"
            echo "  --linux-ip IP   Gateway IP under Linux (default: ${LINUX_IP})"
            echo "  --boot-ip  IP   Gateway IP in bootloader (default: ${BOOT_IP})"
            echo "  --output   DIR  Output directory (default: backups/YYYYMMDD-HHMM)"
            echo ""
            echo "Environment variables: LINUX_IP, BOOT_IP, BACKUP_DIR, SSH_USER"
            exit 0
            ;;
        *) echo "Unknown option: $1. Use --help for usage."; exit 1 ;;
    esac
    shift
done

if [ -z "$BACKUP_DIR" ]; then
    BACKUP_DIR="${SCRIPT_DIR}/backups/$(date '+%Y%m%d-%H%M')"
fi

# --- detection helpers -------------------------------------------------------

# Check if bootloader is reachable (ARP resolves on BOOT_IP)
bootloader_reachable() {
    local iface
    iface="$(ip route get "$BOOT_IP" 2>/dev/null \
        | awk '{for(i=1;i<=NF;i++) if($i=="dev"){print $(i+1); exit}}' || true)"
    [ -z "$iface" ] && return 1

    ip neigh del "$BOOT_IP" dev "$iface" 2>/dev/null || true
    bash -c "echo -n X >/dev/udp/$BOOT_IP/69" 2>/dev/null || true
    sleep 0.3

    local nei
    nei="$(ip neigh show "$BOOT_IP" dev "$iface" 2>/dev/null || true)"
    echo "$nei" | grep -Eqi 'lladdr [0-9a-f]{2}(:[0-9a-f]{2}){5}'
}

# Check if SSH port is open
ssh_reachable() {
    local ip="$1" port="$2"
    timeout 2 bash -c "echo >/dev/tcp/$ip/$port" 2>/dev/null
}

# Detect gateway state: custom_linux, tuya_linux, bootloader, or fail
# SSH is tested first — it's a definitive TCP check, whereas ARP probes
# can give false positives from stale neighbour entries.
detect_state() {
    if ssh_reachable "$LINUX_IP" 22; then
        echo "custom_linux"
    elif ssh_reachable "$LINUX_IP" 2333; then
        echo "tuya_linux"
    elif bootloader_reachable; then
        echo "bootloader"
    else
        return 1
    fi
}

# Detect partition layout from flash content
# The original Lidl/Tuya firmware has "/tuya/" paths in the jffs2 partition;
# custom firmware does not.
detect_layout() {
    local image="$1"
    if grep -qao '/tuya/' "$image" 2>/dev/null; then
        echo "lidl"
    else
        echo "custom"
    fi
}

# --- SSH backup --------------------------------------------------------------

# Back up all MTD partitions via SSH cat /dev/mtdX
# Args: ssh_port
backup_via_ssh() {
    local ssh_port="$1"
    # HostKeyAlgorithms=+ssh-rsa always included: harmless on modern servers,
    # required for old Tuya Dropbear (some users also change port 2333 to 22).
    local ssh_opts="-p ${ssh_port} -o HostKeyAlgorithms=+ssh-rsa -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=10"

    # SSH multiplexing: open one connection, reuse for all commands (single password prompt)
    local ssh_ctl
    ssh_ctl=$(mktemp -u /tmp/backup-ssh-XXXXXX)
    ssh_opts="${ssh_opts} -o ControlMaster=auto -o ControlPath=${ssh_ctl} -o ControlPersist=60"
    # shellcheck disable=SC2086
    trap "ssh $ssh_opts -O exit ${SSH_USER}@${LINUX_IP} 2>/dev/null; rm -f ${ssh_ctl}" RETURN

    echo "Connecting to ${LINUX_IP}:${ssh_port}..."
    local proc_mtd
    # shellcheck disable=SC2086
    proc_mtd=$(ssh $ssh_opts "${SSH_USER}@${LINUX_IP}" "cat /proc/mtd") || {
        echo "Error: cannot connect to ${LINUX_IP}:${ssh_port}." >&2
        exit 1
    }

    echo "$proc_mtd"
    echo ""

    # Parse partitions: "mtd0: 00020000 00010000 "boot+cfg""
    local -a mtd_devs=()
    local -a mtd_names=()
    local -a mtd_sizes=()

    while IFS= read -r line; do
        local dev size_hex name
        dev=$(echo "$line" | awk -F: '{print $1}')
        size_hex=$(echo "$line" | awk '{print $2}')
        # Extract quoted name
        name=$(echo "$line" | sed 's/.*"\(.*\)".*/\1/')
        mtd_devs+=("$dev")
        mtd_names+=("$name")
        mtd_sizes+=("$((16#${size_hex}))")
    done < <(echo "$proc_mtd" | tail -n +2)

    local n_parts=${#mtd_devs[@]}
    echo "Found ${n_parts} partitions."
    echo ""

    # Dump each partition
    local i
    for i in $(seq 0 $((n_parts - 1))); do
        local dev="${mtd_devs[$i]}"
        local name="${mtd_names[$i]}"
        local expected="${mtd_sizes[$i]}"
        local outfile="${BACKUP_DIR}/${dev}_${name}.bin"

        echo "Dumping ${dev} (${name}, ${expected} bytes)..."
        # shellcheck disable=SC2086
        ssh $ssh_opts "${SSH_USER}@${LINUX_IP}" "cat /dev/${dev}" > "$outfile" 2>/dev/null

        local actual
        actual=$(stat -c%s "$outfile" 2>/dev/null || echo 0)
        if [ "$actual" -eq "$expected" ]; then
            echo "  ${dev}_${name}.bin: ${actual} bytes [OK]"
        else
            echo "  ${dev}_${name}.bin: ${actual} bytes [EXPECTED: ${expected}] [MISMATCH]" >&2
        fi
    done

    # Concatenate into fullflash.bin
    echo ""
    echo "Creating fullflash.bin..."
    local concat_files=()
    for i in $(seq 0 $((n_parts - 1))); do
        concat_files+=("${BACKUP_DIR}/${mtd_devs[$i]}_${mtd_names[$i]}.bin")
    done
    cat "${concat_files[@]}" > "${BACKUP_DIR}/fullflash.bin"

    # Pad to 16 MB if needed (last partition may not reach end of flash)
    local current_size
    current_size=$(stat -c%s "${BACKUP_DIR}/fullflash.bin" 2>/dev/null || echo 0)
    if [ "$current_size" -lt "$FLASH_SIZE" ]; then
        local pad=$((FLASH_SIZE - current_size))
        echo "Padding fullflash.bin with ${pad} bytes (0xFF) to reach 16 MiB..."
        dd if=/dev/zero bs=1 count="$pad" 2>/dev/null | tr '\0' '\377' >> "${BACKUP_DIR}/fullflash.bin"
    fi

    # The trailer read back from the chip (if any) described the image that
    # was flashed, not what the flash holds now: recompute it so the V3.1
    # loader accepts the restore and checks it end to end.
    local crc
    crc=$(ffcrc_write "${BACKUP_DIR}/fullflash.bin") || { echo "Error: could not write the CRC trailer" >&2; exit 1; }
    echo "CRC trailer written (${crc}): the bootloader checks it on restore."
}

handle_bootloader() {
    echo "Backup via SSH is required. The gateway must be running Linux."
    echo ""
    echo "To back up from bootloader mode:"
    echo "  1. Power cycle the gateway and let it boot into Linux"
    echo "  2. Run:  $0 --linux-ip <GATEWAY_IP>"
    echo ""
    exit 1
}

# --- main --------------------------------------------------------------------

mkdir -p "$BACKUP_DIR"

# Start logging (duplicate stdout+stderr to backup.log)
exec > >(tee "${BACKUP_DIR}/backup.log") 2>&1

echo "========================================="
echo "  GATEWAY BACKUP"
echo "========================================="
echo ""
echo "Linux IP:    ${LINUX_IP}$(gwconf_source_note "$LINUX_IP_SOURCE")"
echo "Boot IP:     ${BOOT_IP}"
echo "Output:      ${BACKUP_DIR}"
echo ""

echo "Detecting gateway state..."
STATE=$(detect_state) || {
    echo "Error: gateway unreachable." >&2
    echo "  - No bootloader at ${BOOT_IP}" >&2
    echo "  - No SSH at ${LINUX_IP}:22 (custom firmware)" >&2
    echo "  - No SSH at ${LINUX_IP}:2333 (Tuya firmware)" >&2
    exit 1
}
echo "State: ${STATE}"
echo ""

case "$STATE" in
    custom_linux)
        echo "Backing up via SSH (port 22, custom firmware)..."
        echo ""
        backup_via_ssh 22
        # We just held a session with our own firmware at this address —
        # remember it so the other scripts can find the box without an argument.
        gwconf_record_seen "$LINUX_IP"
        ;;
    tuya_linux)
        echo "Backing up via SSH (port 2333, Tuya firmware)..."
        echo ""
        backup_via_ssh 2333
        ;;
    bootloader)
        echo "Gateway is in bootloader mode."
        echo ""
        handle_bootloader
        ;;
esac

# --- verify ------------------------------------------------------------------

echo ""
echo "========================================="
echo "  VERIFICATION"
echo "========================================="
echo ""

if [ -f "${BACKUP_DIR}/fullflash.bin" ]; then
    size=$(stat -c%s "${BACKUP_DIR}/fullflash.bin" 2>/dev/null || echo 0)
    if [ "$size" -eq "$FLASH_SIZE" ]; then
        md5=$(md5sum "${BACKUP_DIR}/fullflash.bin" | awk '{print $1}')
        echo "fullflash.bin: ${size} bytes (16 MiB) [OK]"
        echo "MD5: ${md5}"
    else
        echo "fullflash.bin: ${size} bytes [EXPECTED: ${FLASH_SIZE}] [MISMATCH]" >&2
    fi
else
    echo "Warning: fullflash.bin not found." >&2
fi

echo ""
echo "Backup files:"
ls -lh "${BACKUP_DIR}/"*.bin 2>/dev/null || echo "  (no .bin files)"
echo ""
echo "Log: ${BACKUP_DIR}/backup.log"
echo ""
echo "Backup complete."
