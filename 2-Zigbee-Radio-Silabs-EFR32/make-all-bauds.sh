#!/bin/bash
# make-all-bauds.sh — Build a board's committed firmware × baud matrix
#
# Iterates over the firmware × baud combinations the project commits prebuilts
# for, calling each per-firmware build_*.sh with the chosen baud as a positional
# argument.
#
# Matrix — lidl, the reference board (per CHANGELOG v3.0.0 max-tested values):
#   NCP-UART-HW : 115200, 230400, 460800, 691200, 892857
#   RCP-UART-HW : 460800                            (the only RCP baud in use)
#   OT-RCP      : 460800                            (otbr-agent ceiling)
#   Z3-Router   : 115200                            (text CLI only)
# Total: 8 GBLs.
#
# Another board keeps its matrix in boards/<board>/board.env — BOARD_NCP_BAUDS,
# BOARD_RCP_BAUDS, BOARD_OT_RCP_BAUDS, BOARD_ROUTER_BAUDS — and any key it
# leaves out falls back to the row above; a key set to "" means the board
# commits no prebuilt for that firmware. The Sengled G4 commits 3 GBLs:
# NCP 115200, OT-RCP 230400, Router 115200 (230400 being its measured
# operating point, #134/#142); its RCP is build-it-yourself. The Gecko bootloader is not part of
# this matrix: it carries no baud (build_bootloader.sh builds it in one shot).
#
# Output: <firmware-dir>/firmware/<base>-<BAUD>-<flow>[-<driver>][-<board>].gbl
#         (and .s37) — the flow/driver/board fields are derived here exactly as
#         the build scripts derive them (#145).
#
# Usage:
#   ./make-all-bauds.sh              # Build everything missing (idempotent)
#   ./make-all-bauds.sh --force      # Rebuild everything from scratch
#   ./make-all-bauds.sh --list       # Print what would be built, exit
#   ./make-all-bauds.sh --help       # Show this help
#
#   BOARD=sengled-e39-g8c ./make-all-bauds.sh    # another board's matrix
#
# BOARD= (default lidl) selects the board, as in build_efr32.sh and every
# per-firmware build script; it is exported so those scripts see it.
#
# Power users wanting a single non-matrix variant should call the per-firmware
# build script directly:
#   ./24-NCP-UART-HW/build_ncp.sh 921600
#
# J. Nilo - April 2026; BOARD= support July 2026

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# ----- board selection -----
BOARD="${BOARD:-lidl}"
BOARD_ENV="${SCRIPT_DIR}/boards/${BOARD}/board.env"
if [ ! -f "${BOARD_ENV}" ]; then
    echo "Error: unknown BOARD='${BOARD}' (no ${BOARD_ENV})" >&2
    echo "Available boards: $(cd "${SCRIPT_DIR}/boards" && ls -d */ 2>/dev/null | tr -d /)" >&2
    exit 1
fi
# shellcheck source=/dev/null
. "${BOARD_ENV}"
export BOARD

# ----- matrix (board.env overrides the reference row, key by key) -----
NCP_BAUDS="${BOARD_NCP_BAUDS:-115200 230400 460800 691200 892857}"
# RCP uses "-" rather than ":-": an explicitly empty BOARD_RCP_BAUDS means
# "no committed RCP" (Sengled G4), not "fall back to the reference row".
RCP_BAUDS="${BOARD_RCP_BAUDS-460800}"
OT_RCP_BAUDS="${BOARD_OT_RCP_BAUDS:-460800}"
ROUTER_BAUDS="${BOARD_ROUTER_BAUDS:-115200}"

# ----- filename fields, derived as the build scripts derive them (#145) -----
# <flow> is the board's flow-control type — except for RCP, where sw is clamped
# to none because CPC has no software flow control (the framing does not escape
# XON/XOFF). OT-RCP additionally carries the UART backend its flow mode selects.
# Every non-lidl artefact ends with the board suffix.
case "${BOARD_UART_FLOW:-}" in
    hw|sw|none) ;;
    *)
        echo "Error: BOARD_UART_FLOW='${BOARD_UART_FLOW:-}' in ${BOARD_ENV} — expected hw, sw or none." >&2
        exit 1
        ;;
esac
FLOW="${BOARD_UART_FLOW}"
if [ "${FLOW}" = "sw" ]; then
    RCP_FLOW="none"
    OT_RCP_DRIVER="iostream"
else
    RCP_FLOW="${FLOW}"
    OT_RCP_DRIVER="uartdrv"
fi
if [ "${BOARD}" = "lidl" ]; then BOARD_SUFFIX=""; else BOARD_SUFFIX="-${BOARD}"; fi

# ----- arg parsing -----
FORCE=0
LIST_ONLY=0
case "${1:-}" in
    --force|-f) FORCE=1 ;;
    --list|-l)  LIST_ONLY=1 ;;
    --help|-h)
        sed -n '2,41p' "$0"
        exit 0
        ;;
    "")  ;;
    *)
        echo "Error: unknown argument '$1'. Use --help." >&2
        exit 1
        ;;
esac

# ----- helpers -----
# build <fw_dir> <build_script> <gbl_basename_template> <bauds...>
# gbl_basename_template uses %BAUD% as placeholder.
build_one() {
    local fw_dir="$1"
    local build_script="$2"
    local gbl_template="$3"
    shift 3
    local bauds="$*"

    for baud in $bauds; do
        local out="${SCRIPT_DIR}/${fw_dir}/firmware/${gbl_template//%BAUD%/$baud}.gbl"

        if [ "$LIST_ONLY" = "1" ]; then
            if [ -f "$out" ]; then
                echo "  [exists] $out"
            else
                echo "  [build ] $out"
            fi
            continue
        fi

        if [ "$FORCE" = "0" ] && [ -f "$out" ]; then
            echo "  [skip] $out (already built; use --force to rebuild)"
            continue
        fi

        echo
        echo "============================================================"
        echo "Building ${fw_dir} at ${baud} baud..."
        echo "============================================================"
        ( cd "${SCRIPT_DIR}/${fw_dir}" && ./"${build_script}" "${baud}" )
        if [ ! -f "$out" ]; then
            echo "ERROR: expected output $out not produced." >&2
            exit 1
        fi
        echo "  -> $out"
    done
}

# ----- detect EmberZNet version (NCP and Router filenames embed it) -----
EMBER_CONFIG=""
for c in "${PROJECT_ROOT:-${SCRIPT_DIR}/..}/silabs-tools/gecko_sdk/protocol/zigbee/stack/config/config.h" \
         "${SCRIPT_DIR}/../silabs-tools/gecko_sdk/protocol/zigbee/stack/config/config.h"; do
    [ -f "$c" ] && EMBER_CONFIG="$c" && break
done
if [ -n "$EMBER_CONFIG" ]; then
    EMBER_MAJOR=$(grep '#define EMBER_MAJOR_VERSION' "$EMBER_CONFIG" | awk '{print $3}')
    EMBER_MINOR=$(grep '#define EMBER_MINOR_VERSION' "$EMBER_CONFIG" | awk '{print $3}')
    EMBER_PATCH=$(grep '#define EMBER_PATCH_VERSION' "$EMBER_CONFIG" | awk '{print $3}')
    EMBERZNET_VERSION="${EMBER_MAJOR}.${EMBER_MINOR}.${EMBER_PATCH}"
else
    echo "WARNING: could not locate Gecko SDK to read EmberZNet version" >&2
    EMBERZNET_VERSION="unknown"
fi

# ----- run -----
if [ "$LIST_ONLY" = "1" ]; then
    echo "Matrix (BOARD=${BOARD}, flow=${FLOW}):"
else
    echo "Board: ${BOARD} (flow=${FLOW})"
fi

# Name templates carry the flow-control (+OT-RCP driver) and board fields
# (#145), derived above from board.env. For lidl they resolve to the historical
# literals (flow hw, driver uartdrv, no suffix), so its matrix is unchanged.
build_one "24-NCP-UART-HW"  "build_ncp.sh"     "ncp-uart-hw-${EMBERZNET_VERSION}-%BAUD%-${FLOW}${BOARD_SUFFIX}"       $NCP_BAUDS
build_one "25-RCP-UART-HW"  "build_rcp.sh"     "rcp-uart-802154-%BAUD%-${RCP_FLOW}${BOARD_SUFFIX}"                    $RCP_BAUDS
build_one "26-OT-RCP"       "build_ot_rcp.sh"  "ot-rcp-%BAUD%-${FLOW}-${OT_RCP_DRIVER}${BOARD_SUFFIX}"                $OT_RCP_BAUDS
build_one "27-Router"       "build_router.sh"  "z3-router-${EMBERZNET_VERSION}-%BAUD%-${FLOW}${BOARD_SUFFIX}"         $ROUTER_BAUDS

if [ "$LIST_ONLY" = "0" ]; then
    echo
    echo "============================================================"
    echo "Matrix complete. GBLs in <firmware>/firmware/."
    echo "============================================================"
fi
