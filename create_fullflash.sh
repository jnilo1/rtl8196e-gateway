#!/bin/bash
# create_fullflash.sh — Assemble and optionally flash a 16 MiB image
#
# Rebuilds userdata with the chosen network configuration, then assembles
# bootloader + kernel + rootfs + userdata into fullflash.bin. Optionally uploads
# the image via TFTP and guides you through the FLW serial console command.
#
# Steps:
#   1. Asks for network configuration (or uses env vars)
#   2. Rebuilds userdata.bin via build_userdata.sh
#   3. Assembles all 4 partitions into fullflash.bin
#   4. (with --flash) Uploads fullflash.bin to the gateway via TFTP
#   5. You type FLW on the serial console to write it to flash (~2 min)
#
# Prerequisites for --flash:
#   - Serial console connected (3.3V UART, 38400 baud)
#   - Gateway in bootloader mode (<RealTek> prompt)
#   - Ethernet cable between host and gateway (same L2 segment)
#   - tftp-hpa client installed (sudo apt install tftp-hpa)
#
# Usage: ./create_fullflash.sh [--flash] [--boot-ip IP] [--output FILE]
#
# Environment variables (for non-interactive use):
#   BOARD       - "lidl" (default) or "sengled-e39-g8c" (selects the kernel image)
#   KERNEL      - "6.18" (default) or "7.2" (selects the kernel line)
#   NET_MODE    - "static", "dhcp", or "skip"
#   IPADDR      - Static IP address for the gateway
#   NETMASK     - Netmask
#   GATEWAY     - Default gateway
#
# Unset network values are not fixed constants: they fall back to gateway.env,
# then to the last install, then to an address derived from THIS host's own LAN
# (see lib/gwconf.sh and gateway.env.example), and only then to the project's
# historic 192.168.1.x.
#
# J. Nilo - March 2026

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RTL_DIR="${SCRIPT_DIR}/3-Main-SoC-Realtek-RTL8196E"

# Shared (board, kernel) → pre-built kernel image resolver.
. "${SCRIPT_DIR}/lib/kernel_image.sh"
# Host-side gateway config: network proposals derived from this machine's LAN,
# and the write-back that remembers what we install. See lib/gwconf.sh.
# shellcheck disable=SC1091
. "${SCRIPT_DIR}/lib/gwconf.sh"
# CRC trailer of the raw image, checked by the bootloader — see lib/fullflash_crc.sh.
. "${SCRIPT_DIR}/lib/fullflash_crc.sh"
# Fixed TFTP source port (TFTP_PIN) — see lib/flash_tftp.sh.
# shellcheck disable=SC1091
. "${SCRIPT_DIR}/lib/flash_tftp.sh"

# BOARD (default lidl) and KERNEL (default 6.18) select the pre-built kernel
# image; a Lidl user who sets neither gets the historical kernel-6.18.img.
BOARD="${BOARD:-lidl}"
KERNEL="${KERNEL:-6.18}"

# The bootloader is board-specific (DRAM bring-up) — resolved from the same
# per-board pre-built layout as the kernel (discussion #140).
BOOTLOADER_IMG="$(resolve_boot_image "$BOARD")" || exit 1
KERNEL_IMG="$(resolve_kernel_image "$BOARD" "$KERNEL")" || exit 1
ROOTFS_IMG="${RTL_DIR}/33-Rootfs/rootfs.bin"
USERDATA_DIR="${RTL_DIR}/34-Userdata"
USERDATA_IMG="${USERDATA_DIR}/userdata.bin"

OUTPUT="${SCRIPT_DIR}/fullflash.bin"
# Bootloader-mode address: BOOT_IP env > gateway.env > the bootloader's compiled
# default. NOT derived from this host's LAN — this script targets a gateway
# already sitting at a bootloader prompt, which nothing can move (lib/gwconf.sh,
# gwconf_cold_boot_ip). The TFTP transfer also does not cross a router.
BOOT_IP="${BOOT_IP:-$(gwconf_cold_boot_ip)}"
DO_FLASH=false

FLASH_SIZE=$((16 * 1024 * 1024))  # 16 MiB

# Partition offsets (must match kernel DTS)
OFF_BOOT=0x000000      # boot+cfg  128 KiB
OFF_KERNEL=0x020000    # kernel    1920 KiB
OFF_ROOTFS=0x200000    # rootfs    2048 KiB
OFF_USERDATA=0x400000  # userdata  12288 KiB

CVIMG_HDR=16  # cvimg header size

# --- argument parsing --------------------------------------------------------

while [ $# -gt 0 ]; do
    case "$1" in
        --flash|-f) DO_FLASH=true ;;
        --boot-ip) shift; BOOT_IP="$1" ;;
        --output|-o) shift; OUTPUT="$1" ;;
        --help|-h)
            echo "Usage: $0 [--flash] [--boot-ip IP] [--output FILE]"
            echo ""
            echo "Rebuilds userdata with network config, then assembles a"
            echo "16 MiB flash image. With --flash, uploads it via TFTP."
            echo ""
            echo "Options:"
            echo "  --flash         Upload via TFTP after assembly"
            echo "  --boot-ip IP    Gateway IP in bootloader (default: ${BOOT_IP})"
            echo "  --output FILE   Output path (default: fullflash.bin)"
            echo ""
            echo "Environment: BOARD, KERNEL, NET_MODE, IPADDR, NETMASK, GATEWAY"
            exit 0
            ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
    shift
done

# --- check source images ----------------------------------------------------

echo ""
echo "========================================="
echo "  CREATE FULLFLASH"
echo "========================================="
echo ""

MISSING=0
for f in "$BOOTLOADER_IMG" "$KERNEL_IMG"; do
    if [ ! -f "$f" ]; then
        echo "Error: $(basename "$f") not found at $f" >&2
        MISSING=1
    fi
done
if [ $MISSING -eq 1 ]; then
    echo "Build the components first (build_bootloader.sh, build_kernel.sh)." >&2
    exit 1
fi

# Rebuild rootfs if missing (skeleton is in git, rootfs.bin is not)
if [ ! -f "$ROOTFS_IMG" ]; then
    echo "rootfs.bin not found — rebuilding..."
    ROOTFS_DIR="${RTL_DIR}/33-Rootfs"
    "${ROOTFS_DIR}/build_rootfs.sh"
fi

# --- build userdata ----------------------------------------------------------

# Work on a temporary copy of the skeleton — never modify the original
SKEL_WORK=$(mktemp -d)
cp -a "${USERDATA_DIR}/skeleton/." "$SKEL_WORK/"
trap 'rm -rf "$SKEL_WORK"' EXIT
export SKELETON_DIR="$SKEL_WORK"

ETH0_CONF="${SKEL_WORK}/etc/eth0.conf"
ETH0_BAK="${SKEL_WORK}/etc/eth0.bak"

    # Network config — "skip" means config already injected by caller
    if [ "${NET_MODE:-}" = "skip" ]; then
        echo "→ Network config preserved from gateway"
        # Record it so the host-side tools keep pointing at this box after the
        # flash. No eth0.conf means the gateway was on DHCP and stays there.
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
        if [ "${NET_MODE:-}" = "static" ] || [ "${NET_MODE:-}" = "dhcp" ]; then
            net_choice="${NET_MODE}"
        else
            echo "Network configuration:"
            echo "  [1] Static IP (recommended)"
            echo "  [2] DHCP"
            read -r -p "Choice [1]: " net_choice
            net_choice="${net_choice:-1}"
            [ "$net_choice" = "1" ] && net_choice="static"
            [ "$net_choice" = "2" ] && net_choice="dhcp"
        fi

        if [ "$net_choice" = "static" ]; then
            # Proposals come from what was configured or installed before, else
            # from this host's own LAN, else from the historic 192.168.1.x
            # constants — never a fixed subnet the user may not be on.
            gwconf_suggest_static
            if [ -z "${NET_MODE:-}" ]; then
                echo "Proposed defaults: ${GWCONF_SUGGEST_SOURCE}."
                read -r -p "IP address [${GWCONF_SUGGEST_IPADDR}]: " IPADDR_IN
                read -r -p "Netmask    [${GWCONF_SUGGEST_NETMASK}]: " NETMASK_IN
                read -r -p "Gateway    [${GWCONF_SUGGEST_GATEWAY}]:   " GATEWAY_IN
                IPADDR="${IPADDR_IN:-${IPADDR:-$GWCONF_SUGGEST_IPADDR}}"
                NETMASK="${NETMASK_IN:-${NETMASK:-$GWCONF_SUGGEST_NETMASK}}"
                GATEWAY="${GATEWAY_IN:-${GATEWAY:-$GWCONF_SUGGEST_GATEWAY}}"
            else
                IPADDR="${IPADDR:-$GWCONF_SUGGEST_IPADDR}"
                NETMASK="${NETMASK:-$GWCONF_SUGGEST_NETMASK}"
                GATEWAY="${GATEWAY:-$GWCONF_SUGGEST_GATEWAY}"
            fi
            printf 'IPADDR=%s\nNETMASK=%s\nGATEWAY=%s\n' "$IPADDR" "$NETMASK" "$GATEWAY" > "$ETH0_CONF"
            echo "→ Static IP: $IPADDR / $NETMASK via $GATEWAY"
            [ -z "${NET_MODE:-}" ] && gwconf_warn_if_taken "$IPADDR" "address"
            gwconf_record_install static "$IPADDR" "$NETMASK" "$GATEWAY"
            # The device's DHCP-failure fallback, in the same subnet.
            gwconf_write_eth0_bak "$ETH0_BAK" "$IPADDR" "$NETMASK" "$GATEWAY"
        else
            rm -f "$ETH0_CONF"
            echo "→ DHCP"
            gwconf_record_install dhcp
            # No lease may ever arrive: leave a reachable fallback behind.
            gwconf_write_eth0_bak "$ETH0_BAK"
        fi
    fi

    echo ""

    echo "Building userdata..."
    "${USERDATA_DIR}/build_userdata.sh" --jffs2-only
    echo ""

# --- check sizes -------------------------------------------------------------

boot_data=$(($(stat -c%s "$BOOTLOADER_IMG") - CVIMG_HDR))
kernel_data=$(stat -c%s "$KERNEL_IMG")           # kept with header
rootfs_data=$(($(stat -c%s "$ROOTFS_IMG") - CVIMG_HDR))
userdata_data=$(($(stat -c%s "$USERDATA_IMG") - CVIMG_HDR))

boot_max=$((OFF_KERNEL - OFF_BOOT - FFCRC_LEN)) # 128 KiB minus the CRC trailer (lib/fullflash_crc.sh)
kernel_max=$((OFF_ROOTFS - OFF_KERNEL))    # 1920 KiB
rootfs_max=$((OFF_USERDATA - OFF_ROOTFS))  # 2048 KiB
userdata_max=$((FLASH_SIZE - OFF_USERDATA)) # 12288 KiB

echo "Image sizes (data written to flash):"
echo "  boot.bin:     $(numfmt --to=iec-i --suffix=B $boot_data) / $(numfmt --to=iec-i --suffix=B $boot_max)"
echo "  $(basename "$KERNEL_IMG"): $(numfmt --to=iec-i --suffix=B $kernel_data) / $(numfmt --to=iec-i --suffix=B $kernel_max) (with cs6c header)"
echo "  rootfs.bin:   $(numfmt --to=iec-i --suffix=B $rootfs_data) / $(numfmt --to=iec-i --suffix=B $rootfs_max)"
echo "  userdata.bin: $(numfmt --to=iec-i --suffix=B $userdata_data) / $(numfmt --to=iec-i --suffix=B $userdata_max)"
echo ""

OVERFLOW=0
if [ $boot_data -gt $boot_max ]; then
    echo "Error: boot.bin ($boot_data) exceeds boot+cfg partition ($boot_max)" >&2
    OVERFLOW=1
fi
if [ $kernel_data -gt $kernel_max ]; then
    echo "Error: $(basename "$KERNEL_IMG") ($kernel_data) exceeds kernel partition ($kernel_max)" >&2
    OVERFLOW=1
fi
if [ $rootfs_data -gt $rootfs_max ]; then
    echo "Error: rootfs.bin ($rootfs_data) exceeds rootfs partition ($rootfs_max)" >&2
    OVERFLOW=1
fi
if [ $userdata_data -gt $userdata_max ]; then
    echo "Error: userdata.bin ($userdata_data) exceeds userdata partition ($userdata_max)" >&2
    OVERFLOW=1
fi
if [ $OVERFLOW -eq 1 ]; then exit 1; fi

# --- assemble fullflash.bin --------------------------------------------------

echo "Assembling fullflash.bin (16 MiB)..."

# Start with 16 MiB of 0xFF (erased NOR flash)
dd if=/dev/zero bs=1M count=16 2>/dev/null | tr '\0' '\377' > "$OUTPUT"

# boot+cfg @ 0x000000 — strip 16-byte cvimg header
tail -c +17 "$BOOTLOADER_IMG" | dd of="$OUTPUT" bs=1 conv=notrunc 2>/dev/null

# kernel @ 0x020000 — KEEP cs6c header (bootloader scans for it at boot)
dd if="$KERNEL_IMG" of="$OUTPUT" bs=1 seek=$((OFF_KERNEL)) conv=notrunc 2>/dev/null

# rootfs @ 0x200000 — strip 16-byte cvimg header
tail -c +17 "$ROOTFS_IMG" | dd of="$OUTPUT" bs=1 seek=$((OFF_ROOTFS)) conv=notrunc 2>/dev/null

# userdata @ 0x400000 — strip 16-byte cvimg header
tail -c +17 "$USERDATA_IMG" | dd of="$OUTPUT" bs=1 seek=$((OFF_USERDATA)) conv=notrunc 2>/dev/null

# --- verify ------------------------------------------------------------------

echo ""
echo "Verifying..."

ERRORS=0

actual_size=$(stat -c%s "$OUTPUT")
if [ "$actual_size" -ne "$FLASH_SIZE" ]; then
    echo "  FAIL: size is $actual_size (expected $FLASH_SIZE)" >&2
    ERRORS=1
else
    echo "  Size: 16 MiB [OK]"
fi

# Dumped with od (coreutils, always present) rather than xxd, which on Debian is
# a separate package that is not installed by default (issue #147).
check_magic() {
    local label="$1" offset="$2" expected="$3"
    local nbytes=$(( ${#expected} / 2 ))
    actual=$(dd if="$OUTPUT" bs=1 skip="$offset" count="$nbytes" 2>/dev/null \
             | od -An -tx1 -v | tr -d ' \n')
    if [ "$actual" = "$expected" ]; then
        echo "  ${label} @ $(printf '0x%06X' $offset): $expected [OK]"
    else
        echo "  ${label} @ $(printf '0x%06X' $offset): $actual (expected $expected) [FAIL]" >&2
        ERRORS=1
    fi
}

check_magic "boot+cfg" $((OFF_BOOT))     "0bf00004"
check_magic "kernel"   $((OFF_KERNEL))    "63733663"  # cs6c
check_magic "rootfs"   $((OFF_ROOTFS))    "68737173"  # hsqs
check_magic "userdata" $((OFF_USERDATA))  "1985"       # JFFS2 magic

if [ $ERRORS -ne 0 ]; then
    echo ""
    echo "VERIFICATION FAILED — do not flash this image." >&2
    rm -f "$OUTPUT"
    exit 1
fi

# CRC trailer at 0x1FFF0 (see lib/fullflash_crc.sh), written last so it
# covers the final image.
ff_crc=$(ffcrc_write "$OUTPUT") || { echo "Error: could not write the CRC trailer" >&2; rm -f "$OUTPUT"; exit 1; }
echo "  CRC trailer @ 0x01FFF0: ${ff_crc} [OK]"

echo ""
echo "========================================="
echo "  FULLFLASH READY"
echo "========================================="
echo ""
echo "  $(ls -lh "$OUTPUT" | awk '{print $NF, $5}')"
echo "  MD5: $(md5sum "$OUTPUT" | awk '{print $1}')"
echo ""

# --- optional flash ----------------------------------------------------------

if [ "$DO_FLASH" != "true" ]; then
    echo "To flash, re-run with --flash or manually:"
    echo "  1. Gateway in bootloader mode (<RealTek> prompt)"
    echo "  2. tftp -m binary ${BOOT_IP} -c put fullflash.bin"
    echo "  3. On serial console: FLW 0 80500000 01000000"
    echo "  4. Wait ~2 minutes for the write to complete"
    echo "  5. J BFC00000  or power cycle"
    exit 0
fi

# Check tftp-hpa client
tftp_usage="$(tftp --help 2>&1 || true)"
if ! command -v tftp >/dev/null 2>&1 || ! echo "$tftp_usage" | grep -q '\-c'; then
    echo "Error: tftp-hpa client not found (need the -c flag)." >&2
    echo "Install it with: sudo apt install tftp-hpa" >&2
    exit 1
fi

echo "========================================="
echo "  FLASH VIA TFTP"
echo "========================================="
echo ""
echo "Make sure:"
echo "  - Serial console is connected (38400 8N1)"
echo "  - Gateway shows the <RealTek> prompt"
echo "  - Ethernet cable between this PC and the gateway"
echo ""
read -r -p "Ready to upload? [y/N] " r
if [[ ! "$r" =~ ^[yY]$ ]]; then echo "Aborted."; exit 0; fi

echo ""
echo "Uploading fullflash.bin (16 MiB) to ${BOOT_IP}..."
cd "$SCRIPT_DIR"
out=$(timeout 300 tftp "${TFTP_PIN[@]}" -m binary "$BOOT_IP" -c put fullflash.bin 2>&1) || true

if echo "$out" | grep -qiE "error|timeout|timed out|refused|failed|unknown host|access denied|disk full|illegal|not connected|unknown transfer"; then
    echo "Error: TFTP transfer failed: $out" >&2
    exit 1
fi

echo "Upload OK."
echo ""
echo "On the serial console, type:"
echo ""
echo "    FLW 0 80500000 01000000"
echo ""
echo "Wait for the <RealTek> prompt (~2 minutes)."
echo "Then: J BFC00000  or power cycle the gateway."
echo ""
read -r -p "Flash write succeeded? [y/N] " r
if [[ ! "$r" =~ ^[yY]$ ]]; then echo "Aborted."; exit 1; fi

echo ""
echo "========================================="
echo "  INSTALLATION COMPLETE"
echo "========================================="
echo ""
echo "The gateway will boot into Linux."
if [ -n "${IPADDR:-}" ]; then
    echo "SSH: root@${IPADDR}:22 (no password)"
else
    echo "SSH: port 22, no password — the gateway took a DHCP lease, so find its"
    echo "     address in your router's lease table (it announces itself as"
    echo "     $(gwconf_device_hostname))."
fi
echo ""
