#!/bin/bash
# build_fullflash.sh — Build a complete 16 MiB flash image for the gateway
#
# Assembles bootloader + kernel + rootfs + userdata into a single fullflash.bin
# that can be written to the SPI NOR flash via TFTP + FLW.
#
# Rebuilds userdata with the chosen network configuration, then assembles
# all 4 partitions, verifies magic bytes, and outputs fullflash.bin.
#
# Usage: ./build_fullflash.sh [-q] [--help]
#
# Options:
#   -q, --quiet   Suppress non-essential output (banners, image sizes, assembly
#                 details, verification line-by-line). Keeps: config → lines,
#                 errors, and a single summary line. Used by flash_install.
#
# Environment variables (for non-interactive use):
#   BOARD       - "lidl" (default) or "sengled-e39-g8c" (selects the kernel image)
#   KERNEL      - "6.18" (default) or "7.2" (selects the kernel line)
#   NET_MODE    - "static", "dhcp", or "skip" (config already injected by caller)
#   IPADDR      - Static IP address for the gateway
#   NETMASK     - Netmask
#   GATEWAY     - Default gateway
#
# Unset network values are not fixed constants: they fall back to gateway.env,
# then to the last install, then to an address derived from THIS host's own LAN
# (see lib/gwconf.sh and gateway.env.example), and only then to the project's
# historic 192.168.1.x. The chosen configuration is recorded so the other
# host-side scripts can find the gateway afterwards.
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
QUIET=0

FLASH_SIZE=$((16 * 1024 * 1024))  # 16 MiB

# Partition offsets (must match kernel DTS)
OFF_BOOT=0x000000      # boot+cfg  128 KiB
OFF_KERNEL=0x020000    # kernel    1920 KiB
OFF_ROOTFS=0x200000    # rootfs    2048 KiB
OFF_USERDATA=0x400000  # userdata  12288 KiB

# --- argument parsing --------------------------------------------------------

while [ $# -gt 0 ]; do
    case "$1" in
        -q|--quiet) QUIET=1 ;;
        --help|-h)
            echo "Usage: $0 [-q|--quiet] [--help]"
            echo ""
            echo "Builds a complete 16 MiB flash image (fullflash.bin)."
            echo "Asks for network configuration, rebuilds userdata,"
            echo "then assembles all 4 partitions into a single image."
            echo ""
            echo "Options:"
            echo "  -q, --quiet   Suppress non-essential output"
            echo ""
            echo "Environment: BOARD, KERNEL, NET_MODE, IPADDR, NETMASK, GATEWAY"
            exit 0
            ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
    shift
done

# --- check source images ----------------------------------------------------

log() { [ "$QUIET" -eq 0 ] && echo "$@" || true; }

log ""
log "========================================="
log "  BUILD FULLFLASH"
log "========================================="
log ""

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

# Always rebuild rootfs from skeleton to avoid stale images
log "Building rootfs.bin from skeleton..."
ROOTFS_DIR="${RTL_DIR}/33-Rootfs"
if [ "$QUIET" -eq 1 ]; then
    "${ROOTFS_DIR}/build_rootfs.sh" >/dev/null
else
    "${ROOTFS_DIR}/build_rootfs.sh"
fi

# --- build userdata ----------------------------------------------------------

# Work on a temporary copy of the skeleton — never modify the original
if [ -n "${SKELETON_DIR:-}" ]; then
    # Caller already prepared a working copy (e.g. flash_install_rtl8196e.sh)
    SKEL_WORK="$SKELETON_DIR"
else
    SKEL_WORK=$(mktemp -d)
    cp -a "${USERDATA_DIR}/skeleton/." "$SKEL_WORK/"
    trap 'rm -rf "$SKEL_WORK"' EXIT
fi
export SKELETON_DIR="$SKEL_WORK"

ETH0_CONF="${SKEL_WORK}/etc/eth0.conf"
ETH0_BAK="${SKEL_WORK}/etc/eth0.bak"

    # Network config — "skip" means config already injected by caller
    if [ "${NET_MODE:-}" = "skip" ]; then
        # Config already in the skeleton (preserved by the caller). Record what
        # it says, so the host-side tools keep pointing at this box after the
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
    echo "Generating disk image... be patient"

    log "Building userdata..."
    if [ "$QUIET" -eq 1 ]; then
        "${USERDATA_DIR}/build_userdata.sh" --jffs2-only -q
    else
        "${USERDATA_DIR}/build_userdata.sh" --jffs2-only
    fi
    log ""

# --- check sizes -------------------------------------------------------------

CVIMG_HDR=16  # cvimg header size

boot_data=$(($(stat -c%s "$BOOTLOADER_IMG") - CVIMG_HDR))
kernel_data=$(stat -c%s "$KERNEL_IMG")           # kept with header
rootfs_data=$(($(stat -c%s "$ROOTFS_IMG") - CVIMG_HDR))
userdata_data=$(($(stat -c%s "$USERDATA_IMG") - CVIMG_HDR))

boot_max=$((OFF_KERNEL - OFF_BOOT - FFCRC_LEN)) # 128 KiB minus the CRC trailer (lib/fullflash_crc.sh)
kernel_max=$((OFF_ROOTFS - OFF_KERNEL))    # 1920 KiB
rootfs_max=$((OFF_USERDATA - OFF_ROOTFS))  # 2048 KiB
userdata_max=$((FLASH_SIZE - OFF_USERDATA)) # 12288 KiB

log "Image sizes (data written to flash):"
log "  boot.bin:     $(numfmt --to=iec-i --suffix=B $boot_data) / $(numfmt --to=iec-i --suffix=B $boot_max)"
log "  $(basename "$KERNEL_IMG"): $(numfmt --to=iec-i --suffix=B $kernel_data) / $(numfmt --to=iec-i --suffix=B $kernel_max) (with cs6c header)"
log "  rootfs.bin:   $(numfmt --to=iec-i --suffix=B $rootfs_data) / $(numfmt --to=iec-i --suffix=B $rootfs_max)"
log "  userdata.bin: $(numfmt --to=iec-i --suffix=B $userdata_data) / $(numfmt --to=iec-i --suffix=B $userdata_max)"
log ""

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

log "Assembling fullflash.bin (16 MiB)..."

# Start with 16 MiB of 0xFF (erased NOR flash)
dd if=/dev/zero bs=1M count=16 2>/dev/null | tr '\0' '\377' > "$OUTPUT"

# boot+cfg @ 0x000000 — strip 16-byte cvimg header
#   On flash: raw bootloader code (starts with 0bf0...)
tail -c +17 "$BOOTLOADER_IMG" | dd of="$OUTPUT" bs=1 conv=notrunc 2>/dev/null

# kernel @ 0x020000 — KEEP cs6c header (bootloader scans for it at boot)
#   On flash: cs6c header + compressed kernel
dd if="$KERNEL_IMG" of="$OUTPUT" bs=1 seek=$((OFF_KERNEL)) conv=notrunc 2>/dev/null

# rootfs @ 0x200000 — strip 16-byte cvimg header
#   On flash: raw squashfs (starts with hsqs)
tail -c +17 "$ROOTFS_IMG" | dd of="$OUTPUT" bs=1 seek=$((OFF_ROOTFS)) conv=notrunc 2>/dev/null

# userdata @ 0x400000 — strip 16-byte cvimg header
#   On flash: raw JFFS2 (starts with 1985)
tail -c +17 "$USERDATA_IMG" | dd of="$OUTPUT" bs=1 seek=$((OFF_USERDATA)) conv=notrunc 2>/dev/null

# --- verify ------------------------------------------------------------------

log ""
log "Verifying..."

ERRORS=0

# Check total size
actual_size=$(stat -c%s "$OUTPUT")
if [ "$actual_size" -ne "$FLASH_SIZE" ]; then
    echo "  FAIL: size is $actual_size (expected $FLASH_SIZE)" >&2
    ERRORS=1
else
    log "  Size: 16 MiB [OK]"
fi

# Check magic bytes at each partition offset.
# Dumped with od (coreutils, always present) rather than xxd, which on Debian is
# a separate package that is not installed by default: a missing xxd aborted the
# whole run here, after the image had already been built (issue #147).
check_magic() {
    local label="$1" offset="$2" expected="$3"
    local nbytes=$(( ${#expected} / 2 ))
    actual=$(dd if="$OUTPUT" bs=1 skip="$offset" count="$nbytes" 2>/dev/null \
             | od -An -tx1 -v | tr -d ' \n')
    if [ "$actual" = "$expected" ]; then
        log "  ${label} @ $(printf '0x%06X' $offset): $expected [OK]"
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

# CRC trailer at 0x1FFF0: the V3.1 loader refuses a corrupted transfer before
# writing anything; older loaders treat it as data.  Written last, after the
# magic checks, so the CRC covers the final image.
ff_crc=$(ffcrc_write "$OUTPUT") || { echo "Error: could not write the CRC trailer" >&2; rm -f "$OUTPUT"; exit 1; }
log "  CRC trailer @ 0x01FFF0: ${ff_crc} [OK]"

ff_md5=$(md5sum "$OUTPUT" | awk '{print $1}')
log ""
log "========================================="
log "  FULLFLASH READY"
log "========================================="
log ""
log "  $(ls -lh "$OUTPUT" | awk '{print $NF, $5}')"
log "  MD5: ${ff_md5}"
log ""

# In quiet mode, no summary line — the caller handles messaging
