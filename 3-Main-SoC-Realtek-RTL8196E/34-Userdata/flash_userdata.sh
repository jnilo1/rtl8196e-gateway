#!/bin/bash
# flash_userdata.sh — Configure network, rebuild and flash userdata partition
#
# Asks for network configuration (static IP or DHCP), rebuilds userdata.bin,
# then uploads it to the device in download mode via TFTP.
#
# Usage: ./flash_userdata.sh [IP]
#   IP - Target IP in bootloader mode. Defaults to BOOT_IP, then gateway.env,
#        then an address on this host's own segment (see lib/gwconf.sh).
#
# Environment variables (optional, for non-interactive use):
#   NET_MODE       - "static" or "dhcp" (skip network prompt)
#   IPADDR         - Static IP address for the gateway
#   NETMASK        - Netmask
#   GATEWAY        - Default gateway
#
# Unset network values are not fixed constants: they fall back to gateway.env,
# then to the last install, then to an address derived from THIS host's own LAN
# (see lib/gwconf.sh and gateway.env.example), and only then to the project's
# historic 192.168.1.x.
#   CONFIRM        - Set to "y" to skip the "Proceed?" prompt
#   TRIES          - ARP probe attempts (default: 10)
#   PORT           - UDP port used to trigger ARP (default: 69)
#   SLEEP_BETWEEN  - Pause between ARP probes in seconds (default: 0.2)
#
# J. Nilo - December 2025

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# Shared safe-retry TFTP upload helper (probe_tftp_wrq, tftp_put_safe).
. "$SCRIPT_DIR/../../lib/flash_tftp.sh"
# Host-side gateway config: network proposals derived from this machine's LAN,
# and the write-back that remembers what we install. See lib/gwconf.sh.
# shellcheck disable=SC1091
. "$SCRIPT_DIR/../../lib/gwconf.sh"
# Bootloader-mode address: argument > BOOT_IP env > gateway.env > the
# bootloader's compiled default. NOT derived from this host's LAN: the gateway is
# already at a bootloader prompt (lib/gwconf.sh, gwconf_cold_boot_ip).
TARGET_IP="${1:-${BOOT_IP:-$(gwconf_cold_boot_ip)}}"

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

TRIES="${TRIES:-10}"
PORT="${PORT:-69}"
SLEEP_BETWEEN="${SLEEP_BETWEEN:-0.2}"

# Work on a temporary copy of the skeleton — never modify the original
# If SKELETON_DIR is already set by caller (e.g. flash_remote.sh with
# gateway config injected), use it directly; otherwise create our own.
if [ -n "${SKELETON_DIR:-}" ]; then
    SKEL_WORK="$SKELETON_DIR"
else
    SKEL_WORK=$(mktemp -d)
    cp -a "${SCRIPT_DIR}/skeleton/." "$SKEL_WORK/"
    trap 'rm -rf "$SKEL_WORK"' EXIT
    export SKELETON_DIR="$SKEL_WORK"
fi

ETH0_CONF="${SKEL_WORK}/etc/eth0.conf"
ETH0_BAK="${SKEL_WORK}/etc/eth0.bak"

# --- Network configuration -------------------------------------------------

# "skip" = config already in skeleton (preserved by caller)
if [ "${NET_MODE:-}" = "skip" ]; then
    # Record the preserved config so the host-side tools keep pointing at this
    # box after the flash. No eth0.conf means the gateway was on DHCP.
    if [ -f "$ETH0_CONF" ]; then
        keep_ip="$(gwconf_read_key "$ETH0_CONF" IPADDR  || true)"
        keep_mask="$(gwconf_read_key "$ETH0_CONF" NETMASK || true)"
        keep_gw="$(gwconf_read_key "$ETH0_CONF" GATEWAY || true)"
        gwconf_record_install static "$keep_ip" "$keep_mask" "$keep_gw"
        gwconf_write_eth0_bak "$ETH0_BAK" "$keep_ip" "$keep_mask" "$keep_gw"
    else
        gwconf_record_install dhcp
        gwconf_write_eth0_bak "$ETH0_BAK"
    fi
else
    if [ -n "${NET_MODE:-}" ]; then
        net_choice="$NET_MODE"
    else
        echo "Network configuration for the gateway:"
        echo "  [1] Static IP (recommended)"
        echo "  [2] DHCP"
        read -r -p "Choice [1]: " net_choice
        net_choice="${net_choice:-1}"
    fi

    if [ "$net_choice" = "static" ] || [ "$net_choice" = "1" ]; then
        # Proposals come from what was configured or installed before, else from
        # this host's own LAN, else from the historic 192.168.1.x constants —
        # never a fixed subnet the user may not be on.
        gwconf_suggest_static
        if [ -z "${NET_MODE:-}" ]; then
            echo "Proposed defaults: ${GWCONF_SUGGEST_SOURCE}."
            read -r -p "IP address [${GWCONF_SUGGEST_IPADDR}]: " IPADDR
            read -r -p "Netmask    [${GWCONF_SUGGEST_NETMASK}]: " NETMASK
            read -r -p "Gateway    [${GWCONF_SUGGEST_GATEWAY}]:   " GATEWAY
        fi
        IPADDR="${IPADDR:-$GWCONF_SUGGEST_IPADDR}"
        NETMASK="${NETMASK:-$GWCONF_SUGGEST_NETMASK}"
        GATEWAY="${GATEWAY:-$GWCONF_SUGGEST_GATEWAY}"
        printf 'IPADDR=%s\nNETMASK=%s\nGATEWAY=%s\n' "$IPADDR" "$NETMASK" "$GATEWAY" > "$ETH0_CONF"
        # Optional DNS/domain (defaults: gateway IP, no search domain)
        if [ -z "${NET_MODE:-}" ]; then
            read -r -p "DNS server [$GATEWAY]: " DNS
            read -r -p "Search domain []: " DOMAIN
        fi
        [ -n "${DNS:-}" ] && echo "DNS=$DNS" >> "$ETH0_CONF"
        [ -n "${DOMAIN:-}" ] && echo "DOMAIN=$DOMAIN" >> "$ETH0_CONF"
        echo "→ Static IP: $IPADDR / $NETMASK via $GATEWAY"
        [ -z "${NET_MODE:-}" ] && gwconf_warn_if_taken "$IPADDR" "address"
        gwconf_record_install static "$IPADDR" "$NETMASK" "$GATEWAY"
        # The device's DHCP-failure fallback, in the same subnet.
        gwconf_write_eth0_bak "$ETH0_BAK" "$IPADDR" "$NETMASK" "$GATEWAY"

        # Hand the address to the host-side Docker stacks. This used to sed the
        # address into the committed compose file and into z2m/configuration.yaml,
        # which left the working tree dirty after every install. The OTBR compose
        # file now reads RCP_HOST from this .env instead — Compose picks it up
        # automatically from the compose file's own directory — and .env is
        # gitignored. Zigbee2MQTT reads its port from configuration.yaml, which
        # the docs already have the user edit, so we print the value rather than
        # rewrite a tracked file behind their back.
        DOCKER_DIR="${SCRIPT_DIR}/../../2-Zigbee-Radio-Silabs-EFR32/26-OT-RCP/docker"
        if [ -d "$DOCKER_DIR" ] && [ -w "$DOCKER_DIR" ]; then
            if {
                   echo "# Written by flash_userdata.sh — the gateway address the"
                   echo "# compose files in this directory point at. Safe to edit."
                   echo "RCP_HOST=${IPADDR}"
               } > "$DOCKER_DIR/.env" 2>/dev/null; then
                echo "→ OTBR docker stack pointed at ${IPADDR} (26-OT-RCP/docker/.env)"
                echo "  For the Zigbee2MQTT stack, set this in 26-OT-RCP/docker/z2m/configuration.yaml:"
                echo "      serial: { port: tcp://${IPADDR}:8888 }"
            fi
        fi
    else
        rm -f "$ETH0_CONF"
        echo "→ DHCP"
        gwconf_record_install dhcp
        # No lease may ever arrive: leave a reachable fallback behind.
        gwconf_write_eth0_bak "$ETH0_BAK"
    fi
fi

# --- Rebuild ---------------------------------------------------------------

echo "Rebuilding userdata..."
if [ "${BUILD_QUIET:-}" = "1" ]; then
    "${SCRIPT_DIR}/build_userdata.sh" --jffs2-only -q
else
    "${SCRIPT_DIR}/build_userdata.sh" --jffs2-only
fi
echo ""

# --- helpers ---------------------------------------------------------------

get_iface_for_ip() {
    local ip="$1"
    ip route get "$ip" 2>/dev/null \
        | awk '/ dev /{for (i=1;i<=NF;i++) if ($i=="dev") {print $(i+1); exit}}' || true
}

neigh_has_lladdr() {
    echo "$1" | grep -Eqi 'lladdr [0-9a-f]{2}(:[0-9a-f]{2}){5}'
}

trigger_kernel_arp_via_udp() {
    local ip="$1" port="${2:-69}"
    ( echo -n X >"/dev/udp/$ip/$port" 2>/dev/null || true ) &
    local pid=$!
    sleep 0.3
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
}

check_bootloader_reachable() {
    local ip="$1" iface="$2"
    ip neigh del "$ip" dev "$iface" 2>/dev/null || true
    for _ in $(seq 1 "$TRIES"); do
        trigger_kernel_arp_via_udp "$ip" "$PORT"
        local nei
        nei="$(ip neigh show "$ip" dev "$iface" 2>/dev/null || true)"
        if [ -n "$nei" ] && neigh_has_lladdr "$nei"; then return 0; fi
        sleep "$SLEEP_BETWEEN"
    done
    return 1
}

# --- Flash -----------------------------------------------------------------

IMAGE="${SCRIPT_DIR}/userdata.bin"
SIZE=$(stat -c%s "$IMAGE" 2>/dev/null || stat -f%z "$IMAGE")

if [ "${BOOTLOADER_CONFIRMED:-}" != "1" ]; then
    echo "Checking if gateway is in boot mode..."

    IFACE="$(get_iface_for_ip "$TARGET_IP")"
    if [ -z "$IFACE" ]; then
        echo "Error: cannot determine outgoing interface to ${TARGET_IP}." >&2
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

echo "Flashing userdata.bin (${SIZE} bytes) to ${TARGET_IP}..."
echo ""
if [ "${CONFIRM:-}" != "y" ]; then
    read -r -p "Proceed? [y/N] " confirm
    if [[ ! "$confirm" =~ ^[yY]$ ]]; then
        echo "Aborted."
        exit 0
    fi
fi

NOTIFY_PORT=9999
NOTIFY_TMO=180

echo "Note: userdata is 12 MB — transfer and flash may take 1-2 minutes."
cd "$SCRIPT_DIR"
if ! tftp_put_safe "$TARGET_IP" userdata.bin 3 120 >/dev/null; then
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
