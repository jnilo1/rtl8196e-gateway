#!/bin/bash
# flash_efr32.sh — Flash firmware to the Silabs EFR32 Zigbee/Thread radio
#
# Flow (v3.0+, requires kernel 6.18 with rtl8196e-uart-bridge):
#   1. Presents a menu to select the firmware type (NCP, RCP, OT-RCP, Router)
#   2. Ensures universal-silabs-flasher is available (installs in venv if needed)
#   3. SSHes into the gateway to detect mode (Zigbee vs OTBR via radio.conf):
#      - Zigbee: bridge already armed by S50uart_bridge; drop flow_control=0.
#      - OTBR  : bridge intentionally disarmed (otbr-agent owns ttyS1). Stop
#                S70otbr cleanly, self-arm the bridge at the OT-RCP baud
#                with flow_control=1 (Spinel/HDLC at 460800 needs RTS/CTS).
#      Then stop radio daemons; the bridge stays armed on TCP:8888 throughout.
#   4. Flashes the selected firmware over socket://GW:8888. When USF
#      transitions the EFR32 into the Gecko Bootloader, drop flow_control
#      to 0 (Gecko prefers XON/XOFF) and switch baud to 115200.
#   5. Restores flow_control=1 and reboots the gateway
#
# Kernel bridge sysfs (5 writable params, all under /sys/module/
# rtl8196e_uart_bridge/parameters/):
#   baud         — UART baud rate
#   flow_control — 1 = CRTSCTS on (normal); 0 = off (Gecko Bootloader Xmodem)
#   enable       — 1 = armed, 0 = disarmed
#   (tty, port, bind_addr are set at boot and not touched here)
#
# Baud rate: Pre-built firmware runs at 115200 (NCP, RCP, Router) or 460800
# (OT-RCP). Power users may run up to 892857. This script infers the current
# baud from radio.conf and the bridge's sysfs state, then tries that baud
# first. If detection fails, it scans known bauds via sysfs (instant — no
# process restart needed).
#
# Dependencies:
#   universal-silabs-flasher 1.0.3 (pinned — patch depends on this version)
#
# Usage: see ./flash_efr32.sh --help
#
# J. Nilo - February 2026, kernel-bridge rewrite April 2026,
#           FW × baud matrix + nrst_pulse pre-flash April 2026 (v3.1),
#           CLI flags + ssh_gw wrapper April 2026 (v3.1),
#           FIRMWARE_FLOW_CTRL auto-record from board.env July 2026 (#141),
#           Router board-parameterisation July 2026 (#143)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# Host-side gateway address resolution — see lib/gwconf.sh. Sourced here rather
# than next to lib/ssh.sh below because the default is needed during argument
# parsing. (lib/ssh.sh comes later, where the SSH helpers are first used.)
# shellcheck disable=SC1091
. "${SCRIPT_DIR}/lib/gwconf.sh"
GW_PORT=8888
# --gateway flag > GW_IP env > gateway.env > the last install or the last box
# reached > the gateway's hostname > the historic 192.168.1.88. Resolved through
# the variable-setting form so the source survives (a $(...) capture would set
# it in a subshell).
gwconf_resolve_gateway
GW_IP_DEFAULT="$GWCONF_ADDR"
GW_IP_DEFAULT_SOURCE="$GWCONF_ADDR_SOURCE"
VENV_DIR="${SCRIPT_DIR}/silabs-flasher"

# --- CLI parsing ---------------------------------------------------------

usage() {
    cat <<'USAGE'
Usage: flash_efr32.sh [OPTIONS] [FIRMWARE [BAUD]]

Flash an EFR32 Zigbee/Thread radio firmware on an RTL8196E gateway,
via the in-kernel UART<->TCP bridge. See --board below.

Positional arguments:
  FIRMWARE     One of:
                 bootloader  Gecko Bootloader stage 2 (UART/Xmodem)
                 ncp         NCP-UART-HW  (EZSP, for Zigbee2MQTT/ZHA)
                 rcp         RCP-UART-HW  (CPC: zigbeed EZSP 18, + Thread)
                 otrcp       OT-RCP       (OpenThread + otbr-agent)
                 router      Z3 standalone Zigbee 3.0 router
               Numeric aliases 1-5 are also accepted.
               If omitted, an interactive menu is shown.
  BAUD         UART baud for the flashed firmware. Defaults & supported
               sets per firmware (defaults are the lidl reference; a board
               without RTS/CTS wiring declares lower ones in its board.env —
               the Sengled G4 defaults rcp and otrcp to 230400):
                 ncp     115200 (default), 230400, 460800, 691200, 892857
                 rcp     460800 (default; the only lidl prebuilt), 230400
                         (Sengled G4: build it first with build_rcp.sh)
                 otrcp   230400, 460800 (default)
                 router  115200 (default; only)
               Power users can build a custom-baud GBL with
                 ./2-Zigbee-Radio-Silabs-EFR32/<dir>/build_*.sh <BAUD>
               then pass that BAUD value here.

Options:
  -g, --gateway IP   Gateway IP. Default: GW_IP, else gateway.env, else the
                     last gateway installed or reached, else its hostname.
      --firmware-file PATH
                     Flash this exact .gbl file instead of resolving by glob
  -y, --yes          Skip the "Flash?" confirmation prompt
      --board NAME   Board to flash for (default: lidl). A non-lidl board
                     flashes its -<board>-suffixed firmware. Also settable
                     via the BOARD env var (this flag wins).
      --force        Skip the safety guards: the board-match guard (flash even
                     if the gateway's devicetree model disagrees with the
                     selected board) and the bootloader version guard (flash a
                     bootloader the chip will refuse, erasing the app for
                     nothing)
      --no-reboot    Do not reboot the gateway after a successful flash
                     (useful for chaining multiple invocations)
  -h, --help         Show this help and exit

Environment variables:
  BOARD          Board to flash for (default: lidl; e.g. sengled-e39-g8c for
                 the Sengled Smart Hub G4). A Lidl user sets nothing. The
                 gateway's devicetree model is checked against this before
                 flashing (override with --force). Every firmware is
                 board-parameterised.
  SSH_PASSWORD   Root password for non-interactive password auth (CI / no
                 tty). When set, the first ssh call is fed via sshpass and
                 the ControlMaster takes over for the rest. Requires
                 sshpass (sudo apt install sshpass).
  USF_ALLOW_SYSTEM=1
                 Use a system-installed universal-silabs-flasher instead of
                 the pinned venv (default: ignore system, install venv).
                 Probe results may differ — see issue #92.

Legacy environment variables (deprecated, prefer flags):
  FW_CHOICE   1..5 (legacy v3.0.x interface)
  BAUD_CHOICE baud value
  CONFIRM     set to "y" to skip prompt

Examples:
  flash_efr32.sh                            # Interactive menu
  flash_efr32.sh -y ncp                     # NCP @ default baud
  flash_efr32.sh -y ncp 460800              # NCP @ 460800
  flash_efr32.sh -y -g 10.0.0.5 otrcp       # OT-RCP on a custom IP
  flash_efr32.sh -y --firmware-file ./fw.gbl ncp 460800
  BOARD=sengled-e39-g8c flash_efr32.sh -y ncp   # NCP on the Sengled G4
  SSH_PASSWORD=root flash_efr32.sh -y ncp   # Non-interactive password
USAGE
}

fw_alias_to_num() {
    case "$1" in
        bootloader|btl|gecko|1)        echo 1 ;;
        ncp|ncp-uart-hw|2)             echo 2 ;;
        rcp|rcp-uart-hw|3)             echo 3 ;;
        otrcp|ot-rcp|otbr|spinel|4)    echo 4 ;;
        router|z3|z3-router|5)         echo 5 ;;
        *)                             echo "" ;;
    esac
}

# Parse argv. We parse BEFORE running python3/SSH/venv checks so --help
# is accessible without any dependencies.
fw_arg=
baud_arg=
GW_IP=
NO_REBOOT=
CONFIRM_FLAG=
FIRMWARE_FILE=
board_arg=
FORCE=

while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help)         usage; exit 0 ;;
        -y|--yes)          CONFIRM_FLAG=y; shift ;;
        -g|--gateway)
                           shift
                           if [ $# -eq 0 ]; then
                               echo "Error: --gateway requires an argument." >&2
                               exit 1
                           fi
                           GW_IP="$1"; shift
                           ;;
        --gateway=*)       GW_IP="${1#--gateway=}"; shift ;;
        --firmware-file)
                           shift
                           if [ $# -eq 0 ]; then
                               echo "Error: --firmware-file requires an argument." >&2
                               exit 1
                           fi
                           FIRMWARE_FILE="$1"; shift
                           ;;
        --firmware-file=*) FIRMWARE_FILE="${1#--firmware-file=}"; shift ;;
        --board)
                           shift
                           if [ $# -eq 0 ]; then
                               echo "Error: --board requires an argument." >&2
                               exit 1
                           fi
                           board_arg="$1"; shift
                           ;;
        --board=*)         board_arg="${1#--board=}"; shift ;;
        --force)           FORCE=1; shift ;;
        --no-reboot)       NO_REBOOT=1; shift ;;
        --)                shift; break ;;
        -*)
                           echo "Error: unknown option '$1'. See --help." >&2
                           exit 1
                           ;;
        *)
                           if [ -z "$fw_arg" ]; then
                               fw_arg="$1"
                           elif [ -z "$baud_arg" ]; then
                               baud_arg="$1"
                           else
                               echo "Error: too many positional arguments. See --help." >&2
                               exit 1
                           fi
                           shift
                           ;;
    esac
done

# Resolve gateway IP: --gateway > default (see GW_IP_DEFAULT above). GW_IP is
# the parsed flag value here, not an env var — it is initialised empty at the
# top of the parsing block.
if [ -z "$GW_IP" ]; then
    GW_IP="$GW_IP_DEFAULT"
    # Say where an unstated address came from: this script reflashes the radio,
    # and on a LAN with more than one gateway the wrong target is a bad surprise.
    echo "Gateway: ${GW_IP}$(gwconf_source_note "$GW_IP_DEFAULT_SOURCE")"
fi
if ! echo "$GW_IP" | grep -qE '^[a-zA-Z0-9.-]+$'; then
    echo "Error: invalid --gateway value '$GW_IP'." >&2
    exit 1
fi

# Map flag values onto the legacy env var names so the rest of the
# script doesn't need to know about flags. CLI flags win over env vars.
# Env-var fallback emits a deprecation note on stderr.
if [ -n "$fw_arg" ]; then
    fw_num=$(fw_alias_to_num "$fw_arg")
    if [ -z "$fw_num" ]; then
        echo "Error: unknown firmware '$fw_arg'. One of: bootloader, ncp, rcp, otrcp, router (or 1-5)." >&2
        exit 1
    fi
    FW_CHOICE="$fw_num"
elif [ -n "${FW_CHOICE:-}" ]; then
    echo "Note: FW_CHOICE env var is deprecated. Use the FIRMWARE positional arg." >&2
fi

if [ -n "$baud_arg" ]; then
    if ! echo "$baud_arg" | grep -qE '^[0-9]+$'; then
        echo "Error: BAUD must be a positive integer, got '$baud_arg'." >&2
        exit 1
    fi
    BAUD_CHOICE="$baud_arg"
elif [ -n "${BAUD_CHOICE:-}" ]; then
    echo "Note: BAUD_CHOICE env var is deprecated. Use the BAUD positional arg." >&2
fi

if [ -n "$CONFIRM_FLAG" ]; then
    CONFIRM=y
elif [ "${CONFIRM:-}" = "y" ]; then
    echo "Note: CONFIRM=y env var is deprecated. Use the -y/--yes flag." >&2
fi

# --- Dependency checks (after CLI parsing so --help works without them) ---

if ! command -v python3 >/dev/null 2>&1; then
    echo "Error: python3 not found." >&2
    echo "Install it with: sudo apt install python3" >&2
    exit 1
fi
if ! python3 -c "import venv" 2>/dev/null; then
    echo "Error: python3-venv not found." >&2
    echo "Install it with: sudo apt install python3-venv" >&2
    exit 1
fi
if [ -n "${SSH_PASSWORD:-}" ] && ! command -v sshpass >/dev/null 2>&1; then
    echo "Error: SSH_PASSWORD is set but sshpass is not installed." >&2
    echo "Install it with: sudo apt install sshpass" >&2
    exit 1
fi

BRIDGE_SYSFS="/sys/module/rtl8196e_uart_bridge/parameters"

# Hardened SSH options + ssh_retry helper + wait_for_port live in lib/ssh.sh
# (shared with flash_remote.sh and flash_install_rtl8196e.sh).
. "${SCRIPT_DIR}/lib/ssh.sh"

# ssh_gw — flash_efr32-specific SSH wrapper around lib/ssh.sh's ssh_retry.
#
#   ssh_gw "one-liner-command"     # remote runs the literal string
#   ssh_gw <<'EOF'                  # remote runs heredoc via `sh -s`
#       multi-line script
#   EOF
#
# Note: the remote side runs `sh -s` (POSIX, BusyBox-ash compatible),
# NOT `bash -s` — the gateway has no bash binary
# (CONFIG_BASH_IS_NONE=y in the rootfs BusyBox config).
#
# StrictHostKeyChecking=accept-new is appropriate here because the
# gateway is already up and reachable in this script's workflow (the
# host key was learned at first contact). For first-flash scenarios,
# flash_install_rtl8196e.sh / flash_remote.sh use a looser policy.
ssh_gw() {
    local target="root@${GW_IP}"
    if [ $# -gt 0 ]; then
        ssh_retry "${SSH_HARDEN_OPTS[@]}" -o StrictHostKeyChecking=accept-new "$target" "$@"
    else
        # 'sh -s' = POSIX shell reading stdin. Works with BusyBox ash
        # on the gateway (no bash available there).
        ssh_retry "${SSH_HARDEN_OPTS[@]}" -o StrictHostKeyChecking=accept-new "$target" 'sh -s'
    fi
}

FW_DIR="${SCRIPT_DIR}/2-Zigbee-Radio-Silabs-EFR32"

# --- Board selection -------------------------------------------------------
#
# BOARD picks which board's firmware to flash (default lidl). A Lidl user sets
# nothing. A non-lidl board flashes its `-<board>`-suffixed firmware, built
# from boards/<board>/board.env (see 2-Zigbee-Radio-Silabs-EFR32/boards/
# README.md). Every firmware is board-parameterised (#143). The --board flag
# wins over the BOARD env var, which wins over the default.
BOARD="${board_arg:-${BOARD:-lidl}}"
if [ ! -f "${FW_DIR}/boards/${BOARD}/board.env" ]; then
    echo "Error: unknown BOARD='${BOARD}'." >&2
    echo "Available boards: $(cd "${FW_DIR}/boards" 2>/dev/null && ls -d */ 2>/dev/null | tr -d /)" >&2
    exit 1
fi
# Prefix for the example commands we echo back (empty for lidl).
if [ "$BOARD" = "lidl" ]; then BOARD_PREFIX=""; else BOARD_PREFIX="BOARD=${BOARD} "; fi

# Chip-side flow-control mode of this board's firmware builds (hw|sw|none),
# from board.env. Recorded into radio.conf as FIRMWARE_FLOW_CTRL after an
# app-slot flash (#141) so S50uart_bridge / S70otbr configure the host side
# of the UART to match the chip, with nothing to edit manually.
BOARD_FLOW=$(. "${FW_DIR}/boards/${BOARD}/board.env" && printf '%s\n' "${BOARD_UART_FLOW:-}")
case "$BOARD_FLOW" in
    hw|sw|none) ;;
    *)
        echo "Error: BOARD_UART_FLOW='${BOARD_FLOW}' in boards/${BOARD}/board.env — expected hw, sw or none." >&2
        exit 1
        ;;
esac

# --- Firmware × baud matrix ------------------------------------------------
#
# Pre-built GBLs embed the baud and (since #145) the UART flow-control type;
# OT-RCP also embeds the UART driver. <flow> = hw|sw|none is the as-built value
# (RCP has no sw mode → sw boards build/name as none). A non-lidl board adds a
# -<board> suffix last.
#   ncp-uart-hw-<EmberVersion>-<BAUD>-<flow>.gbl
#   rcp-uart-802154-<BAUD>-<flow>.gbl
#   ot-rcp-<BAUD>-<flow>-<driver>.gbl        (driver = uartdrv|iostream)
#   z3-router-<EmberVersion>-<BAUD>-<flow>.gbl
#
# Per-firmware baud sets (per CHANGELOG v3.0.0 max-tested values):
#   NCP-UART-HW : 115200, 230400, 460800, 691200, 892857
#   RCP-UART-HW : 230400, 460800                   (lidl ships 460800 only;
#                                                    230400 = no-RTS/CTS boards,
#                                                    build-it-yourself)
#   OT-RCP      : 230400, 460800                    (460800 = otbr-agent ceiling;
#                                                    230400 = no-RTS/CTS boards, #134)
#   Z3-Router   : 115200                            (text CLI only)
#
# resolve_firmware <fw_choice> <baud> -> sets FIRMWARE global to GBL path

NCP_BAUDS="115200 230400 460800 691200 892857"
RCP_BAUDS="230400 460800"
OT_RCP_BAUDS="230400 460800"
ROUTER_BAUDS="115200"

NCP_DEFAULT_BAUD=115200
RCP_DEFAULT_BAUD=460800
OT_RCP_DEFAULT_BAUD=460800
ROUTER_DEFAULT_BAUD=115200

# Per-board default baud, for the firmwares where the board changes the answer.
# The defaults above are the lidl reference, which wires RTS/CTS. A board
# without it has a lower reliable ceiling — @hlyi's G4 measurements (#134,
# #142) put the operating point at 230400, because at 460800 the host's
# 16-byte RX FIFO overruns and no software flow control can protect that
# stage. Such a board declares BOARD_RCP_DEFAULT_BAUD / BOARD_OT_RCP_DEFAULT_BAUD
# in its board.env, so the menu offers — and a non-interactive run picks — the
# baud whose prebuilt is committed for that board. Absent keys keep the
# reference defaults.
RCP_DEFAULT_BAUD=$(. "${FW_DIR}/boards/${BOARD}/board.env" && printf '%s\n' "${BOARD_RCP_DEFAULT_BAUD:-$RCP_DEFAULT_BAUD}")
OT_RCP_DEFAULT_BAUD=$(. "${FW_DIR}/boards/${BOARD}/board.env" && printf '%s\n' "${BOARD_OT_RCP_DEFAULT_BAUD:-$OT_RCP_DEFAULT_BAUD}")
for _spec in "RCP:${RCP_DEFAULT_BAUD}:${RCP_BAUDS}" "OT_RCP:${OT_RCP_DEFAULT_BAUD}:${OT_RCP_BAUDS}"; do
    _fw="${_spec%%:*}"; _rest="${_spec#*:}"; _want="${_rest%%:*}"; _set="${_rest#*:}"
    case " ${_set} " in
        *" ${_want} "*) ;;
        *)
            echo "Error: BOARD_${_fw}_DEFAULT_BAUD='${_want}' in boards/${BOARD}/board.env is not a supported baud (${_set})." >&2
            exit 1
            ;;
    esac
done
unset _spec _fw _rest _want _set

FW_BTL="${FW_DIR}/23-Bootloader-UART-Xmodem/firmware/bootloader-uart-xmodem-2.4.2.gbl"

# resolve_firmware <fw_choice> <baud> -> sets FIRMWARE / FW_LABEL globals
#
# We glob the firmware directory rather than computing exact filenames.
# This avoids the EmberZNet version detection (which required the Silabs
# SDK to be installed locally just to flash) and tolerates filename
# evolution across firmware bumps.
#
# The flow (and OT-RCP driver) field is anchored exactly from the board's
# BOARD_UART_FLOW, so a forced-driver experiment (e.g. ot-rcp-…-hw-iostream on a
# lidl board whose default is uartdrv) is never auto-resolved.
# Pattern per firmware (<flow>/<driver> computed below from BOARD_FLOW):
#   1 (Bootloader) : pinned exact path for lidl; for a non-lidl board,
#                    23-Bootloader-UART-Xmodem/firmware/bootloader-uart-xmodem-*-<board>.gbl
#   2 (NCP)    : 24-NCP-UART-HW/firmware/ncp-uart-hw-*-<BAUD>-<flow>.gbl
#   3 (RCP)    : 25-RCP-UART-HW/firmware/rcp-uart-802154-<BAUD>-<flow>.gbl
#   4 (OT-RCP) : 26-OT-RCP/firmware/ot-rcp-<BAUD>-<flow>-<driver>.gbl
#   5 (Router) : 27-Router/firmware/z3-router-*-<BAUD>-<flow>.gbl
#
# If multiple GBLs match (e.g. two EmberZNet versions side-by-side), refuse
# the implicit choice. Use --firmware-file to make the image selection explicit.
resolve_firmware() {
    local choice="$1" baud="$2"
    local pattern dir build_dir build_script bsuf=""
    local candidates candidate_count flowtag drv

    # Per-board selection: every firmware is board-parameterised; non-lidl
    # artefacts carry a -<board> filename suffix in the same flat firmware/
    # dir. An explicit --firmware-file bypasses all board logic (power-user
    # escape hatch).
    if [ -z "$FIRMWARE_FILE" ] && [ "$BOARD" != "lidl" ]; then
        bsuf="-${BOARD}"
    fi

    # As-built flow tag embedded in the filename (#145), mirroring the build
    # scripts: RCP has no software flow control, so a sw board's RCP is built
    # and named as none. OT-RCP additionally carries the auto-selected UART
    # driver (sw -> iostream, hw|none -> uartdrv); anchoring it exactly means a
    # forced-driver experiment never shadows the default build.
    flowtag="$BOARD_FLOW"
    case "$choice" in
        3) [ "$BOARD_FLOW" = "sw" ] && flowtag=none ;;
        4) case "$BOARD_FLOW" in sw) drv=iostream ;; *) drv=uartdrv ;; esac ;;
    esac

    case "$choice" in
        1)  # Gecko Bootloader — lidl keeps the pinned exact path (no baud in
            # the filename to anchor a safe glob); non-lidl boards resolve
            # their -<board>-suffixed build through the glob path below.
            if [ -z "$bsuf" ]; then
                FIRMWARE="${FIRMWARE_FILE:-$FW_BTL}"; FW_LABEL="Gecko Bootloader"; return 0
            fi
            build_dir="23-Bootloader-UART-Xmodem"; build_script="build_bootloader.sh"; pattern="bootloader-uart-xmodem-*${bsuf}.gbl"; FW_LABEL="Gecko Bootloader" ;;
        2) build_dir="24-NCP-UART-HW";  build_script="build_ncp.sh";    pattern="ncp-uart-hw-*-${baud}-${flowtag}${bsuf}.gbl";   FW_LABEL="NCP-UART-HW @ ${baud} baud" ;;
        3) build_dir="25-RCP-UART-HW";  build_script="build_rcp.sh";    pattern="rcp-uart-802154-${baud}-${flowtag}${bsuf}.gbl"; FW_LABEL="RCP-UART-HW @ ${baud} baud" ;;
        4) build_dir="26-OT-RCP";       build_script="build_ot_rcp.sh"; pattern="ot-rcp-${baud}-${flowtag}-${drv}${bsuf}.gbl";  FW_LABEL="OT-RCP @ ${baud} baud" ;;
        5) build_dir="27-Router";       build_script="build_router.sh"; pattern="z3-router-*-${baud}-${flowtag}${bsuf}.gbl";     FW_LABEL="Z3-Router @ ${baud} baud" ;;
        *) echo "Invalid firmware choice: $choice" >&2; exit 1 ;;
    esac
    if [ -n "$FIRMWARE_FILE" ]; then
        FIRMWARE="$FIRMWARE_FILE"
        if [ ! -f "$FIRMWARE" ]; then
            echo "Error: --firmware-file does not exist: $FIRMWARE" >&2
            exit 1
        fi
        case "$FIRMWARE" in
            *.gbl) ;;
            *) echo "Error: --firmware-file must point to a .gbl file: $FIRMWARE" >&2; exit 1 ;;
        esac
        return 0
    fi
    dir="${FW_DIR}/${build_dir}/firmware"
    candidates=$(find "$dir" -maxdepth 1 -type f -name "$pattern" -print 2>/dev/null | sort)
    candidate_count=$(printf '%s\n' "$candidates" | sed '/^$/d' | wc -l)
    FIRMWARE=$(printf '%s\n' "$candidates" | sed '/^$/d' | head -1)
    if [ -z "$FIRMWARE" ] || [ ! -f "$FIRMWARE" ]; then
        echo "Error: no GBL found matching $dir/$pattern" >&2
        echo "       Build with: cd 2-Zigbee-Radio-Silabs-EFR32/${build_dir} && ${BOARD_PREFIX}./${build_script} ${baud}" >&2
        # make-all-bauds.sh takes the same BOARD= selector and rebuilds that
        # board's committed matrix, whichever board is selected.
        echo "       Or run: cd 2-Zigbee-Radio-Silabs-EFR32 && ${BOARD_PREFIX}./make-all-bauds.sh" >&2
        exit 1
    fi
    if [ "$candidate_count" -gt 1 ]; then
        echo "Error: multiple GBL files match $dir/$pattern:" >&2
        printf '  %s\n' $candidates >&2
        echo "Use --firmware-file PATH to select the exact image." >&2
        exit 1
    fi
}

# --- Firmware selection menu -----------------------------------------------

if [ -n "${FW_CHOICE:-}" ]; then
    fw_choice="$FW_CHOICE"
else
    echo "EFR32 Firmware Flasher"
    echo ""
    echo "  [1] Bootloader    — Gecko Bootloader stage 2 (UART/Xmodem)"
    echo "  [2] NCP-UART-HW   — Zigbee NCP for zigbee2mqtt / ZHA         (bauds: ${NCP_BAUDS})"
    echo "  [3] RCP-UART-HW   — Multi-PAN RCP for zigbee2mqtt            (bauds: ${RCP_BAUDS})"
    echo "  [4] OT-RCP        — OpenThread RCP for otbr-agent            (bauds: ${OT_RCP_BAUDS})"
    echo "  [5] Z3-Router     — Zigbee 3.0 standalone router             (bauds: ${ROUTER_BAUDS})"
    echo ""
    read -r -p "Firmware to flash [2]: " fw_choice
    fw_choice="${fw_choice:-2}"
fi

# --- Baud selection menu ---------------------------------------------------
# (skipped for the bootloader, which has no baud variant)

case "$fw_choice" in
    1) DEFAULT_FW_BAUD="" ; ALLOWED_BAUDS="" ;;
    2) DEFAULT_FW_BAUD="$NCP_DEFAULT_BAUD"   ; ALLOWED_BAUDS="$NCP_BAUDS" ;;
    3) DEFAULT_FW_BAUD="$RCP_DEFAULT_BAUD"   ; ALLOWED_BAUDS="$RCP_BAUDS" ;;
    4) DEFAULT_FW_BAUD="$OT_RCP_DEFAULT_BAUD"; ALLOWED_BAUDS="$OT_RCP_BAUDS" ;;
    5) DEFAULT_FW_BAUD="$ROUTER_DEFAULT_BAUD"; ALLOWED_BAUDS="$ROUTER_BAUDS" ;;
    *) echo "Invalid choice."; exit 1 ;;
esac

if [ "$fw_choice" = "1" ]; then
    fw_baud=""   # bootloader has no baud variant
elif [ -n "${BAUD_CHOICE:-}" ]; then
    fw_baud="$BAUD_CHOICE"
elif [ -z "${FW_CHOICE:-}" ]; then
    # Interactive prompt (only when FW_CHOICE wasn't passed via env either).
    echo ""
    echo "Available bauds for this firmware: ${ALLOWED_BAUDS}"
    read -r -p "Baud [${DEFAULT_FW_BAUD}]: " fw_baud
    fw_baud="${fw_baud:-$DEFAULT_FW_BAUD}"
else
    fw_baud="$DEFAULT_FW_BAUD"   # FW_CHOICE set without BAUD_CHOICE → default
fi

# Validate the chosen baud is in the firmware's allowed set.
if [ -n "$fw_baud" ]; then
    case " ${ALLOWED_BAUDS} " in
        *" ${fw_baud} "*) ;;
        *)
            echo "Error: baud ${fw_baud} not in supported set for this firmware." >&2
            echo "       Supported: ${ALLOWED_BAUDS}" >&2
            echo "       Build a custom GBL with: cd 2-Zigbee-Radio-Silabs-EFR32/<dir> && ./build_*.sh ${fw_baud}" >&2
            exit 1
            ;;
    esac
fi

resolve_firmware "$fw_choice" "$fw_baud"
if [ ! -f "$FIRMWARE" ]; then
    echo "Error: firmware image not found: $FIRMWARE" >&2
    exit 1
fi

echo ""
echo "Firmware: $(basename "$FIRMWARE")"
echo "Image:    $FIRMWARE"
[ "$BOARD" != "lidl" ] && echo "Board:    ${BOARD}"
echo "Gateway:  ${GW_IP}:${GW_PORT}"
echo ""

# --- 1. Check / install universal-silabs-flasher ---------------------------
#
# Always use the pinned venv install (USF 1.0.3 + our probe-methods patch).
# A system-wide universal-silabs-flasher in $PATH is intentionally ignored:
# older releases use `--probe-method` (singular, no comma-list) instead of
# `--probe-methods`, and none of them carry our DEFAULT_PROBE_METHODS
# extension (extra bauds for EZSP/SPINEL/CPC). Issue #92.
#
# Operators who really want to use a system USF can set USF_ALLOW_SYSTEM=1,
# but the bauds and CLI must match the pinned version or probe will fail.

PATCH_FILE="$SCRIPT_DIR/silabs-flasher-probe-methods.patch"
PATCH_HASH_FILE="${VENV_DIR}/.patch-hash"

venv_usf_version() {
    [ -x "${VENV_DIR}/bin/python" ] || return 1
    "${VENV_DIR}/bin/python" -c 'import importlib.metadata as m; print(m.version("universal-silabs-flasher"))' 2>/dev/null
}

# Reinstall if the existing venv is not the pinned version. A stale venv is
# worse than no venv here because probe-method CLI and baud lists changed.
if [ -x "${VENV_DIR}/bin/universal-silabs-flasher" ]; then
    installed_usf_version=$(venv_usf_version || true)
    if [ "$installed_usf_version" != "1.0.3" ]; then
        echo "universal-silabs-flasher venv is ${installed_usf_version:-unknown}, expected 1.0.3 — reinstalling..."
        rm -rf "$VENV_DIR"
    fi
fi

# Reinstall if the probe-methods patch has changed since last install.
if [ -x "${VENV_DIR}/bin/universal-silabs-flasher" ] && [ -f "$PATCH_FILE" ]; then
    current_hash=$(md5sum "$PATCH_FILE" 2>/dev/null | awk '{print $1}')
    applied_hash=$(cat "$PATCH_HASH_FILE" 2>/dev/null || true)
    if [ "$current_hash" != "$applied_hash" ]; then
        echo "Probe methods patch changed — reinstalling USF..."
        rm -rf "$VENV_DIR"
    fi
fi

install_usf_venv() {
    echo "Installing universal-silabs-flasher 1.0.3 in ${VENV_DIR}..."
    python3 -m venv "$VENV_DIR"
    "${VENV_DIR}/bin/pip" install --quiet universal-silabs-flasher==1.0.3
    # Patch USF to probe Spinel/EZSP/CPC at extra bauds (upstream's
    # DEFAULT_PROBE_METHODS misses 230400, 691200, 892857 etc.)
    USF_CONST=$(find "$VENV_DIR" -path '*/universal_silabs_flasher/const.py' -print -quit)
    if [ -n "$USF_CONST" ] && [ -f "$PATCH_FILE" ] && \
       patch --dry-run -f "$USF_CONST" "$PATCH_FILE" >/dev/null 2>&1; then
        patch -f "$USF_CONST" "$PATCH_FILE" >/dev/null
        md5sum "$PATCH_FILE" | awk '{print $1}' > "$PATCH_HASH_FILE"
        echo "Installed (patched probe methods)."
    else
        echo "Installed (no patch applied — file or patch missing)."
    fi
}

if [ "${USF_ALLOW_SYSTEM:-0}" = "1" ] && command -v universal-silabs-flasher >/dev/null 2>&1; then
    FLASHER="$(command -v universal-silabs-flasher)"
    echo "universal-silabs-flasher: ${FLASHER} (system, USF_ALLOW_SYSTEM=1)"
    echo "Note: probe results may differ from the pinned 1.0.3 install — see #92."
else
    if [ ! -x "${VENV_DIR}/bin/universal-silabs-flasher" ]; then
        install_usf_venv
    fi
    installed_usf_version=$(venv_usf_version || true)
    if [ "$installed_usf_version" != "1.0.3" ]; then
        echo "Error: pinned USF venv is not usable (version=${installed_usf_version:-unknown}, expected 1.0.3)." >&2
        exit 1
    fi
    if [ -f "$PATCH_FILE" ]; then
        current_hash=$(md5sum "$PATCH_FILE" 2>/dev/null | awk '{print $1}')
        applied_hash=$(cat "$PATCH_HASH_FILE" 2>/dev/null || true)
        if [ "$current_hash" != "$applied_hash" ]; then
            echo "Error: USF probe-methods patch is not recorded as applied." >&2
            echo "Delete ${VENV_DIR} and rerun, or inspect ${PATCH_FILE}." >&2
            exit 1
        fi
    fi
    FLASHER="${VENV_DIR}/bin/universal-silabs-flasher"
    echo "universal-silabs-flasher: venv (${VENV_DIR})"
fi
echo ""

# --- 2. SSH: verify bridge, detect baud, switch to flash mode --------------
# The in-kernel UART bridge exposes sysfs knobs; we change baud and
# flow_control without disarming the bridge.  TCP:8888 stays up across
# all operations below.

echo "Connecting to ${GW_IP} — detecting configuration..."
# Open the ControlMaster up-front using SSH_PASSWORD if provided
# (no-op when SSH_PASSWORD is unset — ssh's tty prompt handles it).
if ! ssh_prime_with_password "${SSH_HARDEN_OPTS[@]}" \
        -o StrictHostKeyChecking=accept-new "root@${GW_IP}"; then
    exit 1
fi
# Remote shell emits structured KEY=VALUE lines (one per line). Local
# parsing is then trivial via grep, no fragile suffix-matching. Any
# stderr/non-KEY=VALUE noise from `set -u` etc. is tolerated.
DETECT_OUT=$(ssh_gw "BRIDGE_SYSFS='$BRIDGE_SYSFS' BRIDGE_PORT='$GW_PORT' sh -s" <<'REMOTE_EOF'
emit() { echo "$1=$2"; }

# bridge_active_peer: prints "IP:PORT" if a TCP client has an ESTABLISHED
# connection to the bridge port, empty otherwise. Reads /proc/net/tcp on
# the gateway so it sees clients regardless of where they live (the
# gateway itself, the host running flash_efr32.sh, or a third box like a
# Pi/NAS running zigbee2mqtt).
# RTL8196E is little-endian — IP fields in /proc/net/tcp are byte-reversed.
bridge_active_peer() {
    PORT_HEX=$(printf '%04X' "$BRIDGE_PORT")
    awk -v ph="$PORT_HEX" '
        BEGIN {
            for (i = 0; i < 10; i++) hv[i] = i
            hv["A"]=10; hv["B"]=11; hv["C"]=12; hv["D"]=13; hv["E"]=14; hv["F"]=15
            hv["a"]=10; hv["b"]=11; hv["c"]=12; hv["d"]=13; hv["e"]=14; hv["f"]=15
        }
        function h2d(s,    n, i) {
            n = 0
            for (i = 1; i <= length(s); i++) n = n * 16 + hv[substr(s, i, 1)]
            return n
        }
        NR == 1 { next }
        $4 != "01" { next }
        {
            split($2, lp, ":")
            if (lp[2] != ph) next
            split($3, rp, ":")
            printf "%d.%d.%d.%d:%d",
                h2d(substr(rp[1], 7, 2)), h2d(substr(rp[1], 5, 2)),
                h2d(substr(rp[1], 3, 2)), h2d(substr(rp[1], 1, 2)),
                h2d(rp[2])
            exit
        }
    ' /proc/net/tcp 2>/dev/null
}

# Bridge present? (kernel 6.18 with CONFIG_RTL8196E_UART_BRIDGE=y)
if [ ! -d "$BRIDGE_SYSFS" ]; then
    emit STATUS no-bridge
    exit 0
fi

# Mode is the source of truth for who owns ttyS1:
#   Zigbee : S50uart_bridge arms the bridge at boot
#   OTBR   : otbr-agent (started by S70otbr) holds ttyS1; bridge disarmed
MODE=zigbee
FIRMWARE_BAUD_CFG=
if [ -f /userdata/etc/radio.conf ]; then
    grep -q '^MODE=otbr' /userdata/etc/radio.conf && MODE=otbr
    FIRMWARE_BAUD_CFG=$(grep '^FIRMWARE_BAUD=' /userdata/etc/radio.conf | tail -1 | cut -d= -f2)
fi
emit MODE "$MODE"
emit CFG_BAUD "$FIRMWARE_BAUD_CFG"

# Board model from the devicetree — authoritative since this script always runs
# against a live gateway. The trailing NUL is dropped by command substitution.
emit MODEL "$(cat /proc/device-tree/model 2>/dev/null)"

ARMED=$(cat "$BRIDGE_SYSFS/armed" 2>/dev/null || echo 0)
SELF_ARMED=0

if [ "$ARMED" != '1' ]; then
    if [ "$MODE" = 'otbr' ]; then
        # Stop S70otbr cleanly: the init script's stop path handles the
        # LED + 30-s sync daemon trap and final dataset flush — a bare
        # `killall otbr-agent` would skip those.
        /userdata/etc/init.d/S70otbr stop >/dev/null 2>&1 || \
            killall otbr-agent 2>/dev/null
        sleep 1
        # Arm at the radio.conf-derived baud (default 460800 for OT-RCP).
        # flow_control=1 because Spinel/HDLC at 460800 needs RTS/CTS for
        # probe reliability — dropped to 0 lower down once USF moves the
        # EFR32 into the Gecko Bootloader.
        BAUD=${FIRMWARE_BAUD_CFG:-460800}
        echo "$BAUD" > "$BRIDGE_SYSFS/baud"
        echo 1 > "$BRIDGE_SYSFS/flow_control"
        echo 1 > "$BRIDGE_SYSFS/enable"
        sleep 1
        # flow_control readback is non-zero on success: 1 on a board that wires
        # RTS/CTS, or 2 (sw) on one that does not, where the bridge clamps the
        # hw request. Only a literal 0 means flow control failed to engage.
        if [ "$(cat "$BRIDGE_SYSFS/armed" 2>/dev/null)" != '1' ] || \
           [ "$(cat "$BRIDGE_SYSFS/baud" 2>/dev/null)" != "$BAUD" ] || \
           [ "$(cat "$BRIDGE_SYSFS/flow_control" 2>/dev/null)" = '0' ]; then
            emit STATUS self-arm-failed
            exit 0
        fi
        ARMED=1
        SELF_ARMED=1
        emit BAUD "$BAUD"
        emit ARMED "$ARMED"
        emit SELF_ARMED "$SELF_ARMED"
        emit PEER "$(bridge_active_peer)"
        emit STATUS ok
        exit 0
    fi
    emit STATUS not-armed
    exit 0
fi

# Already armed (Zigbee path): read current baud — authoritative for
# what the EFR32 currently sees.
BAUD=$(cat "$BRIDGE_SYSFS/baud" 2>/dev/null || echo 115200)
emit BAUD "$BAUD"
emit ARMED "$ARMED"
emit SELF_ARMED "$SELF_ARMED"
emit PEER "$(bridge_active_peer)"
emit STATUS ok
REMOTE_EOF
)
DETECT_RC=$?
if [ $DETECT_RC -ne 0 ]; then
    echo "Error: cannot reach gateway ${GW_IP} (rc=$DETECT_RC)." >&2
    exit 1
fi
# We just ran our own detection script on the box at this address — remember it,
# so the other host-side tools can find the gateway without an argument.
gwconf_record_seen "$GW_IP"

# Parse KEY=VALUE lines. We grep for safety (other shells could spit
# unexpected noise on stderr) and use awk to extract the value of each
# key so we can populate locals defensively.
detect_get() { echo "$DETECT_OUT" | awk -F= -v k="$1" '$1==k {print $2}' | tail -1; }
DETECT_STATUS=$(detect_get STATUS)
RADIO_MODE=$(detect_get MODE)
CONFIG_BAUD=$(detect_get CFG_BAUD)
CURRENT_BAUD=$(detect_get BAUD)
SELF_ARMED=$(detect_get SELF_ARMED)
PEER=$(detect_get PEER)
GW_MODEL=$(detect_get MODEL)

case "$DETECT_STATUS" in
    ok) ;;
    no-bridge)
        echo "Error: in-kernel UART bridge not found on ${GW_IP}." >&2
        echo "This script requires kernel 6.18 with CONFIG_RTL8196E_UART_BRIDGE=y." >&2
        exit 1
        ;;
    not-armed)
        echo "Error: UART bridge is not armed on ${GW_IP}." >&2
        echo "Check S50uart_bridge init script; or arm manually:" >&2
        echo "  echo 1 > ${BRIDGE_SYSFS}/enable" >&2
        exit 1
        ;;
    self-arm-failed)
        echo "Error: failed to self-arm the bridge on ${GW_IP} (OTBR mode)." >&2
        echo "Check that S70otbr stopped cleanly; or arm manually:" >&2
        echo "  /userdata/etc/init.d/S70otbr stop" >&2
        echo "  echo 460800 > ${BRIDGE_SYSFS}/baud" >&2
        echo "  echo 1     > ${BRIDGE_SYSFS}/flow_control" >&2
        echo "  echo 1     > ${BRIDGE_SYSFS}/enable" >&2
        exit 1
        ;;
    *)
        echo "Error: unexpected detection STATUS='${DETECT_STATUS}'." >&2
        echo "Raw output:" >&2; echo "$DETECT_OUT" >&2
        exit 1
        ;;
esac

# --- Board-match guardrail -------------------------------------------------
#
# flash_efr32 always runs against a live gateway, so /proc/device-tree/model is
# authoritative: refuse to push a board's radio firmware to a different board.
# The effective BOARD is the explicit --board/BOARD= or the default lidl, so this
# also catches the common slip — forgetting BOARD= on a G4 box (effective lidl
# vs a "Sengled" model). Skipped only if the model is unreadable; --force bypasses.
if [ "$FORCE" != "1" ] && [ -n "$GW_MODEL" ]; then
    case "$BOARD" in
        lidl)            want_sig="Lidl" ;;
        sengled-e39-g8c) want_sig="Sengled" ;;
        *)               want_sig="" ;;   # board with no known model signature
    esac
    if [ -n "$want_sig" ] && ! printf '%s' "$GW_MODEL" | grep -q "$want_sig"; then
        echo "Error: board mismatch — selected BOARD='${BOARD}', but ${GW_IP} reports:" >&2
        echo "         model = \"${GW_MODEL}\"" >&2
        case "$GW_MODEL" in
            *Sengled*) echo "  This looks like a Sengled G4 — re-run with BOARD=sengled-e39-g8c." >&2 ;;
            *Lidl*)    echo "  This looks like a Lidl gateway — re-run with BOARD=lidl (the default)." >&2 ;;
        esac
        echo "  Flashing another board's radio firmware is wrong for this hardware." >&2
        echo "  Re-run with the matching BOARD=, or pass --force to override." >&2
        exit 1
    fi
fi

# Refuse to flash if another TCP client is already attached. The bridge
# silently replaces clients (rtl8196e_uart_bridge_main.c "replacing
# previous client"), which would kick a running Z2M / ZHA / otbr-agent
# on any host and let it fight USF for the socket. Up to the user to
# stop it on the right machine — we only report where it lives.
if [ -n "$PEER" ]; then
    echo "Error: TCP:${GW_PORT} on ${GW_IP} already has an active client (${PEER})." >&2
    echo "Stop the zigbee2mqtt / ZHA / otbr-agent talking to the gateway, then re-run." >&2
    exit 1
fi

assert_bridge_idle() {
    local peer
    peer=$(ssh_gw "BRIDGE_PORT='$GW_PORT' sh -s" <<'REMOTE_EOF'
PORT_HEX=$(printf '%04X' "$BRIDGE_PORT")
awk -v ph="$PORT_HEX" '
    NR == 1 { next }
    $4 != "01" { next }
    {
        split($2, lp, ":")
        if (lp[2] == ph) {
            print $3
            exit
        }
    }
' /proc/net/tcp 2>/dev/null
REMOTE_EOF
)
    if [ -n "$peer" ]; then
        echo "Error: TCP:${GW_PORT} on ${GW_IP} gained an active client (${peer}) before flashing." >&2
        echo "Stop the client and rerun; refusing to race the flasher against a live radio user." >&2
        exit 1
    fi
}

# Defaults if a field is somehow empty (shouldn't happen; defensive).
RADIO_MODE="${RADIO_MODE:-zigbee}"
CURRENT_BAUD="${CURRENT_BAUD:-115200}"

if [ -n "$CONFIG_BAUD" ]; then
    if ! echo "$CONFIG_BAUD" | grep -qE '^[0-9]+$'; then
        echo "Warning: ignoring invalid FIRMWARE_BAUD in radio.conf: '$CONFIG_BAUD'" >&2
        CONFIG_BAUD=
    elif [ "$CONFIG_BAUD" != "$CURRENT_BAUD" ]; then
        echo "Reconciling bridge baud: sysfs=${CURRENT_BAUD}, radio.conf=${CONFIG_BAUD}"
        if [ "$RADIO_MODE" = "otbr" ]; then
            ssh_gw "
                echo ${CONFIG_BAUD} > ${BRIDGE_SYSFS}/baud
                echo 1 > ${BRIDGE_SYSFS}/flow_control
                [ \"\$(cat ${BRIDGE_SYSFS}/baud 2>/dev/null)\" = '${CONFIG_BAUD}' ]
                [ \"\$(cat ${BRIDGE_SYSFS}/flow_control 2>/dev/null)\" != '0' ]
            "
        else
            ssh_gw "
                echo ${CONFIG_BAUD} > ${BRIDGE_SYSFS}/baud
                echo 0 > ${BRIDGE_SYSFS}/flow_control
                [ \"\$(cat ${BRIDGE_SYSFS}/baud 2>/dev/null)\" = '${CONFIG_BAUD}' ]
                [ \"\$(cat ${BRIDGE_SYSFS}/flow_control 2>/dev/null)\" = '0' ]
            "
        fi
        CURRENT_BAUD="$CONFIG_BAUD"
    fi
fi

if [ "$SELF_ARMED" = "1" ]; then
    echo "Detected: ${RADIO_MODE} @ ${CURRENT_BAUD} baud (bridge self-armed; S70otbr stopped)"
else
    echo "Detected: ${RADIO_MODE} @ ${CURRENT_BAUD} baud (bridge armed)"
fi

# Remember the original baud so we can restore it at cleanup (in case the
# flash fails halfway — the bridge would otherwise be left at 115200 and
# any zigbeed/otbr-agent trying to restart would talk at the wrong speed).
ORIG_BAUD="$CURRENT_BAUD"

# Switch bridge to flash mode: stop daemons, optionally disable RTS/CTS.
# The bridge stays armed — TCP:8888 never drops.
#
# Flow control policy:
#   Zigbee path (NCP/RCP): drop flow_control=0 right away. EZSP/CPC probes
#     succeed without RTS/CTS at the application baud, and Gecko Bootloader
#     prefers no HW flow control once we transition there.
#   OTBR path (Spinel/HDLC): keep flow_control=1 during the probe — at
#     460800 the EFR32 OT-RCP firmware demands RTS/CTS, dropping it loses
#     the probe response. flow_control=0 is set lower down, only after USF
#     has put the EFR32 into Gecko Bootloader.
if [ "$RADIO_MODE" = "otbr" ]; then
    echo "Switching bridge to flash mode (flow_control=1, OTBR path)..."
else
    echo "Switching bridge to flash mode (flow_control=0)..."
fi
ssh_gw "
    killall otbr-agent 2>/dev/null || true
    killall cpcd 2>/dev/null || true
    killall zigbeed 2>/dev/null || true
    # Stop LED PWM timer (interferes with UART during Xmodem transfer)
    echo 0 > /sys/class/leds/status/brightness 2>/dev/null || true
    if [ '${RADIO_MODE}' != 'otbr' ]; then
        # Disable hardware flow control — Gecko Bootloader uses XON/XOFF.
        echo 0 > ${BRIDGE_SYSFS}/flow_control
        [ \"\$(cat ${BRIDGE_SYSFS}/flow_control 2>/dev/null)\" = '0' ]
    else
        # OTBR keeps flow control on for the probe: non-zero (1 hw, or 2 sw on
        # a board without RTS/CTS wiring where the bridge clamps hw to sw).
        [ \"\$(cat ${BRIDGE_SYSFS}/flow_control 2>/dev/null)\" != '0' ]
    fi
    [ \"\$(cat ${BRIDGE_SYSFS}/baud 2>/dev/null)\" = '${CURRENT_BAUD}' ]
    sleep 1
"

# v3.1 — pulse EFR32 nRST so the chip starts from a known-clean state before
# we probe it. Eliminates the 'stuck app / corrupt state' failure mode that
# v3.0.1's USF probe would otherwise hit. Requires the nrst_pulse sysfs knob
# (kernel >= 6.18 with v3.1 rtl8196e-uart-bridge driver) — best-effort: if
# the knob isn't there, fall through silently.
#
# BUT skip the pulse if the chip is ALREADY in the Gecko Bootloader: the user
# may have entered it deliberately (blmode_pulse, or a manual download-mode
# reboot), and an nRST pulse would reset the chip back into the application and
# undo that — exactly the failure reported in #130. The bootloader answers at
# 115200/no-flow, so probe there first and only pulse when it does NOT answer.
echo "Checking whether the chip is already in the Gecko Bootloader..."
ssh_gw "
    echo 115200 > ${BRIDGE_SYSFS}/baud
    echo 0 > ${BRIDGE_SYSFS}/flow_control
" >/dev/null 2>&1
sleep 0.3
if "$FLASHER" --device "socket://${GW_IP}:${GW_PORT}" \
        --probe-methods "bootloader:115200" probe 2>&1 | grep -qi "Detected.*bootloader"; then
    echo "  Chip is already in the Gecko Bootloader — skipping the nRST pulse."
else
    echo "Pulsing EFR32 nRST for clean pre-probe state..."
    ssh_gw "
        if [ -w ${BRIDGE_SYSFS}/nrst_pulse ]; then
            echo 1 > ${BRIDGE_SYSFS}/nrst_pulse
            sleep 1   # boot ROM + app init
        else
            echo '(nrst_pulse sysfs absent — skipping pre-flash reset)'
        fi
    "
fi

# v3.1 Z3-Router CLI fallback: when the chip is ALREADY running the router
# firmware, USF can't probe it (router speaks only its mini-CLI, not
# EZSP/CPC/Spinel/Gecko-BTL). The standard probe path will fail; we'll then
# send the router's `bootloader reboot` CLI command at 115200 to make the
# chip enter Gecko Bootloader. Defined as a function — only invoked from the
# error branch when the normal probe fails AND the target firmware is router.
router_cli_to_bootloader() {
    echo "Router-target fallback: sending CLI 'bootloader reboot' at 115200..."
    ssh_gw "
        echo 115200 > ${BRIDGE_SYSFS}/baud
        echo 0 > ${BRIDGE_SYSFS}/flow_control
        sleep 0.2
        # nc reads stdin (the printf), forwards to bridge, exits when stdin
        # closes. Wrap in a subshell + sleep so nc doesn't linger.
        ( printf '\\r\\nbootloader reboot\\r\\n'; sleep 0.5 ) | nc localhost ${GW_PORT} >/dev/null 2>&1 &
        NC_PID=\$!
        sleep 1
        kill \$NC_PID 2>/dev/null || true
        wait 2>/dev/null || true
        echo '  CLI command sent, bridge left at 115200/no-flow.'
    "
}

# Restore bridge to a sane state on every exit path.
#
# Important: this trap NEVER reboots the gateway. The reboot is explicit on
# the success path only (see end of script). On failure we leave the gateway
# alive so the user can investigate (`ssh root@gw dmesg`, `cat radio.conf`,
# etc.) before manually rebooting.
FLASH_OK=0
cleanup() {
    # Re-enable flow control: request hw (1); the bridge clamps it to sw on a
    # board without RTS/CTS wiring, so this restores the board-appropriate mode.
    ssh_gw "
        echo ${ORIG_BAUD} > ${BRIDGE_SYSFS}/baud 2>/dev/null || true
        echo 1 > ${BRIDGE_SYSFS}/flow_control 2>/dev/null || true
    " >/dev/null 2>&1 || true

    if [ "$FLASH_OK" != "1" ]; then
        echo "" >&2
        echo "Gateway state restored to baud=${ORIG_BAUD}, flow control re-enabled." >&2
        echo "Flash did not complete successfully. To reboot manually:" >&2
        echo "  ssh root@${GW_IP} reboot" >&2
    fi
    ssh_cleanup_multiplex
}
trap cleanup EXIT

if ! wait_for_port "$GW_IP" "$GW_PORT"; then
    echo "Error: bridge not reachable on ${GW_IP}:${GW_PORT}." >&2
    exit 1
fi
if [ "$RADIO_MODE" = "otbr" ]; then
    echo "Bridge ready at ${CURRENT_BAUD} baud, flow_control=1 (OTBR path)."
else
    echo "Bridge ready at ${CURRENT_BAUD} baud, flow_control=0."
fi
echo ""

# --- 3. Flash ---------------------------------------------------------------

if [ "${CONFIRM:-}" != "y" ]; then
    read -r -p "Flash $(basename "$FIRMWARE") to ${GW_IP}? [y/N] " confirm
    if [[ ! "$confirm" =~ ^[yY]$ ]]; then
        echo "Aborted."
        exit 0
    fi
fi

assert_bridge_idle

echo ""
echo "Flashing..."

# Helper: change bridge baud (no process restart needed).
set_bridge_baud() {
    local baud="$1"
    ssh_gw "
        echo ${baud} > ${BRIDGE_SYSFS}/baud
        [ \"\$(cat ${BRIDGE_SYSFS}/baud 2>/dev/null)\" = '${baud}' ]
    "
}

set_bridge_flow_control() {
    local value="$1"
    # On success the readback equals the request, with one exception: an hw
    # request (1) on a board that does not wire RTS/CTS, where the bridge clamps
    # to sw (2). So an ON request (non-zero) is satisfied by any non-zero
    # readback; an OFF request (0) must read back exactly 0 (0 is never clamped,
    # and the Gecko Bootloader Xmodem path genuinely needs all flow control off).
    if [ "$value" = "0" ]; then
        ssh_gw "
            echo 0 > ${BRIDGE_SYSFS}/flow_control
            [ \"\$(cat ${BRIDGE_SYSFS}/flow_control 2>/dev/null)\" = '0' ]
        "
    else
        ssh_gw "
            echo ${value} > ${BRIDGE_SYSFS}/flow_control
            [ \"\$(cat ${BRIDGE_SYSFS}/flow_control 2>/dev/null)\" != '0' ]
        "
    fi
}

# Probe selection. We always probe ALL four known protocols at the current
# baud, because the gateway-side `radio.conf` MODE doesn't fully constrain
# what the chip speaks: OT-RCP firmware can be used in Zigbee bridge mode
# (ZoH adapter, OTBR-on-host docker), so MODE=zigbee + chip=OT-RCP =
# legitimate. USF tries the methods in order and stops at the first match.

# Pre-check: is the chip already in Gecko Bootloader? Two common ways to
# get there: (a) a previous bootloader-only flash left the app slot empty,
# so enterBootloader() returns true via BADAPP at every boot; (b) the user
# manually rebooted into download mode. In both cases the chip is at
# 115200/no-flow and the running-app probe at the radio.conf baud will
# return nothing useful.
# probe_bootloader: is the chip sitting in the Gecko Bootloader right now? Prints
# its version on success (empty if USF logged none) and returns 0; returns 1 if
# the chip is not in the bootloader. This is the only way to learn the running
# bootloader version — it is not readable while the application runs.
probe_bootloader() {
    local log rc=1
    log=$(mktemp)
    set_bridge_baud 115200
    set_bridge_flow_control 0
    sleep 0.3
    "$FLASHER" --device "socket://${GW_IP}:${GW_PORT}" \
        --probe-methods "bootloader:115200" probe >"$log" 2>&1 || true
    if grep -qi "Detected.*bootloader" "$log"; then
        sed -nE "s/.*GECKO_BOOTLOADER, version '?([0-9]+\.[0-9]+\.[0-9]+)'?.*/\1/p" "$log" | tail -1
        rc=0
    fi
    rm -f "$log"
    return $rc
}

# Hardware bootloader entry (#123, #148). A board that wires an EFR32 pin to a
# SoC GPIO can drop the radio into its bootloader without asking the running
# application anything — which is the entire point: it works when the app is
# wedged, and when the app speaks a protocol USF cannot use to request bootloader
# entry (the Z3 Router, which otherwise costs a five-baud probe sweep).
#
# Boards with no such pin keep blmode_gpio at -1 and are skipped — the Lidl has
# no EFR32 pin routed to a SoC GPIO at all. And a radio running a bootloader
# built without GPIO activation simply reboots into its application: the probe
# below then fails and we fall through to the normal path, no worse off.
BLMODE_GPIO=$(ssh_gw "cat ${BRIDGE_SYSFS}/blmode_gpio 2>/dev/null" || true)
if [ -n "$BLMODE_GPIO" ] && [ "$BLMODE_GPIO" != "-1" ]; then
    echo "Hardware bootloader entry: pulsing blmode (GPIO ${BLMODE_GPIO})..."
    set_bridge_baud 115200
    set_bridge_flow_control 0
    ssh_gw "echo 1 > ${BRIDGE_SYSFS}/blmode_pulse" || \
        echo "  blmode pulse rejected — falling back to the in-band path." >&2
    wait_for_port "$GW_IP" "$GW_PORT"
fi

echo "Pre-check: is chip already in Gecko Bootloader?"
if BTL_RUNNING_VERSION=$(probe_bootloader); then
    echo "  Chip is in Gecko Bootloader (115200)${BTL_RUNNING_VERSION:+, version ${BTL_RUNNING_VERSION}} — uploading directly."
    PROBE="bootloader:115200"
else
    BTL_RUNNING_VERSION=""
    # Restore bridge to the running-app baud + flow_control for the
    # normal probe path.
    set_bridge_baud "$CURRENT_BAUD"
    if [ "$RADIO_MODE" = "otbr" ]; then
        set_bridge_flow_control 1
    fi
    PROBE="ezsp:${CURRENT_BAUD},cpc:${CURRENT_BAUD},spinel:${CURRENT_BAUD},bootloader:${CURRENT_BAUD}"
    echo "  Chip is running an app — probing at ${CURRENT_BAUD} baud."
fi

# A bootloader-only flash leaves the app slot empty. USF runs run_firmware()
# after the upload, which then fails with NoFirmwareError — that's expected
# success for this path. We tolerate it conditionally below.
IS_BOOTLOADER_FLASH=0
[ "$fw_choice" = "1" ] && IS_BOOTLOADER_FLASH=1

FLASH_LOG=$(mktemp)
trap 'rm -f "$FLASH_LOG"; cleanup' EXIT

# is_acceptable_failure: bootloader path tolerates NoFirmwareError as success.
# It means the app slot is empty, which is the expected outcome here — it says
# nothing about whether the bootloader itself was replaced. That is verified
# separately, after the flash (see the bootloader-only block below).
is_acceptable_failure() {
    [ "$IS_BOOTLOADER_FLASH" = "1" ] && grep -q "NoFirmwareError" "$FLASH_LOG"
}

# usf_filter: USF offers no way to skip the run_firmware() step it performs
# after every upload, so a bootloader-only flash always ends in an uncaught
# NoFirmwareError and spills a full Python traceback onto the terminal. The
# outcome is expected — the application slot really is empty — but it reads like
# a crash, and it is what people mistake for the whole story: it is the
# signature of the app being erased, not of the bootloader being replaced (#148).
#
# So collapse that one traceback into a single honest line — and only on the
# bootloader path. On an application flash a NoFirmwareError means the opposite:
# the app slot came out empty when it should hold the firmware we just wrote,
# i.e. a real failure, which is why is_acceptable_failure() gates on
# IS_BOOTLOADER_FLASH too. Calling that "expected" would be the same class of
# lie this whole commit exists to remove.
#
# Every other exception is passed through verbatim, on every path. In particular
# FailedToEnterBootloaderError stays visible: the script recovers from it, but it
# is also the signature of hardware flow control blocking bootloader entry
# (22-Backup-Flash-Restore/README.md), so it is not ours to silence. Swallowing
# errors nobody has looked at is exactly how the #148 failure stayed invisible.
#
# The unfiltered output still goes to $FLASH_LOG, which is what
# is_acceptable_failure() reads. awk always exits 0, so under `set -o pipefail`
# USF's own status still governs the pipeline.
usf_filter() {
    # Only a bootloader flash produces the expected traceback. On an application
    # flash there is nothing to collapse, and the awk below reads by newline —
    # which would hold USF's carriage-return progress bar until EOF, so the flash
    # would appear to hang and print only its result at the end (#148). Stream
    # those straight through instead: cat copies bytes as they arrive, so the
    # live bar shows exactly as it did before the traceback filter existed.
    if [ "$IS_BOOTLOADER_FLASH" != "1" ]; then
        cat
        return
    fi
    awk -v is_btl="$IS_BOOTLOADER_FLASH" '
        /^Traceback \(most recent call last\):/ { in_tb = 1; buf = $0 ORS; next }
        in_tb {
            buf = buf $0 ORS
            if ($0 ~ /^[A-Za-z_][A-Za-z_0-9.]*(Error|Exception):/) {
                in_tb = 0
                if (is_btl == "1" && $0 ~ /NoFirmwareError/) {
                    print "  (expected: a bootloader update leaves the application slot empty)"
                } else {
                    printf "%s", buf
                }
                buf = ""
                fflush()
            }
            next
        }
        { print; fflush() }
        END { if (in_tb) printf "%s", buf }
    '
}

# gbl_bootloader_version: the version *inside* a bootloader .gbl — the number
# the chip will actually compare against, as opposed to the one in the
# filename, which is just a claim. Layout: the 8-byte header tag and its 8-byte
# payload, then the bootloader tag (0xF50909F5) at offset 16, its length, and
# the version word (little-endian u32) at offset 24. The word packs
# major<<24 | minor<<16 | customer, with customer occupying the low 16 bits.
# Read with od — xxd is not installed by default on Debian (#147).
gbl_bootloader_version() {
    local f="$1" tag ver b0 b1 b2 b3
    tag=$(od -An -tx1 -j16 -N4 -v "$f" 2>/dev/null | tr -d ' \n')
    [ "$tag" = "f50909f5" ] || return 1
    ver=$(od -An -tx1 -j24 -N4 -v "$f" 2>/dev/null | tr -d ' \n')
    [ ${#ver} -eq 8 ] || return 1
    b0=$((0x${ver:0:2})); b1=$((0x${ver:2:2}))
    b2=$((0x${ver:4:2})); b3=$((0x${ver:6:2}))
    printf '%d.%d.%d\n' "$b3" "$b2" "$(( (b1 << 8) | b0 ))"
}

# btl_version_word: "2.4.3" -> the 32-bit word the chip actually compares
# (major<<24 | minor<<16 | customer). Comparing the dotted strings would be
# wrong: 2.4.10 sorts below 2.4.9 as text.
btl_version_word() {
    local v="$1" a b c
    a=${v%%.*}; v=${v#*.}
    b=${v%%.*}; c=${v#*.}
    case "${a}${b}${c}" in ''|*[!0-9]*) return 1 ;; esac
    echo $(( (a << 24) | (b << 16) | c ))
}

# --- Pre-flight: refuse a bootloader flash that cannot possibly install -------
#
# Requested by @hlyi (#148), and he is right that this belongs *before* the
# upload: the post-flash check further down tells the truth, but only once the
# application has already been erased. The Gecko bootloader accepts a stage-2
# image only if its version is strictly greater than the running one, and it
# stages the image inside application space on the way in — so a same-or-older
# .gbl costs you the application and changes nothing.
#
# The running bootloader version is only readable while the chip is *in* the
# bootloader. When it is (chip already there, or a blmode pulse put it there),
# we compare against the real thing. Otherwise the application is running and
# the only figure available is the one recorded in radio.conf, which is a hint,
# not proof — so it is labelled as such. Either way --force overrides.
#
# The two figures fail in opposite directions, so they are handled separately.
# An unreadable *image* version is disqualifying: this guard exists precisely
# because a bootloader flash is destructive before it is validated, and a file
# whose version cannot be read is not one we can reason about at all. Refuse.
# An unknown *running* version is merely unverifiable — it is the normal state
# of a chip whose bootloader was installed by hand, which never records one —
# so it warns and proceeds. Both used to fall through this block in silence,
# which handed back exactly the no-op-that-erases-the-app the guard is for.
if [ "$IS_BOOTLOADER_FLASH" = "1" ]; then
    BTL_IMAGE_VERSION=$(gbl_bootloader_version "$FIRMWARE" || true)
    BTL_VERSION_SOURCE="the chip"
    if [ -z "$BTL_RUNNING_VERSION" ]; then
        BTL_RUNNING_VERSION=$(ssh_gw "grep '^BOOTLOADER_VERSION=' /userdata/etc/radio.conf 2>/dev/null | tail -1 | cut -d= -f2" || true)
        BTL_VERSION_SOURCE="radio.conf (the chip is running an app, so its bootloader version cannot be read)"
    fi

    img_word=""
    if [ -n "$BTL_IMAGE_VERSION" ]; then
        img_word=$(btl_version_word "$BTL_IMAGE_VERSION" || echo "")
    fi
    run_word=""
    if [ -n "$BTL_RUNNING_VERSION" ]; then
        run_word=$(btl_version_word "$BTL_RUNNING_VERSION" || echo "")
    fi

    if [ -z "$img_word" ]; then
        if [ "$FORCE" = "1" ]; then
            echo "" >&2
            echo "Warning: cannot read a bootloader version out of" >&2
            echo "  ${FIRMWARE}" >&2
            echo "--force given: flashing it as a bootloader anyway." >&2
        else
            echo "" >&2
            echo "Refusing to flash: cannot read a bootloader version out of" >&2
            echo "  ${FIRMWARE}" >&2
            echo "" >&2
            echo "A Gecko bootloader .gbl carries a bootloader tag (0xF50909F5) and a version" >&2
            echo "word; this file has neither where they belong. It is most likely not a" >&2
            echo "bootloader image at all (an application .gbl?) or it is truncated." >&2
            echo "" >&2
            echo "Flashing it would erase the application slot to stage an image the chip then" >&2
            echo "declines in silence — the exact trade this guard exists to refuse. Check the" >&2
            echo "file, or pass --force if you know something it does not." >&2
            exit 1
        fi
    fi

    if [ -n "$img_word" ] && [ -z "$run_word" ]; then
        echo "" >&2
        echo "Note: cannot establish the bootloader version currently on the chip" >&2
        echo "(per ${BTL_VERSION_SOURCE}) — proceeding with ${BTL_IMAGE_VERSION} unverified." >&2
        echo "If the chip already runs ${BTL_IMAGE_VERSION} or newer it will decline the image" >&2
        echo "in silence and the application slot will be erased for nothing; reflash the" >&2
        echo "application afterwards if that happens." >&2
    fi

    if [ -n "$img_word" ] && [ -n "$run_word" ]; then
        if [ "$img_word" -le "$run_word" ]; then
            if [ "$FORCE" = "1" ]; then
                echo ""
                echo "Warning: bootloader ${BTL_IMAGE_VERSION} is not newer than ${BTL_RUNNING_VERSION}" >&2
                echo "(per ${BTL_VERSION_SOURCE}). --force given: flashing anyway. The chip will" >&2
                echo "decline the image and the application slot will be erased for nothing." >&2
            else
                echo "" >&2
                echo "Refusing to flash: bootloader ${BTL_IMAGE_VERSION} is not newer than the" >&2
                echo "${BTL_RUNNING_VERSION} already installed (per ${BTL_VERSION_SOURCE})." >&2
                echo "" >&2
                echo "The Gecko bootloader installs a stage-2 image only when its version is" >&2
                echo "strictly greater than the running one, and declines in silence otherwise —" >&2
                echo "after staging the image inside application space, which erases the app. So" >&2
                echo "this flash would cost you the radio firmware and change nothing (#148)." >&2
                echo "" >&2
                echo "If you really mean it (you rebuilt the bootloader and did not bump" >&2
                echo "BOARD_BTL_CUSTOMER in board.env, say), use SWD/J-Link, or pass --force and" >&2
                echo "expect to reflash the application afterwards." >&2
                exit 1
            fi
        fi
    fi
fi

# try_flash_at_baud: fallback used when the standard radio.conf/sysfs baud
# probe fails. This treats radio.conf as an operational hint, not proof of
# what firmware is actually running on the chip.
try_flash_at_baud() {
    local baud="$1"
    set_bridge_baud "$baud"
    set_bridge_flow_control 0
    : > "$FLASH_LOG"
    if "$FLASHER" --device "socket://${GW_IP}:${GW_PORT}" \
            --probe-methods "ezsp:${baud},cpc:${baud},spinel:${baud},bootloader:${baud}" \
            flash --firmware "$FIRMWARE" 2>&1 | tee "$FLASH_LOG" | usf_filter; then
        return 0
    fi
    if is_acceptable_failure; then
        return 0  # bootloader: NoFirmwareError == success
    fi
    if grep -q "FailedToEnterBootloaderError" "$FLASH_LOG"; then
        # USF detected the running app and sent enter_bootloader;
        # chip is now in Gecko Bootloader at 115200/no-flow. Same dance as
        # the main path: retry via bootloader:115200 only.
        echo ""
        echo "${baud} fallback: app detected, EFR32 entered bootloader — retrying via bootloader:115200..."
        set_bridge_baud 115200
        set_bridge_flow_control 0
        : > "$FLASH_LOG"
        if "$FLASHER" --device "socket://${GW_IP}:${GW_PORT}" \
                --probe-methods "bootloader:115200" \
                flash --firmware "$FIRMWARE" 2>&1 | tee "$FLASH_LOG" | usf_filter; then
            return 0
        fi
        if is_acceptable_failure; then
            return 0
        fi
    fi
    return 1
}

assert_bridge_idle

# First attempt: probe the running app + flash.
if "$FLASHER" --device "socket://${GW_IP}:${GW_PORT}" \
        --probe-methods "$PROBE" \
        flash --firmware "$FIRMWARE" 2>&1 | tee "$FLASH_LOG" | usf_filter; then
    : # success
elif is_acceptable_failure; then
    : # bootloader: NoFirmwareError == success
elif grep -q "FailedToEnterBootloaderError" "$FLASH_LOG"; then
    # USF detected the running app and sent enter_bootloader. The chip is
    # now in Gecko Bootloader at 115200/no-flow but the bridge is still at
    # the app baud. Switch the bridge and retry via bootloader:115200.
    echo ""
    echo "Firmware detected — EFR32 entered bootloader. Switching bridge to 115200..."
    set_bridge_baud 115200
    set_bridge_flow_control 0
    : > "$FLASH_LOG"
    if "$FLASHER" --device "socket://${GW_IP}:${GW_PORT}" \
            --probe-methods "bootloader:115200" \
            flash --firmware "$FIRMWARE" 2>&1 | tee "$FLASH_LOG" | usf_filter; then
        : # success
    elif is_acceptable_failure; then
        : # bootloader: NoFirmwareError == success
    else
        echo "Flash via bootloader failed." >&2
        exit 1
    fi
else
    # Standard probe failed. Sweep all firmware bauds we ship before falling
    # back to the router CLI. This covers stale radio.conf in both directions:
    # config says 115200 while the chip is really at 460800, and vice versa.

    FALLBACK_OK=0
    FALLBACK_BAUDS="115200 230400 460800 691200 892857"
    echo ""
    echo "Standard probe failed — sweeping fallback bauds: ${FALLBACK_BAUDS}"
    for fallback_baud in $FALLBACK_BAUDS; do
        [ "$fallback_baud" = "$CURRENT_BAUD" ] && continue
        echo ""
        echo "Trying fallback baud ${fallback_baud}..."
        if try_flash_at_baud "$fallback_baud"; then
            FALLBACK_OK=1
            break
        fi
    done

    if [ "$FALLBACK_OK" = "0" ]; then
        echo ""
        echo "Baud sweep failed — trying router CLI fallback (chip may be running the Z3 Router)."
        router_cli_to_bootloader
        : > "$FLASH_LOG"
        if "$FLASHER" --device "socket://${GW_IP}:${GW_PORT}" \
                --probe-methods "bootloader:115200" \
                flash --firmware "$FIRMWARE" 2>&1 | tee "$FLASH_LOG" | usf_filter; then
            FALLBACK_OK=1
        elif is_acceptable_failure; then
            FALLBACK_OK=1
        fi
    fi

    if [ "$FALLBACK_OK" = "0" ]; then
        echo "" >&2
        echo "Error: chip not responding to any of:" >&2
        echo "  * standard probe at ${CURRENT_BAUD} baud" >&2
        echo "  * fallback baud sweep: ${FALLBACK_BAUDS}" >&2
        echo "  * router CLI fallback (Z3 Router)" >&2
        echo "" >&2
        echo "Likely causes:" >&2
        echo "  * The chip's app firmware is corrupted / non-responsive." >&2
        echo "    Power-cycle the gateway and retry; if it still fails, the chip" >&2
        echo "    needs J-Link recovery (header J1, see 22-Backup-Flash-Restore/)." >&2
        echo "  * The chip is running an exotic firmware at an unsupported baud" >&2
        echo "    or a protocol USF cannot use to enter the Gecko Bootloader." >&2
        exit 1
    fi
fi

# Extract the Gecko Bootloader version from USF's flash log. USF emits a
# line like:
#   INFO Detected bootloader version '2.4.2'
# only on the path where the chip is *already* in the bootloader at probe
# time (or when --bootloader-reset wired up a GPIO/RTS-DTR external entry).
# On the common app→bootloader transition path the version is detected
# internally but never logged at INFO, so the grep below finds nothing —
# the trailing `|| true` keeps `set -euo pipefail` from aborting the
# script and dropping the radio.conf write that follows. Issue #96.
BOOTLOADER_VERSION_DETECTED=$(grep -oE "Detected bootloader version '[^']+'" "$FLASH_LOG" 2>/dev/null \
    | tail -1 | sed -E "s/^Detected bootloader version '([^']+)'$/\1/" || true)
if [ -n "$BOOTLOADER_VERSION_DETECTED" ] && [ "$IS_BOOTLOADER_FLASH" != "1" ]; then
    echo "Gecko Bootloader version: ${BOOTLOADER_VERSION_DETECTED}"
fi

rm -f "$FLASH_LOG"

# --- Bootloader-only flash: verify, then stop ------------------------------
#
# A completed upload does NOT mean the bootloader changed. The Gecko bootloader
# installs a stage-2 image only when it is strictly newer than the running one:
#
#     if (imageProps->bootloaderVersion > bootload_getBootloaderVersion())
#         bootload_commitBootloaderUpgrade(...)      // btl_comm_xmodem_common.c
#
# There is no else branch — a same-or-older image is declined without a word.
# And it is declined *after* the damage: the image is staged at
# BTL_UPGRADE_LOCATION (0x8000), which lives inside application space, and the
# first application page is erased as the first bytes arrive. So the upload
# reports success, the application is gone, and the old bootloader is still in
# place. Reported by @hlyi on a Sengled G4 (#148); the Lidl has the same layout
# and the same trap.
#
# Hence: never trust the upload, and never trust the filename. Read back what
# is actually running and compare it with what is actually inside the .gbl.
if [ "$IS_BOOTLOADER_FLASH" = "1" ]; then
    BTL_IMAGE_VERSION=$(gbl_bootloader_version "$FIRMWARE" || true)
    BTL_RUNNING_VERSION=$(probe_bootloader || true)

    if [ -z "$BTL_RUNNING_VERSION" ]; then
        echo ""
        echo "Warning: could not read the bootloader version back from the chip," >&2
        echo "so this flash is unverified. Confirm the bootloader by hand before" >&2
        echo "relying on it." >&2
    elif [ -n "$BTL_IMAGE_VERSION" ] && [ "$BTL_IMAGE_VERSION" != "$BTL_RUNNING_VERSION" ]; then
        echo "" >&2
        echo "BOOTLOADER NOT UPDATED — the chip still runs ${BTL_RUNNING_VERSION}, the image was ${BTL_IMAGE_VERSION}." >&2
        echo "" >&2
        echo "The Gecko bootloader installs a new stage 2 only when its version is strictly" >&2
        echo "greater than the running one, and it declines in silence. The image was staged" >&2
        echo "inside application space on the way in, so THE APPLICATION SLOT IS NOW EMPTY." >&2
        echo "" >&2
        echo "Recover the radio: flash an application (ncp / rcp / otrcp / router)." >&2
        echo "To really replace the bootloader: build one with a higher version (BTL_CUSTOMER" >&2
        echo "in 2-Zigbee-Radio-Silabs-EFR32/23-Bootloader-UART-Xmodem/build_bootloader.sh)," >&2
        echo "or flash the combined .s37 over SWD/J-Link." >&2
        exit 1
    fi

    if [ -n "$BTL_RUNNING_VERSION" ]; then
        echo "Gecko Bootloader version: ${BTL_RUNNING_VERSION} (verified on the chip)"
        ssh_gw "
            mkdir -p /userdata/etc
            touch /userdata/etc/radio.conf
            sed -i '/^BOOTLOADER_VERSION=/d' /userdata/etc/radio.conf
            echo 'BOOTLOADER_VERSION=${BTL_RUNNING_VERSION}' >> /userdata/etc/radio.conf
        " 2>/dev/null || true
    fi
    echo ""
    echo "Bootloader flashed successfully. The app slot is now empty."
    echo "Flash an application firmware with one of:"
    echo "  ${BOARD_PREFIX}./flash_efr32.sh -y ncp     ${GW_IP:+--gateway $GW_IP}"
    echo "  ${BOARD_PREFIX}./flash_efr32.sh -y rcp     ${GW_IP:+--gateway $GW_IP}"
    echo "  ${BOARD_PREFIX}./flash_efr32.sh -y otrcp   ${GW_IP:+--gateway $GW_IP}"
    echo "  ${BOARD_PREFIX}./flash_efr32.sh -y router  ${GW_IP:+--gateway $GW_IP}"
    FLASH_OK=1
    exit 0
fi

# --- 4. Configure radio mode + cleanup -------------------------------------

# Ensure radio.conf matches the flashed firmware AND its baud, so the right
# daemon starts on reboot at the right speed AND so an offline reader can
# tell exactly which app is on the chip without probing.
#
# radio.conf keys (multi-key file, we touch only the ones we own):
#   FIRMWARE=<name>          — what app is in the EFR32 application slot:
#                              ncp | rcp | otrcp | router. NOT 'bootloader'
#                              (bootloader is a runtime mode, not an app).
#                              A bootloader-only flash leaves this untouched.
#   FIRMWARE_VERSION=<ver>   — when the GBL filename embeds it (NCP, Router).
#                              Absent for RCP/OT-RCP — version is host-side
#                              (zigbeed for RCP, ot-br-posix for OT-RCP).
#   FIRMWARE_BAUD=<baud>     — the chip's UART baud, set at flash time. Single
#                              source of truth: S50uart_bridge / S70otbr both
#                              read this (working link forces both ends equal).
#   FIRMWARE_FLOW_CTRL=<fc>  — the chip's flow-control mode (hw|sw|none), from
#                              the selected board's board.env (BOARD_UART_FLOW,
#                              #141). S50uart_bridge writes it to the bridge's
#                              flow_control knob; S70otbr omits uart-flow-control
#                              from the spinel URL for none|sw (presence flag,
#                              #142). RCP flashes on a sw board record none —
#                              the chip build is clamped to no flow (#143).
#   BOOTLOADER_VERSION=<ver> — Gecko Bootloader Stage-2 version reported by
#                              USF during the flash (e.g. '2.4.2'). Updated
#                              on EVERY successful flash (USF transits the
#                              bootloader to upload the GBL — bootloader-only
#                              AND app flashes both refresh this).
#   MODE=otbr                — drives OTBR vs Zigbee path (set when fw=OT-RCP).
#
# Per-firmware mapping:
#   1 (Bootloader)  → don't touch radio.conf (we only updated the bootloader,
#                     not the application; existing config still applies)
#   2 (NCP)         → FIRMWARE=ncp + FIRMWARE_VERSION=<v> + FIRMWARE_BAUD=<v>
#   3 (RCP)         → FIRMWARE=rcp + FIRMWARE_BAUD=<v>
#   4 (OT-RCP)      → FIRMWARE=otrcp + FIRMWARE_BAUD=<v> + MODE=otbr
#   5 (Router)      → FIRMWARE=router + FIRMWARE_VERSION=<v> + FIRMWARE_BAUD=<v>
#   All app flashes (2-5) also record FIRMWARE_FLOW_CTRL=<board.env flow> —
#   except RCP on a sw board, recorded as none: build_rcp.sh clamps the chip
#   to no flow control (CPC has no XON/XOFF), and the key must describe what
#   is actually on the chip so S50uart_bridge arms the bridge to match.
#
# Legacy host-side keys (BRIDGE_BAUD, OTBR_BAUD) are stripped on every flash
# so old configs converge on the single FIRMWARE_BAUD truth.

# Extract FIRMWARE_VERSION from the GBL filename when it embeds one
# (currently NCP and Router: ncp-uart-hw-7.5.1-460800-hw.gbl, z3-router-7.5.1-115200-hw.gbl).
# The version is the field after the fw-name prefix; match it independently of
# the trailing baud/flow/board fields (#145 — a fixed baud-is-last anchor used
# to leave the version empty for board-suffixed builds).
case "$(basename "$FIRMWARE")" in
    ncp-uart-hw-*-*.gbl|z3-router-*-*.gbl)
        FW_VERSION_DETECTED=$(basename "$FIRMWARE" | sed -E 's/^(ncp-uart-hw|z3-router)-([0-9.]+)-.*/\2/')
        ;;
    *)
        FW_VERSION_DETECTED=
        ;;
esac

# Flow-control value recorded in radio.conf. Defaults to the board's flow
# (chip truth for NCP/OT-RCP/Router builds); the RCP case overrides it below.
RADIO_FLOW="$BOARD_FLOW"

case "$fw_choice" in
    1)  # Bootloader-only flash — leave radio.conf alone
        FIRMWARE_NAME=
        FIRMWARE_VER=
        MODE_LINE=
        DAEMON_MSG="(no daemon change — bootloader-only flash)"
        ORIG_BAUD="$CURRENT_BAUD"
        ;;
    4)  # OT-RCP → OTBR mode at chosen baud
        FIRMWARE_NAME=otrcp
        FIRMWARE_VER=
        MODE_LINE=MODE=otbr
        DAEMON_MSG="otbr-agent (S70otbr) at ${fw_baud} baud"
        ORIG_BAUD="$fw_baud"
        ;;
    3)  # RCP-UART-HW → Zigbee via in-kernel bridge at chosen baud
        FIRMWARE_NAME=rcp
        FIRMWARE_VER=
        MODE_LINE=
        DAEMON_MSG="in-kernel UART bridge on TCP:8888 at ${fw_baud} baud"
        ORIG_BAUD="$fw_baud"
        # build_rcp.sh clamps a sw board to NO flow control (CPC has no
        # XON/XOFF, and its binary frames don't escape 0x11/0x13 — a bridge
        # running sw against a CPC chip would eat in-frame bytes as flow
        # control). Record the chip truth so the bridge arms to match.
        [ "$BOARD_FLOW" = "sw" ] && RADIO_FLOW=none
        ;;
    2)  # NCP-UART-HW → Zigbee via in-kernel bridge at chosen baud
        FIRMWARE_NAME=ncp
        FIRMWARE_VER="$FW_VERSION_DETECTED"
        MODE_LINE=
        DAEMON_MSG="in-kernel UART bridge on TCP:8888 at ${fw_baud} baud"
        ORIG_BAUD="$fw_baud"
        ;;
    5)  # Z3-Router → bridge stays armed at router CLI baud
        FIRMWARE_NAME=router
        FIRMWARE_VER="$FW_VERSION_DETECTED"
        MODE_LINE=
        DAEMON_MSG="in-kernel UART bridge on TCP:8888 at ${fw_baud} baud (router CLI)"
        ORIG_BAUD="$fw_baud"
        ;;
esac

# Persist FIRMWARE / FIRMWARE_VERSION / FIRMWARE_BAUD / FIRMWARE_FLOW_CTRL /
# BOOTLOADER_VERSION / MODE to /userdata/etc/radio.conf so init scripts arm at
# the right speed on next boot AND a human / future script can tell what's on
# the chip (both app slot AND Stage 2 bootloader) without probing. Legacy
# host-side keys (BRIDGE_BAUD, OTBR_BAUD) are stripped here too so old
# configs converge.
# Skipped for bootloader-only flash where FIRMWARE_NAME is empty (that path
# writes only BOOTLOADER_VERSION earlier and exits).
if [ -n "$FIRMWARE_NAME" ]; then
    # Hard-fail on radio.conf write errors. The chip is now flashed; if we
    # don't update radio.conf, S50uart_bridge / S70otbr will arm at the
    # WRONG baud on next boot and the gateway will appear bricked. A silent
    # "|| true" here used to mask SSH auth/transport failures (issue #93,
    # @skinkie) — the user saw "Flash complete" while radio.conf was stale.
    if ! ssh_gw "
        mkdir -p /userdata/etc
        touch /userdata/etc/radio.conf
        # Strip every key we own, then re-append in canonical order.
        sed -i '/^FIRMWARE=/d;/^FIRMWARE_VERSION=/d;/^FIRMWARE_BAUD=/d;/^FIRMWARE_FLOW_CTRL=/d;/^BOOTLOADER_VERSION=/d;/^MODE=/d;/^BRIDGE_BAUD=/d;/^OTBR_BAUD=/d' /userdata/etc/radio.conf
        # Use 'if' (not '[ -n x ] && echo') for the optional keys: a false
        # short-circuit on the LAST line would make the whole brace group —
        # and thus this SSH command — exit non-zero, which the caller misreads
        # as a failed radio.conf write. RCP/NCP/Router flashes have an empty
        # MODE_LINE, so '[ -n '' ] && echo' was tripping that false negative.
        {
            echo 'FIRMWARE=${FIRMWARE_NAME}'
            if [ -n '${FIRMWARE_VER}' ]; then echo 'FIRMWARE_VERSION=${FIRMWARE_VER}'; fi
            echo 'FIRMWARE_BAUD=${fw_baud}'
            echo 'FIRMWARE_FLOW_CTRL=${RADIO_FLOW}'
            if [ -n '${BOOTLOADER_VERSION_DETECTED}' ]; then echo 'BOOTLOADER_VERSION=${BOOTLOADER_VERSION_DETECTED}'; fi
            if [ -n '${MODE_LINE}' ]; then echo '${MODE_LINE}'; fi
        } >> /userdata/etc/radio.conf
    "; then
        echo "" >&2
        echo "Error: failed to write /userdata/etc/radio.conf over SSH." >&2
        echo "The EFR32 is flashed (FIRMWARE=${FIRMWARE_NAME} @ ${fw_baud} baud)" >&2
        echo "but the gateway-side bridge will arm at the WRONG baud on next" >&2
        echo "boot. Fix it manually with one of:" >&2
        echo "" >&2
        echo "  ssh root@${GW_IP} \"echo FIRMWARE=${FIRMWARE_NAME} >  /userdata/etc/radio.conf\"" >&2
        echo "  ssh root@${GW_IP} \"echo FIRMWARE_BAUD=${fw_baud}    >> /userdata/etc/radio.conf\"" >&2
        echo "  ssh root@${GW_IP} \"echo FIRMWARE_FLOW_CTRL=${RADIO_FLOW} >> /userdata/etc/radio.conf\"" >&2
        if [ -n "${MODE_LINE}" ]; then
            echo "  ssh root@${GW_IP} \"echo ${MODE_LINE}             >> /userdata/etc/radio.conf\"" >&2
        fi
        echo "" >&2
        echo "Or re-run this script (the chip is already on the new firmware," >&2
        echo "the script will detect the running app and only update radio.conf)." >&2
        # Don't reboot — let cleanup() print its recovery hint and exit.
        exit 1
    fi
fi

echo ""
echo "Flash complete."

# Mark success so the cleanup trap doesn't print the "did not complete" hint.
FLASH_OK=1

if [ -n "${NO_REBOOT:-}" ]; then
    echo "Skipping reboot (--no-reboot). Apply the new firmware with:"
    echo "  ssh root@${GW_IP} reboot"
    echo "Done — ${DAEMON_MSG} will start on next boot."
else
    echo "Rebooting gateway..."
    ssh_gw "reboot" >/dev/null 2>&1 || true
    echo "Done — ${DAEMON_MSG} will start automatically."
fi
