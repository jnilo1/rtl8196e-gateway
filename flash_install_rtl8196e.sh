#!/bin/bash
# flash_install_rtl8196e.sh — Install custom firmware on Lidl Silvercrest Gateway
#
# Builds a fullflash.bin image (via build_fullflash.sh) and flashes it to the
# gateway via TFTP.
#
# Two modes of operation, chosen by one capability test — is `boothold`
# runnable over SSH:
#   - Upgrade: pass LINUX_IP — the script connects via SSH, saves user config,
#     and (if boothold is present) reboots straight into the bootloader. If
#     boothold is absent (Tuya / very old firmware), it guides you to serial.
#   - First flash: no argument — the gateway must already be in bootloader mode
#     (<RealTek> prompt via serial ESC).
#
# Interactive vs non-interactive:
#   By default the script is interactive: it prompts for backup, flash
#   confirmation and (on first flash) network configuration.
#   Pass -y (or CONFIRM=y) for non-interactive mode — all prompts are skipped.
#   This enables unattended remote upgrades over SSH.
#   Note: if auto-flash fails and falls back to manual FLW, a terminal (tty)
#   is still required for serial console guidance.
#
# The flash step is the same regardless of firmware version — no version parsing.
# On the boothold (auto) path no classification is needed at all: we got there
# by running `boothold` on a custom firmware, so the bootloader is a custom
# V2.x with auto-flash by construction — upload, then confirm the write.
# On the manual / first-flash path, auto-flash vs manual FLW is decided
# behaviourally, in two cheap observations:
#   - Pre-upload ICMP: Tuya / pre-ICMP stock bootloaders never answer ping and
#     have no auto-flash → go straight to guided FLW (no wait).
#   - Post-upload ICMP: a bootloader WITH auto-flash starts writing 16 MiB the
#     instant the upload lands and, being single-threaded, stops answering ping
#     for the duration; one WITHOUT keeps answering at its idle prompt. So "ping
#     was up and goes silent" = auto-flash in progress. "Stays up" = no
#     auto-flash → guided FLW. Decided in seconds, not minutes.
# The write is then confirmed on two independent channels (see
# confirm_autoflash): the bootloader's UDP:9999 OK/FAIL notification, and —
# when the gateway's post-flash address is known for certain — SSH coming
# back up at that address, which can only mean the new firmware booted.
#
# Prerequisites:
#   - Ethernet cable between host and gateway
#   - tftp-hpa client installed (sudo apt install tftp-hpa)
#   - Serial console (3.3V UART, 38400 8N1, line wrap ON) — needed to enter
#     bootloader mode (first flash / Tuya) and for older bootloaders that
#     require manual flash commands (the script will guide you)
#
# Usage: ./flash_install_rtl8196e.sh [-y] [LINUX_IP] [--help]
#
# Arguments:
#   LINUX_IP        Gateway IP when running Linux (for upgrade with config save)
#                   Omit for first-time flash (gateway must be in bootloader mode)
#
# Options:
#   -y, --yes       Non-interactive mode: skip all confirmation prompts
#   --boot-ip <IP>  Bootloader-mode / TFTP server IP. Overrides the BOOT_IP
#                   env var (precedence: flag > env > gateway.env > derived).
#   --force         Skip the board-mismatch safety check (upgrade path).
#
# Addresses. Nothing here is hardcoded to one subnet any more: BOOT_IP and the
# network settings offered at install time fall back to gateway.env, then to an
# address derived from THIS host's own LAN, then to the project's historic
# 192.168.1.x constants. See lib/gwconf.sh and gateway.env.example.
#
# Environment variables:
#   BOOT_IP      - Gateway IP in bootloader mode. On the boothold path it is
#                  handed to the bootloader (V2.7+) so the gateway comes up on
#                  this address in download mode. It must be on the same L2
#                  segment as this host — TFTP does not cross a router.
#                  The --boot-ip flag takes precedence when both are given.
#   SSH_TIMEOUT  - TCP probe timeout in seconds (default: 2)
#   SSH_PASSWORD - Root password for non-interactive auth (CI / no tty).
#                  When set, the first ssh call is fed via sshpass and the
#                  ControlMaster takes over for the rest. Requires sshpass
#                  (sudo apt install sshpass).
#   NET_MODE     - "static" or "dhcp" (skip network prompt)
#   IPADDR       - Static IP address for the gateway (default: see above)
#   NETMASK      - Netmask (default: this host's own netmask)
#   GATEWAY      - Default gateway (default: this host's own default route)
#   BOARD        - "lidl" (default) or "sengled-e39-g8c" (kernel image baked in)
#   KERNEL       - "6.18" (default) or "7.2" (kernel line baked in)
#   CONFIRM      - Set to "y" to skip confirmation prompts (same as -y)
#
# J. Nilo - March 2026

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Hardened SSH helpers — see lib/ssh.sh.
# shellcheck disable=SC1091
. "${SCRIPT_DIR}/lib/ssh.sh"
# Safe-retry TFTP upload helpers — see lib/flash_tftp.sh.
. "${SCRIPT_DIR}/lib/flash_tftp.sh"
# (board, kernel) validation — see lib/kernel_image.sh.
. "${SCRIPT_DIR}/lib/kernel_image.sh"
# Host-side gateway config — see lib/gwconf.sh.
# shellcheck disable=SC1091
. "${SCRIPT_DIR}/lib/gwconf.sh"
# Firmware-version parser — preserves prerelease/build suffixes (#156).
# shellcheck disable=SC1091
. "${SCRIPT_DIR}/lib/firmware_version.sh"
# PATH normalisation — the prerequisite check below probes mkfs.jffs2, which
# some distributions keep out of a non-root PATH. See lib/hostpath.sh.
# shellcheck disable=SC1091
. "${SCRIPT_DIR}/lib/hostpath.sh"
LINUX_IP=""
FW_VERSION=""
FW_VERSION_MAJOR=""
# Default entry mode. Overridden to "auto" only when a running Linux exposes
# boothold over SSH; stays "manual" for the already-at-bootloader-prompt path
# (no LINUX_IP), where LINUX_RUNNING is empty and the block below is skipped.
ENTRY="manual"
# Set to 1 once the running gateway's user config has been saved for
# re-injection into the new image (upgrade path). Drives the flash-warning
# wording so we don't claim "all data will be replaced" when it is preserved.
CONFIG_SAVED=""
# BOOT_IP precedence: --boot-ip flag > BOOT_IP env > gateway.env > a default that
# depends on how we reach the bootloader. This script has both paths, and they
# differ in a way that matters:
#
#   - first flash / manual entry: the gateway is ALREADY at a bootloader prompt,
#     reached by a cold boot and a serial ESC. Nothing can move it, so it sits at
#     its compiled address whatever LAN this host is on. Deriving here would look
#     for it in the wrong subnet.
#   - upgrade via boothold: we hand the bootloader its address, so the host-LAN
#     derivation applies. Done further down, once the entry mode is known.
#
# BOOT_IP_STATED records whether the user named an address, in which case neither
# default applies. The flag is captured into BOOT_IP_FLAG and applied after the
# parsing loop.
BOOT_IP_STATED=0
if [ -n "${BOOT_IP:-}" ]; then
    BOOT_IP_STATED=1
else
    gwconf_resolve_boot_ip
    if gwconf_boot_ip_is_derived; then
        BOOT_IP="$GWCONF_BOOTLOADER_COLD_IP"     # safe for the first-flash path
    else
        BOOT_IP="$GWCONF_BOOT"                   # gateway.env named one
        BOOT_IP_STATED=1
    fi
fi
BOOT_IP_FLAG=""
SSH_TIMEOUT="${SSH_TIMEOUT:-2}"
# BOARD/KERNEL select the pre-built kernel image baked into the fullflash
# (default lidl / 6.18 = the historical image). Exported so build_fullflash.sh
# (child) resolves the same pair.
BOARD="${BOARD:-lidl}"
KERNEL="${KERNEL:-6.18}"
export BOARD KERNEL
# --force overrides the board-mismatch guard (upgrade path).
FORCE=0

# --- argument parsing --------------------------------------------------------

while [ $# -gt 0 ]; do
    case "$1" in
        -y|--yes) CONFIRM="y" ;;
        --help|-h)
            echo "Usage: $0 [-y] [--boot-ip <IP|host>] [LINUX_IP]"
            echo ""
            echo "Installs custom firmware on an RTL8196E gateway (see BOARD below)."
            echo ""
            echo "Arguments:"
            echo "  LINUX_IP         Gateway IP when running Linux (upgrade with config save)"
            echo "                   Omit for first-time flash (gateway must be in bootloader)"
            echo ""
            echo "Options:"
            echo "  -y, --yes        Non-interactive mode (skip all prompts)"
            echo "  --boot-ip <IP|host>  Bootloader-mode / TFTP server IP (overrides BOOT_IP"
            echo "                   env; default: ${BOOT_IP}). A hostname is resolved host-side."
            echo "  --force          Skip the board-mismatch safety check (upgrade path)."
            echo ""
            echo "Environment: BOOT_IP (default: ${BOOT_IP}), BOARD, KERNEL,"
            echo "  SSH_TIMEOUT, SSH_PASSWORD (sshpass), NET_MODE,"
            echo "  CONFIRM, IPADDR, NETMASK, GATEWAY (network default gateway)"
            exit 0
            ;;
        --boot-ip)
            shift
            [ $# -gt 0 ] || { echo "Error: --boot-ip requires an argument." >&2; exit 1; }
            BOOT_IP_FLAG="$1"
            ;;
        --boot-ip=*) BOOT_IP_FLAG="${1#*=}" ;;
        --force) FORCE=1 ;;
        --*) echo "Unknown option: $1. Use --help for usage."; exit 1 ;;
        *)
            if [ -n "$LINUX_IP" ]; then
                echo "Error: unexpected argument '$1' (LINUX_IP already set to '$LINUX_IP')." >&2
                exit 1
            fi
            LINUX_IP="$1"
            ;;
    esac
    shift
done

# Apply --boot-ip override (flag > env > default), resolving a hostname if one
# was given (the on-device boothold/bootloader need a literal IPv4 — resolve
# host-side). A dotted-quad passes through unchanged.
[ -n "$BOOT_IP_FLAG" ] && { BOOT_IP="$BOOT_IP_FLAG"; BOOT_IP_STATED=1; }
if BOOT_IP_RESOLVED="$(resolve_ipv4 "$BOOT_IP")"; then
    [ "$BOOT_IP_RESOLVED" != "$BOOT_IP" ] && echo "Resolved BOOT_IP '$BOOT_IP' -> $BOOT_IP_RESOLVED"
    BOOT_IP="$BOOT_IP_RESOLVED"
else
    echo "Error: invalid BOOT_IP '$BOOT_IP' (not a dotted-quad IPv4, and could" >&2
    echo "not be resolved as a hostname)." >&2
    exit 1
fi

# --- prerequisites -----------------------------------------------------------
# Fail fast with a single actionable message before building or touching the
# gateway. Users who didn't go through 1-Build-Environment/install_deps.sh
# would otherwise hit silent failures deep in the build (issue #84).

missing_pkgs=()
check_cmd() {
    # $1 = command to probe, $2 = apt package to install if missing
    command -v "$1" >/dev/null 2>&1 || missing_pkgs+=("$2")
}

check_cmd fakeroot     fakeroot
check_cmd gcc          gcc
check_cmd mkfs.jffs2   mtd-utils
check_cmd mksquashfs   squashfs-tools

# sshpass is only required when SSH_PASSWORD is set (non-interactive
# password auth — see lib/ssh.sh:ssh_prime_with_password).
[ -n "${SSH_PASSWORD:-}" ] && check_cmd sshpass sshpass

# tftp-hpa: the BSD tftp client is also called "tftp" but lacks the -c flag.
# Capture --help output first — tftp-hpa exits 64 on --help, which under
# `set -o pipefail` would make the piped grep inherit that non-zero code
# even on a successful match.
tftp_help="$(tftp --help 2>&1 || true)"
if ! command -v tftp >/dev/null 2>&1 \
   || ! echo "$tftp_help" | grep -q -- '-c'; then
    missing_pkgs+=("tftp-hpa")
fi

if [ "${#missing_pkgs[@]}" -gt 0 ]; then
    echo "Error: missing build/flash prerequisites: ${missing_pkgs[*]}" >&2
    echo "Install them with:" >&2
    echo "  sudo apt install ${missing_pkgs[*]}" >&2
    exit 1
fi

# Fail fast on a bad BOARD/KERNEL, or a missing per-board pre-built bootloader,
# before touching the gateway or building (build_fullflash.sh re-resolves both).
kernel_image_validate "$BOARD" "$KERNEL" || exit 1
resolve_boot_image "$BOARD" >/dev/null || exit 1

# Resolve IFACE for BOOT_IP and require L2 reachability — the bootloader's TFTP
# server only answers on the same L2 segment. Sets IFACE on success; exits with
# an actionable hint when the host has no interface in the bootloader's subnet.
require_boot_l2() {
    IFACE="$(ip route get "$BOOT_IP" 2>/dev/null \
        | awk '{for(i=1;i<=NF;i++) if($i=="dev"){print $(i+1); exit}}' || true)"
    if [ -z "${IFACE:-}" ]; then
        echo "Error: cannot determine outgoing interface to ${BOOT_IP}." >&2
        exit 1
    fi
    if ip route get "$BOOT_IP" 2>/dev/null | grep -qE '\svia\s'; then
        echo "Error: ${BOOT_IP} is reached via a gateway (routed). The bootloader's" >&2
        echo "TFTP server only answers on the same L2 segment." >&2
        echo "" >&2
        echo "Add a secondary address on the interface that faces the gateway, e.g.:" >&2
        echo "    sudo ip addr add 192.168.1.10/24 dev <iface>" >&2
        echo "" >&2
        echo "Then re-run this script. Remove the address afterwards with 'ip addr del'." >&2
        exit 1
    fi
}

# probe_tftp_wrq <ip> and tftp_put_safe come from lib/flash_tftp.sh (sourced
# above) — a 1-byte WRQ probe that ACKs only when the bootloader's TFTP server is
# idle, and the safe re-probe-gated upload retry built on it.

# Build fullflash.bin, sanity-check it, and ask the final confirmation.
# On the upgrade (auto) path this runs while Linux is still up — BEFORE boothold —
# so the slow image build does not happen with the gateway stranded in the
# bootloader, and an abort (or a build failure) leaves Linux untouched. On a
# first flash / manual entry it runs at the convergence point (gateway already in
# the bootloader), where build_fullflash.sh prompts interactively for networking.
# Sets GW_HINT_IP, FULLFLASH and the IMAGE_READY guard so the convergence point
# does not build a second time.
build_image_and_confirm() {
    # In the upgrade path SKELETON_DIR is already exported (with saved config
    # from the running gateway). On a first flash the parent has no SKEL_WORK
    # yet, but build_fullflash.sh will prompt for networking and write into
    # whatever SKELETON_DIR points to. We pre-create one here so the parent
    # can read back the chosen IPADDR for the post-install hint, instead of
    # losing it when build_fullflash's own mktemp dir is reaped.
    if [ -z "${SKELETON_DIR:-}" ]; then
        USERDATA_SKEL="${SCRIPT_DIR}/3-Main-SoC-Realtek-RTL8196E/34-Userdata/skeleton"
        SKEL_WORK=$(mktemp -d)
        cp -a "$USERDATA_SKEL/." "$SKEL_WORK/"
        trap 'rm -rf "$SKEL_WORK"; ssh_cleanup_multiplex' EXIT
        export SKELETON_DIR="$SKEL_WORK"
    fi

    # Called with -q (quiet): only config → lines, errors, and a summary are
    # shown. Run build_fullflash.sh without -q for full verbose output.
    "${SCRIPT_DIR}/build_fullflash.sh" -q

    # Read back the IP the user picked (or kept) so the post-install hints
    # show the right address. Fall back to LINUX_IP (upgrade path) or the
    # default for first-time installs that chose DHCP / left the default.
    if [ -z "${IPADDR:-}" ] && [ -f "${SKELETON_DIR}/etc/eth0.conf" ]; then
        IPADDR=$(gwconf_read_key "${SKELETON_DIR}/etc/eth0.conf" IPADDR || true)
    fi
    # Last resort is whatever this host knows about the gateway (gateway.env, a
    # previous install, the device's hostname) rather than a fixed address.
    GW_HINT_IP="${LINUX_IP:-${IPADDR:-$(gwconf_gateway_addr)}}"

    FULLFLASH="${SCRIPT_DIR}/fullflash.bin"
    if [ ! -f "$FULLFLASH" ]; then
        echo "Error: fullflash.bin not found after build." >&2
        exit 1
    fi

    FLASH_SIZE=$((16 * 1024 * 1024))
    ff_size=$(stat -c%s "$FULLFLASH")
    if [ "$ff_size" -ne "$FLASH_SIZE" ]; then
        echo "Error: fullflash.bin is ${ff_size} bytes (expected ${FLASH_SIZE})." >&2
        exit 1
    fi

    # --- confirm: last chance to abort. Skipped in non-interactive mode. ------
    echo ""
    echo "WARNING: This will overwrite the ENTIRE flash chip (16 MiB)."
    if [ "$CONFIG_SAVED" = "1" ]; then
        echo "Your saved config (network, password, SSH keys, radio, Thread"
        echo "credentials) will be re-injected; everything else is replaced."
    else
        echo "All data on the gateway will be replaced."
    fi
    echo ""
    echo "  Image:  fullflash.bin ($(md5sum "$FULLFLASH" | awk '{print $1}'))"
    echo "  Target: ${BOOT_IP}"
    echo ""

    if [ "${CONFIRM:-}" != "y" ]; then
        read -r -p "Proceed? [y/N] " confirm
        if [[ ! "$confirm" =~ ^[yY]$ ]]; then echo "Aborted."; exit 0; fi
    fi

    IMAGE_READY=1
}

# Board-mismatch guard for the upgrade path. What identifies a board — the
# device-tree model first, the DRAM bring-up the bootloader performed when the
# model is too old to name one — lives in board_guard_check (lib/kernel_image.sh);
# read the comment there before changing what counts as proof. A full flash
# bundles a board-specific kernel AND bootloader, and a mismatched bootloader
# bricks the gateway, so a board that cannot be confirmed at all is refused
# here rather than flashed hopefully. --force is the way past it.
# Expects FI_SSH_OPTS / FI_SSH_TARGET in scope (port-22 path).
fi_check_board_match() {
    board_guard_check "$BOARD" "$FI_SSH_TARGET" "${FI_SSH_OPTS[@]}"
    case $? in
        0) return 0 ;;
        1) return 1 ;;
        *)
            echo "  Nothing was touched. If you know which board this is, re-run with" >&2
            echo "  the matching BOARD= and --force." >&2
            return 1
            ;;
    esac
}


# --- detect gateway state (early — fail fast before building) ----------------
# If LINUX_IP is provided, probe SSH to determine firmware type and save config.
# Otherwise, check if bootloader is already reachable at BOOT_IP.

echo ""
echo "========================================="
echo "  FIRMWARE INSTALLATION"
echo "========================================="
echo ""

# Detect gateway state based on whether LINUX_IP was provided.
# BOOT_IP L2 reachability is enforced lazily — at boothold time on the upgrade
# path, immediately on the bootloader path — so backup-via-SSH still works
# from a routed network where the host has no 192.168.1.0/24 interface yet.
LINUX_RUNNING=""
if [ -n "$LINUX_IP" ]; then
    echo "Probing SSH on ${LINUX_IP}..."
    if timeout "$SSH_TIMEOUT" bash -c "echo >/dev/tcp/$LINUX_IP/22" 2>/dev/null; then
        LINUX_RUNNING="${LINUX_IP}:22"
    elif timeout "$SSH_TIMEOUT" bash -c "echo >/dev/tcp/$LINUX_IP/2333" 2>/dev/null; then
        LINUX_RUNNING="${LINUX_IP}:2333"
    else
        echo "Error: cannot reach gateway at ${LINUX_IP} (no SSH on port 22 or 2333)." >&2
        echo "Check the Ethernet cable, or if the gateway is already in bootloader mode" >&2
        echo "re-run without argument:  $0" >&2
        exit 1
    fi
fi

if [ -n "$LINUX_RUNNING" ]; then
    fw_host="${LINUX_RUNNING%%:*}"
    fw_port="${LINUX_RUNNING##*:}"
    echo "Linux detected at ${fw_host}:${fw_port}."

    if [ "$fw_port" = "2333" ]; then
        # Port 2333 is exclusively Tuya/Lidl stock — no boothold, no SSH needed.
        ENTRY="manual"
    else
        # Port 22 — could be custom or Tuya. SSH in and test the one capability
        # that decides automated entry: can we run `boothold`?
        # StrictHostKeyChecking=no + /dev/null known_hosts is intentional:
        # this workflow installs custom firmware over Tuya stock, so the
        # gateway's host key changes legitimately mid-flow. ControlMaster
        # comes from lib/ssh.sh — first call prompts for password/passphrase
        # at most once, the rest of the back-to-back commands ride the
        # same channel.
        FI_SSH_OPTS=(
            "${SSH_HARDEN_OPTS[@]}"
            -o StrictHostKeyChecking=no
            -o UserKnownHostsFile=/dev/null
            -p "$fw_port"
        )
        FI_SSH_TARGET="root@${fw_host}"

        # Open the ControlMaster up-front using SSH_PASSWORD if provided
        # (no-op when SSH_PASSWORD is unset — ssh's tty prompt handles it).
        if ! ssh_prime_with_password "${FI_SSH_OPTS[@]}" "$FI_SSH_TARGET"; then
            exit 1
        fi

        # Verify SSH access before proceeding
        if ! ssh_retry "${FI_SSH_OPTS[@]}" "$FI_SSH_TARGET" "true" 2>/dev/null; then
            echo "Error: SSH authentication failed." >&2
            exit 1
        fi

        # Board-mismatch guard: a full flash carries a board-specific kernel and
        # bootloader; refuse a different board than the gateway reports (--force overrides).
        if [ "$FORCE" != "1" ]; then
            fi_check_board_match || exit 1
        fi

        # boothold present = custom firmware we can warm-reboot into the
        # bootloader. Absent = Tuya, or custom too old to have boothold
        # (pre-v1.1.0) — either way, automated entry is impossible.
        if ssh_retry "${FI_SSH_OPTS[@]}" "$FI_SSH_TARGET" "command -v boothold" >/dev/null 2>&1; then
            ENTRY="auto"
        else
            ENTRY="manual"
        fi
    fi
    echo "Entry mode: ${ENTRY}"

    # Read firmware version early — used ONLY for the v2→v3 radio.conf pre-seed
    # decision below (no longer gates the flash). Requires the SSH channel, so
    # only meaningful on the automated (boothold) path.
    if [ "$ENTRY" = "auto" ]; then
        fw_ver_line=$(ssh_retry "${FI_SSH_OPTS[@]}" "$FI_SSH_TARGET" "head -1 /userdata/etc/version" 2>/dev/null || true)
        if firmware_version_parse "$fw_ver_line"; then
            FW_VERSION="$FWPARSE_VERSION"
            FW_VERSION_MAJOR="$FWPARSE_MAJOR"
            echo "Firmware version: v${FW_VERSION}"
        fi
    fi

    # --- propose backup (while Linux is still running) -----------------------
    # Skipped in non-interactive mode (-y / CONFIRM=y)
    if [ "${CONFIRM:-}" != "y" ]; then
        echo ""
        echo "It is recommended to back up the flash before installing."
        read -r -p "Run backup_gateway.sh now? [y/N] " do_backup
        if [[ "$do_backup" =~ ^[yY]$ ]]; then
            "${SCRIPT_DIR}/backup_gateway.sh" --linux-ip "$fw_host" --boot-ip "$BOOT_IP"
            echo ""
        fi
    fi

    if [ "$ENTRY" = "auto" ]; then
        # Save user config before reboot (will be injected into userdata)
        # Only user-configurable files — not init scripts or system files
        # Work on a temporary copy of the skeleton — never modify the original
        USERDATA_SKEL="${SCRIPT_DIR}/3-Main-SoC-Realtek-RTL8196E/34-Userdata/skeleton"
        SKEL_WORK=$(mktemp -d)
        cp -a "$USERDATA_SKEL/." "$SKEL_WORK/"
        trap 'rm -rf "$SKEL_WORK"; ssh_cleanup_multiplex' EXIT
        export SKELETON_DIR="$SKEL_WORK"

        SAVE_TAR=$(mktemp)
        # User-configurable files; other user-added paths under /userdata are
        # carried by preserve_user_additions below.
        SAVE_FILES="etc/eth0.conf etc/mac_address etc/radio.conf etc/leds.conf etc/passwd etc/TZ etc/hostname etc/dropbear ssh thread"
        ssh_retry "${FI_SSH_OPTS[@]}" "$FI_SSH_TARGET" \
            "tar cf - -C /userdata $SAVE_FILES 2>/dev/null" > "$SAVE_TAR" 2>/dev/null || true
        if [ -s "$SAVE_TAR" ]; then
            tar xf "$SAVE_TAR" -C "$SKEL_WORK" 2>/dev/null || true
            echo "Gateway config saved."
            CONFIG_SAVED=1
            export NET_MODE="skip"
            # Remember where this box was and what it calls itself, so the
            # host-side tools can find it again without an argument. The
            # hostname comes out of the config we just pulled — no extra SSH.
            gwconf_record_seen "$fw_host" \
                "$(head -1 "${SKEL_WORK}/etc/hostname" 2>/dev/null || true)"
        fi
        rm -f "$SAVE_TAR"

        # Also carry any user-added paths under /userdata (custom programs,
        # scripts, whole new directories) — anything not shipped in the skeleton
        # and not already re-injected above — into the new image.
        preserve_user_additions "$SKEL_WORK" "$FI_SSH_TARGET" "${FI_SSH_OPTS[@]}"

        # v2 → v3 migration: pre-v3.0 firmware shipped serialgateway and
        # had no /userdata/etc/radio.conf — the EFR32-side baud was hard-
        # coded to 115200 (NCP-UART-HW @ 115200 was the v2.x default). The
        # v3.x in-kernel UART bridge defaults to 460800 when radio.conf is
        # missing, which leaves the host bridge mismatched against the
        # still-115200 chip until either the chip is reflashed or
        # radio.conf is created. Pre-seed the full v3.x radio.conf
        # describing the known v2.x state (NCP @ 115200) so the new
        # userdata boots into a working state AND a future reader can
        # tell what's on the chip without probing.
        if [ -n "${FW_VERSION_MAJOR:-}" ] && [ "$FW_VERSION_MAJOR" -lt 3 ] \
           && [ ! -f "${SKEL_WORK}/etc/radio.conf" ]; then
            echo "Pre-seeding radio.conf for v${FW_VERSION} → v3.x migration (FIRMWARE=ncp @ 115200)."
            echo "  ↑ Default v2.x assumption. Cancel now (Ctrl-C) and run on the gateway:"
            echo "      cat > /userdata/etc/radio.conf  (with FIRMWARE=otrcp/rcp/... if non-default),"
            echo "    then re-run this script — your radio.conf will be preserved."
            echo "  See 3-Main-SoC-Realtek-RTL8196E/35-Migration/README.md for the recipes."
            cat > "${SKEL_WORK}/etc/radio.conf" <<EOF
FIRMWARE=ncp
FIRMWARE_BAUD=115200
EOF
        fi

        # From here the bootloader's address is OURS to choose — boothold hands
        # it over — so the host-LAN derivation applies, unlike the first-flash
        # path above where the bootloader is already up at its compiled default.
        # Only when the user named no address.
        if [ "$BOOT_IP_STATED" = 0 ]; then
            gwconf_resolve_boot_ip
            if gwconf_boot_ip_is_derived && [ "$GWCONF_BOOT" != "$BOOT_IP" ]; then
                BOOT_IP="$GWCONF_BOOT"
                echo "Bootloader address: ${BOOT_IP}$(gwconf_source_note "$GWCONF_BOOT_SOURCE")"
            fi
        fi

        # Confirm the host can TFTP to the bootloader before tipping the
        # gateway into bootloader mode — failing after boothold leaves the
        # gateway stranded at ${BOOT_IP} with the user scrambling to fix
        # their network mid-flow.
        require_boot_l2

        # Build the image (and take the final confirmation) NOW, while Linux is
        # still up. The saved config is already in SKELETON_DIR, so the image
        # has everything it needs — no bootloader state is required. Doing it
        # here means the slow build no longer runs with the gateway stranded in
        # the bootloader; only the TFTP upload + flash write block the gateway.
        # An abort at the confirmation (or a build failure) leaves Linux intact.
        build_image_and_confirm

        # Pass BOOT_IP to boothold (V2.7+): the bootloader comes up on that
        # address in download mode, so a non-default BOOT_IP needs no serial
        # IPCONFIG. Older boothold ignores the argument (stays at 192.168.1.6).
        #
        # Arm first, reboot second — two commands, so the arming has an exit
        # status of its own and its output reaches the operator. boothold
        # refuses to write when the running kernel declares no boothold page,
        # and it verifies its own write by reading it back; a refusal means the
        # gateway is still up on its current firmware and the run must stop
        # here, instead of rebooting anyway and blaming the bootloader a minute
        # later. The address boothold prints is the one the bootloader has to
        # be built to read (it is per board) — worth seeing when a warm reboot
        # comes back into Linux.
        echo "Arming boot hold..."
        BOOTHOLD_RC=0
        BOOTHOLD_OUT="$(ssh_retry "${FI_SSH_OPTS[@]}" "$FI_SSH_TARGET" \
                        "boothold \"$BOOT_IP\"" 2>&1)" || BOOTHOLD_RC=$?
        [ -n "$BOOTHOLD_OUT" ] && printf '%s\n' "$BOOTHOLD_OUT" | sed 's/^/  /'
        case "$BOOTHOLD_RC" in
            0)
                echo "Rebooting into the bootloader..."
                # Plain ssh, not ssh_retry: the connection dropping is
                # the expected outcome here, and a retry would only run
                # reboot again on a gateway already going down.
                ssh "${FI_SSH_OPTS[@]}" "$FI_SSH_TARGET" "reboot" >/dev/null 2>&1 || true
                ;;
            255|43)
                # The connection died instead of returning a status. That is
                # also what a successful arm looks like on firmware older than
                # v3.2.0, where boothold was a shell script that rebooted the
                # gateway itself — indistinguishable from a dropped link from
                # here. Say so and let the wait phases below decide: they probe
                # what the flash actually needs, a TFTP server that answers.
                echo "SSH dropped while arming — pre-v3.2.0 boothold reboots on its own."
                ;;
            *)
                echo "" >&2
                echo "Error: boothold refused to arm the bootloader (exit ${BOOTHOLD_RC})." >&2
                echo "The gateway is still running its current firmware and nothing was" >&2
                echo "written — no reboot was sent." >&2
                echo "" >&2
                echo "Enter the bootloader over the serial console instead (ESC at power-on)," >&2
                echo "then re-run without an IP:  $0" >&2
                exit 1
                ;;
        esac
        # Close ControlMaster socket — gateway is rebooting, no point waiting
        # for ControlPersist to expire on a connection that's already dead.
        ssh_cleanup_multiplex
    else
        echo ""
        echo "No boothold on this firmware (Tuya stock, or custom older than v1.1.0)."
        echo "Cannot enter the bootloader automatically. To enter bootloader mode:"
        echo "  - Connect serial console (3.3V UART, 38400 8N1, line wrap ON)"
        echo "  - Power cycle the gateway"
        echo "  - Press ESC repeatedly during boot to get the <RealTek> prompt"
        echo "  - Then re-run:  $0"
        echo ""
        exit 1
    fi

    # --- wait for bootloader after boothold + reboot -------------------------
    # Two-phase wait to avoid ARP false positives (Linux responds to ARP for
    # BOOT_IP via ARP flux while still shutting down).

    # Phase 1: wait for SSH to go down (Linux is shutting down)
    echo "Waiting for shutdown..."
    tries=0
    while [ $tries -lt 15 ]; do
        if ! timeout 1 bash -c "echo >/dev/tcp/$fw_host/$fw_port" 2>/dev/null; then
            break
        fi
        sleep 1
        tries=$((tries + 1))
    done

    # Phase 2: wait for the bootloader. ARP alone is not enough: the dying
    # Linux keeps answering ARP for a few seconds after SSH goes down (ARP
    # flux), and a proxy-ARP router can answer for an address that is not up
    # at all. Both produced false "Bootloader detected" (discussion #115):
    # the ICMP classification then ran against a rebooting box and wrongly
    # picked the manual path, or the 16 MiB upload ran against nothing and
    # sat in a 5-minute timeout. Require what the flash actually needs — a
    # TFTP server that ACKs a WRQ.
    echo "Waiting for bootloader at ${BOOT_IP}..."
    tries=0
    BOOTLOADER_UP=""
    while [ $tries -lt 45 ]; do
        ip neigh del "$BOOT_IP" dev "$IFACE" 2>/dev/null || true
        bash -c "echo -n X >/dev/udp/$BOOT_IP/69" 2>/dev/null || true
        sleep 1
        nei="$(ip neigh show "$BOOT_IP" dev "$IFACE" 2>/dev/null || true)"
        if echo "$nei" | grep -Eqi 'lladdr [0-9a-f]{2}(:[0-9a-f]{2}){5}' \
           && probe_tftp_wrq "$BOOT_IP"; then
            BOOTLOADER_UP=1
            break
        fi
        tries=$((tries + 1))
    done

    if [ -z "$BOOTLOADER_UP" ]; then
        echo "Error: bootloader not detected after boothold (no TFTP server at ${BOOT_IP})." >&2
        echo "" >&2
        # Distinguish the two ways this fails. If SSH answers again, the gateway
        # rebooted straight back into Linux: the flag was written and verified
        # (boothold read it back), so the bootloader on the flash simply did not
        # look where the running kernel wrote. That page is a per-board contract
        # between the kernel DTS and the bootloader's board.h, and a mismatched
        # pair — a bootloader built for another board — fails exactly this way,
        # with nothing printed on either side.
        if timeout 2 bash -c "echo >/dev/tcp/$fw_host/$fw_port" 2>/dev/null; then
            echo "The gateway is answering at ${fw_host}:${fw_port} again — it rebooted" >&2
            echo "straight back into Linux, so the bootloader never saw the flag that" >&2
            echo "boothold wrote and verified at the address printed above." >&2
            echo "" >&2
            echo "That address comes from the running kernel's device tree; the bootloader" >&2
            echo "reads a constant compiled into it, one per board (DRAM top - 0x2000)." >&2
            echo "They must match. Check the bootloader banner on the serial console: its" >&2
            echo "RAM size must be the one your board really has, and BOARD= here must be" >&2
            echo "the board it was built for." >&2
        else
            echo "Note: pre-V2.7 bootloaders ignore the boothold IP handoff and come up at" >&2
            echo "the default 192.168.1.6 — if you used --boot-ip, retry from a host on" >&2
            echo "that subnet without it." >&2
        fi
        echo "" >&2
        echo "Nothing was written: a power cycle returns the gateway to its current" >&2
        echo "firmware." >&2
        exit 1
    fi
else
    # No LINUX_IP given — check if bootloader is reachable via ARP
    echo "Checking for bootloader at ${BOOT_IP}..."
    require_boot_l2
    ip neigh del "$BOOT_IP" dev "$IFACE" 2>/dev/null || true
    bash -c "echo -n X >/dev/udp/$BOOT_IP/69" 2>/dev/null || true
    sleep 0.3

    nei="$(ip neigh show "$BOOT_IP" dev "$IFACE" 2>/dev/null || true)"
    if ! echo "$nei" | grep -Eqi 'lladdr [0-9a-f]{2}(:[0-9a-f]{2}){5}'; then
        echo "Gateway not detected at ${BOOT_IP}."
        echo ""
        echo "For first-time flash:"
        echo "  - Connect serial console (3.3V UART, 38400 8N1, line wrap ON)"
        echo "  - Power cycle the gateway"
        echo "  - Press ESC repeatedly during boot to get the <RealTek> prompt"
        echo "  - Then re-run:  $0"
        echo ""
        echo "For upgrade (with config save):"
        echo "  - Run:  $0 <LINUX_IP>   (e.g. $0 $(gwconf_gateway_addr))"
        echo ""
        exit 1
    fi

    # ARP resolved — but is it really bootloader? Probe TFTP to confirm.
    if ! probe_tftp_wrq "$BOOT_IP"; then
        echo "Device at ${BOOT_IP} is not in bootloader mode (no TFTP server)."
        echo "If the gateway is running Linux, run:  $0 <LINUX_IP>"
        exit 1
    fi

    # ARP resolved + TFTP responding = bootloader.
    echo ""
    echo "Bootloader detected. No config files will be imported."
    echo "You will be prompted for the gateway network settings."
    if [ "${CONFIRM:-}" != "y" ]; then
        read -r -p "Proceed? [y/N] " r
        if [[ ! "$r" =~ ^[yY]$ ]]; then
            echo "Aborted."
            exit 0
        fi
    fi
fi

echo "Bootloader detected at ${BOOT_IP}."

# If we reached bootloader without going through Linux (no backup opportunity),
# warn the user.
if [ -z "$LINUX_RUNNING" ] && [ "${CONFIRM:-}" != "y" ]; then
    echo ""
    echo "WARNING: No backup was made. To back up first, boot the gateway"
    echo "into Linux and run:  ./backup_gateway.sh"
    echo ""
    read -r -p "Continue without backup? [y/N] " do_continue
    if [[ ! "$do_continue" =~ ^[yY]$ ]]; then
        echo "Aborted."
        exit 0
    fi
fi

# --- build fullflash.bin + final confirmation --------------------------------
# On the upgrade (auto) path this already ran before boothold, while Linux was
# still up — IMAGE_READY is set, so we skip it here. On a first flash / manual
# entry the gateway is already in the bootloader and nothing was built yet:
# build now (build_fullflash.sh prompts interactively for networking).
[ "${IMAGE_READY:-}" = "1" ] || build_image_and_confirm

# --- flash --------------------------------------------------------------------
# One shared routine, no firmware-version logic. We always upload the image (both
# auto-flash and manual FLW need it in RAM), then decide behaviourally — never by
# version, and independent of how we entered (ENTRY):
#   - pre-upload ICMP silent  → Tuya / pre-v2 stock, no auto-flash → guided FLW.
#   - pre-upload ICMP up, then goes silent post-upload → auto-flash is writing the
#     16 MiB (single-threaded, can't answer ICMP) → wait for the UDP:9999 OK.
#   - pre-upload ICMP up, stays up post-upload → custom bootloader without
#     auto-flash (e.g. old v1.x) → guided FLW. Decided in seconds, no dead wait.

# Manual FLW guidance — the uploaded image is already in RAM at 0x80500000; the
# user finishes on the serial console. Interactive by design: show the FLW step,
# WAIT for the user to confirm it completed, then propose the reboot. On "no" we
# exit non-zero so the caller never prints "INSTALLATION COMPLETE" for a flash
# that did not happen. Requires an interactive terminal.
manual_flw_guidance() {
    if [ ! -t 0 ]; then
        echo "Error: manual flash requires an interactive terminal (serial console guidance)." >&2
        exit 1
    fi
    echo ""
    echo "NOTE: a custom (V2.x) bootloader may auto-flash the uploaded image on its"
    echo "own even when this script could not detect it. If the gateway reboots and"
    echo "comes back on SSH within ~2 min (ssh root@${GW_HINT_IP:-<gateway>}), the"
    echo "flash already succeeded — you are done and can ignore the steps below."
    echo "The steps below are only for stock/older bootloaders that need a manual FLW."
    echo ""
    echo "The image is in the gateway's RAM at 0x80500000. On the serial console"
    echo "(38400 8N1, line wrap ON), type:"
    echo ""
    echo "    FLW 0 80500000 01000000"
    echo ""
    echo "Answer (Y)es when prompted, then wait ~2 min until the <RealTek> prompt returns."
    echo ""
    read -r -p "Has the flash completed (FLW back at the <RealTek> prompt)? [y/N] " flw_done
    if [[ ! "$flw_done" =~ ^[yY]$ ]]; then
        echo ""
        echo "Manual FLW not confirmed. Before doing anything else, check whether the"
        echo "gateway auto-flashed on its own (custom V2.x bootloaders often do, even"
        echo "when this script could not detect it):"
        echo ""
        echo "    ping ${GW_HINT_IP:-<gateway>}      # wait up to ~2 min for the reboot"
        echo "    ssh root@${GW_HINT_IP:-<gateway>}"
        echo ""
        echo "If it answers, the flash already succeeded — you are done."
        echo "If it stays unreachable, nothing was written: the image is still in RAM"
        echo "at 0x80500000 — run the FLW above and reboot manually (J BFC00000), or"
        echo "re-run this script to upload again."
        exit 1
    fi
    echo ""
    echo "Reboot into the new firmware — on the serial console type:"
    echo ""
    echo "    J BFC00000"
    echo ""
    echo "(or do a hard reset / power cycle)."
}

# Confirm the auto-flash write on two independent channels, echoing "OK",
# "SSH" (gateway back up on SSH — implies the write succeeded), "FAIL", or ""
# on timeout:
#   - UDP:9999 — the bootloader's OK/FAIL notification, sent to the TFTP
#     client when the write ends. Instant, but easily lost: host firewalls
#     drop unsolicited inbound UDP, nc may be absent, and some netcat
#     variants reject `-l -p`. Never the only channel (discussion #115:
#     a successful flash was reported as "No auto-flash confirmation").
#   - SSH return — after writing, the bootloader reboots the box into the
#     NEW firmware. Nothing answers at the gateway's address during the
#     write, so SSH coming up there can only mean the flash succeeded.
#     Only polled when the post-flash address is known for certain
#     (SSH_POLL_IP, see below) — never against a guessed default.
# The write takes ~2 min and the reboot to SSH ~40 s more, hence the 270 s
# ceiling; both channels break out the moment they conclude.
confirm_autoflash() {
    local deadline result="" notify_file="" nc_pid=""
    deadline=$(( $(date +%s) + 270 ))
    if command -v nc >/dev/null 2>&1; then
        notify_file=$(mktemp)
        (timeout 270 nc -u -l -p 9999 > "$notify_file" 2>/dev/null) &
        nc_pid=$!
    fi
    while [ "$(date +%s)" -lt "$deadline" ]; do
        if [ -n "$notify_file" ] && [ -s "$notify_file" ]; then
            result="$(tr -d '\0' < "$notify_file" 2>/dev/null || true)"
            break
        fi
        if [ -n "$SSH_POLL_IP" ] \
           && timeout 2 bash -c "echo >/dev/tcp/$SSH_POLL_IP/22" 2>/dev/null; then
            result="SSH"
            break
        fi
        sleep 2
    done
    if [ -n "$nc_pid" ]; then
        kill "$nc_pid" 2>/dev/null || true
        wait "$nc_pid" 2>/dev/null || true
    fi
    [ -n "$notify_file" ] && rm -f "$notify_file"
    echo "$result"
}

# Behavioural auto-flash probe (call right after the upload). An auto-flash
# bootloader is now busy writing 16 MiB to SPI NOR and stops answering ICMP; one
# without auto-flash is idle at its prompt and keeps answering. Poll ICMP for a
# short window: two consecutive misses = it went silent = auto-flash in progress
# (return 0). If it stays reachable for the whole window, there is no auto-flash
# (return 1). Window ~15s — long enough to clear any brief post-upload checksum
# pause before the write, short enough to not be a "dead wait".
autoflash_in_progress() {
    local fails=0
    for _ in $(seq 1 15); do
        if ping -c 1 -W 1 "$BOOT_IP" >/dev/null 2>&1; then
            fails=0
            sleep 1
        else
            fails=$((fails + 1))
            [ "$fails" -ge 2 ] && return 0
        fi
    done
    return 1
}

print_complete() {
    echo ""
    echo "========================================="
    echo "  INSTALLATION COMPLETE"
    echo "========================================="
    echo ""
    echo "SSH: root@${GW_HINT_IP}:22 (no password) in ~30 seconds."
    echo ""
    echo "If it does not come back (a first full-flash from a pre-V2.9 bootloader"
    echo "can loop once): unplug the gateway for a few seconds and plug it back in."
    echo "A cold power cycle clears it; a warm reboot will not. V2.9 onward boots"
    echo "clean automatically after a full-flash."
}

# Post-flash SSH probe target for confirm_autoflash — only set when the
# gateway's address after the flash is known for certain: the upgrade path
# (the box held that address minutes ago and its config is preserved), or a
# static IP explicitly chosen for this install. Never a guessed default —
# an unrelated device answering SSH there would fake a success.
SSH_POLL_IP=""
if [ -n "$LINUX_RUNNING" ]; then
    SSH_POLL_IP="${LINUX_IP}"
elif [ "${NET_MODE:-}" = "static" ] && [ -n "${IPADDR:-}" ]; then
    SSH_POLL_IP="$IPADDR"
fi

# Run the confirmation and report. On OK/SSH the flash is proven done; on
# FAIL or timeout, fall back to the guided manual path (which itself starts
# by telling the user how to re-check for a quiet success).
confirm_and_report() {
    local result
    result="$(confirm_autoflash)"
    case "$result" in
    OK)
        echo ""
        echo "Flash write succeeded. The gateway will reboot automatically."
        ;;
    SSH)
        echo ""
        echo "Gateway is back up on SSH — flash write succeeded."
        ;;
    FAIL)
        echo "Auto-flash reported FAIL. Falling back to manual flash."
        manual_flw_guidance
        ;;
    *)
        echo "No auto-flash confirmation. Falling back to manual flash."
        manual_flw_guidance
        ;;
    esac
    print_complete
}

# Pre-upload capability probe (see note at top of file): Tuya / pre-ICMP stock
# bootloaders never answer ping and have no auto-flash. Only meaningful on the
# manual / first-flash path — on the boothold path the bootloader is custom by
# construction (V2.1–V2.4 answer no ICMP yet auto-flash fine), so classifying
# it "Tuya" from a missed ping is exactly the #115 false negative.
PING_BEFORE=no
if [ "$ENTRY" != "auto" ]; then
    for _i in $(seq 1 10); do
        if ping -c 1 -W 1 "$BOOT_IP" >/dev/null 2>&1; then
            PING_BEFORE=yes
            break
        fi
    done
fi

echo ""
cd "$SCRIPT_DIR"

# Upload the 16 MiB image via the shared safe-retry helper (lib/flash_tftp.sh):
# on a mid-transfer stall (discussion #135) it re-probes and retries only while
# the bootloader is still idle, never re-sending onto an in-progress auto-flash.
# The bootloader here is custom-by-construction (its WRQ probe gated the wait
# loop), so AUTOFLASH — probe gone quiet — means the image most likely landed and
# it is writing flash: fall through to confirm_and_report.
status=$(tftp_put_safe "$BOOT_IP" fullflash.bin 3 300) || true
case "$status" in
    OK)
        echo "Upload OK." ;;
    AUTOFLASH)
        : ;;   # likely already auto-flashing; fall through to confirmation
    *)
        echo "Error: TFTP transfer failed after retries." >&2
        echo "" >&2
        echo "Nothing was written (the bootloader kept answering its TFTP probe between" >&2
        echo "attempts, so no image landed). The gateway is still at the bootloader prompt" >&2
        echo "at ${BOOT_IP} — re-run this script, or power-cycle to boot existing firmware." >&2
        exit 1 ;;
esac

if [ "$ENTRY" = "auto" ]; then
    # Boothold path: the bootloader ACKed the WRQ probe and auto-flashes by
    # construction. No ICMP classification — go straight to confirmation.
    echo "Auto-flash in progress (bootloader is writing flash). Waiting for confirmation..."
    confirm_and_report
elif [ "$PING_BEFORE" = "no" ]; then
    echo "Bootloader does not answer ICMP (Tuya / pre-v2) — manual flash required."
    manual_flw_guidance
    print_complete
elif autoflash_in_progress; then
    echo "Auto-flash detected (bootloader is writing flash). Waiting for confirmation..."
    confirm_and_report
else
    echo "Bootloader stayed responsive after upload — no auto-flash. Manual flash required."
    manual_flw_guidance
    print_complete
fi

# --- EFR32 radio firmware info -----------------------------------------------
if [ "${CONFIRM:-}" != "y" ] && [ -t 0 ]; then
    echo ""
    echo "The separate EFR32 radio has not been changed. Choose and flash it next:"
    echo ""
    echo "  ./flash_efr32.sh -g ${GW_HINT_IP} ncp      # Zigbee2MQTT / ZHA"
    echo "  ./flash_efr32.sh -g ${GW_HINT_IP} rcp      # cpcd + zigbeed"
    echo "  ./flash_efr32.sh -g ${GW_HINT_IP} otrcp    # Thread / OTBR"
    echo "  ./flash_efr32.sh -g ${GW_HINT_IP} router   # standalone Zigbee router"
    echo ""
    echo "flash_efr32.sh writes radio.conf to match the firmware it installs."
fi
