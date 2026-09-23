#!/bin/bash
# build_rcp.sh — Build RCP 802.15.4 firmware for EFR32MG1B232F256GM48
#
# This builds an 802.15.4 RCP firmware compatible with zigbeed and cpcd.
# Uses CPC Protocol v5 (native GSDK 4.5.0) for optimal stability.
#
# Prerequisites:
#   - slc (Silicon Labs CLI) in PATH
#   - arm-none-eabi-gcc in PATH
#   - GECKO_SDK environment variable set
#
# Usage:
#   ./build_rcp.sh                  # Build firmware at default baud (460800)
#   ./build_rcp.sh 230400           # Build at lower baud (cpcd POSIX-supported only)
#   ./build_rcp.sh clean            # Clean build directory
#   ./build_rcp.sh --help           # Show this help
#
# NOTE: cpcd validates baud against the POSIX standard list and rejects
#       non-standard values like 691200 / 892857. RCP is therefore capped
#       at 460800 in practice.
#
# Output:
#   firmware/rcp-uart-802154-<BAUD>-<flow>.gbl  (ready to flash via UART/Xmodem)
#   firmware/rcp-uart-802154-<BAUD>-<flow>.s37  (for J-Link/SWD flashing)
#   <flow> = hw|none (#145). CPC has no software flow control, so a sw board is
#   built and named as none. Non-lidl BOARD= builds add a -<board> suffix last.
#
# J. Nilo - December 2025; baud parameter added April 2026; BOARD= support July 2026 (#143)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
OUTPUT_DIR="${SCRIPT_DIR}/firmware"
PATCHES_DIR="${SCRIPT_DIR}/patches"

# Project root (for auto-detecting silabs-tools)
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SILABS_TOOLS_DIR="${PROJECT_ROOT}/silabs-tools"

# Board selection (BOARD=lidl by default). board.env packages the chip OPN
# and the UART routing to the RTL8196E; see ../boards/README.md.
#
# Flow-control note: CPC supports only two modes — RTS/CTS or none (the
# framing has no XON/XOFF escaping, so software flow does not exist here).
# A board.env with BOARD_UART_FLOW=sw is therefore clamped to *none*. The
# chip's flow partner is the gateway's in-kernel UART bridge (cpcd connects
# to it over TCP — bus_type: TCP — so cpcd's uart_hardflow never applies);
# flash_efr32.sh records FIRMWARE_FLOW_CTRL=none for this build so the
# bridge arms to match.
BOARDS_DIR="${SCRIPT_DIR}/../boards"
BOARD="${BOARD:-lidl}"
BOARD_ENV="${BOARDS_DIR}/${BOARD}/board.env"
if [ ! -f "${BOARD_ENV}" ]; then
    echo "Error: unknown BOARD='${BOARD}' (no ${BOARD_ENV})" >&2
    echo "Available boards: $(cd "${BOARDS_DIR}" && ls -d */ 2>/dev/null | tr -d /)" >&2
    exit 1
fi
# shellcheck source=/dev/null
. "${BOARD_ENV}"
. "${BOARDS_DIR}/lib_uart_config.sh"

# Target chip — from the selected board.
TARGET_DEVICE="${BOARD_TARGET_DEVICE:?board.env must set BOARD_TARGET_DEVICE}"
PROJECT_NAME="rcp-uart-802154"

# Non-default boards get a filename suffix so their artefacts don't overwrite
# the lidl reference firmware (which keeps its historical name).
[ "${BOARD}" = "lidl" ] && BOARD_SUFFIX="" || BOARD_SUFFIX="-${BOARD}"

# CPC flow-control token: hw → RTS/CTS, none AND sw → none (see note above).
RCP_FLOW_TOK="$(flow_control_token usartHwFlowControlCtsAndRts usartHwFlowControlNone usartHwFlowControlNone)" || exit 1
if [ "${BOARD_UART_FLOW}" = "sw" ]; then
    echo "NOTE: BOARD_UART_FLOW=sw — CPC has no software flow control; building"
    echo "      with flow control NONE. flash_efr32.sh records it as such, so"
    echo "      the gateway's UART bridge (cpcd's TCP peer) arms to match."
    echo ""
fi

# Default baud — historical RCP default. cpcd POSIX cap is 460800.
DEFAULT_BAUD=460800
TESTED_BAUDS="115200 230400 460800"

case "${1:-}" in
    clean)
        echo "Cleaning build directory..."
        rm -rf "${BUILD_DIR}"
        echo "Done."
        exit 0
        ;;
    --help|-h)
        sed -n '2,18p' "$0"
        echo
        echo "Tested bauds: ${TESTED_BAUDS}"
        echo "Default baud: ${DEFAULT_BAUD}"
        exit 0
        ;;
    "")
        BAUD=${DEFAULT_BAUD}
        ;;
    *)
        BAUD="$1"
        ;;
esac

if ! echo "${BAUD}" | grep -qE '^[0-9]+$'; then
    echo "Error: invalid baud '${BAUD}' (must be a positive integer)" >&2
    echo "Tested bauds: ${TESTED_BAUDS}" >&2
    exit 1
fi
case " ${TESTED_BAUDS} " in
    *" ${BAUD} "*) ;;
    *)
        echo "WARNING: baud ${BAUD} is outside the tested set {${TESTED_BAUDS}}."
        echo "         RCP firmware at non-POSIX bauds will likely be rejected"
        echo "         by cpcd at runtime. Build will proceed anyway."
        ;;
esac

echo "========================================="
echo "  RCP 802.15.4 Firmware Builder"
echo "  Board:  ${BOARD} (${BOARD_NAME})"
echo "  Target: ${TARGET_DEVICE}"
echo "  CPC Protocol: v5 (GSDK 4.5.0)"
echo "  Baud:   ${BAUD}"
echo "========================================="
echo ""

# =========================================
# Auto-detect silabs-tools in project directory
# =========================================
if [ -d "${SILABS_TOOLS_DIR}/slc_cli" ]; then
    export PATH="${SILABS_TOOLS_DIR}/slc_cli:$PATH"
    export PATH="${SILABS_TOOLS_DIR}/arm-gnu-toolchain/bin:$PATH"
    export PATH="${SILABS_TOOLS_DIR}/commander:$PATH"
    export GECKO_SDK="${SILABS_TOOLS_DIR}/gecko_sdk"
    export JAVA_TOOL_OPTIONS="-Duser.home=${SILABS_TOOLS_DIR}"
fi

# =========================================
# Check prerequisites
# =========================================

# Check slc
if ! command -v slc >/dev/null 2>&1; then
    echo "ERROR: slc (Silicon Labs CLI) not found in PATH"
    echo ""
    echo "Setup options:"
    echo "  1. Use Docker: docker run -it --rm -v \$(pwd):/workspace rtl8196e-gateway-builder"
    echo "  2. Native: cd 1-Build-Environment/12-silabs-toolchain && ./install_silabs.sh"
    exit 1
fi
SLC_VERSION=$(slc --version 2>/dev/null | head -1)
echo "slc: ${SLC_VERSION}"

# Check ARM GCC
if ! command -v arm-none-eabi-gcc >/dev/null 2>&1; then
    echo "ERROR: arm-none-eabi-gcc not found in PATH"
    exit 1
fi
echo "ARM GCC: $(arm-none-eabi-gcc --version | head -1)"

# Check GECKO_SDK
if [ -z "${GECKO_SDK:-}" ]; then
    # Try common locations
    if [ -d "${SILABS_TOOLS_DIR}/gecko_sdk" ]; then
        export GECKO_SDK="${SILABS_TOOLS_DIR}/gecko_sdk"
    elif [ -d "/home/builder/gecko_sdk" ]; then
        export GECKO_SDK="/home/builder/gecko_sdk"
    elif [ -d "$HOME/silabs/gecko_sdk" ]; then
        export GECKO_SDK="$HOME/silabs/gecko_sdk"
    elif [ -d "$HOME/gecko_sdk" ]; then
        export GECKO_SDK="$HOME/gecko_sdk"
    else
        echo "ERROR: GECKO_SDK environment variable not set"
        exit 1
    fi
fi

if [ ! -d "${GECKO_SDK}/protocol/openthread" ]; then
    echo "ERROR: Gecko SDK not found or incomplete: ${GECKO_SDK}"
    echo "       OpenThread component required for RCP build"
    exit 1
fi
echo "Gecko SDK: ${GECKO_SDK}"
echo ""

# =========================================
# Prepare build directory
# =========================================
echo "[1/5] Preparing build directory..."
rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

# SDK sample directory
SDK_SAMPLE_DIR="${GECKO_SDK}/protocol/openthread/sample-apps/ot-ncp"
SDK_PLATFORM_DIR="${GECKO_SDK}/util/third_party/openthread/src/lib/platform"

# Copy slcp and main.c from patches, other sources from SDK sample
cp "${PATCHES_DIR}/${PROJECT_NAME}.slcp" .
cp "${PATCHES_DIR}/main.c" .              # Patched with RTL8196E boot delay
cp "${SDK_SAMPLE_DIR}/app.c" .
cp "${SDK_SAMPLE_DIR}/app.h" .
cp "${SDK_PLATFORM_DIR}/reset_util.h" .
# Point the slcp's flow-control config item at the board's (CPC-clamped) flow
# type so the project file matches the VCOM header that apply_uart_config
# writes below. The value sits on the line after the name in this slcp. lidl
# resolves back to the same hw token, so the slcp stays byte-identical.
# (This slcp pins no device component — --with does the job.)
sed -i -E "/SL_CPC_DRV_UART_VCOM_FLOW_CONTROL_TYPE/{n;s|(value: )[A-Za-z0-9]+|\1${RCP_FLOW_TOK}|}" ${PROJECT_NAME}.slcp
echo "  - Copied slcp, main.c from patches (RTL8196E delay; flow=${BOARD_UART_FLOW})"
echo "  - Copied app.c, app.h, reset_util.h from SDK"

# =========================================
# Generate project with slc
# =========================================
echo ""
echo "[2/5] Generating project with slc..."
slc generate ${PROJECT_NAME}.slcp --sdk "${GECKO_SDK}" --with ${TARGET_DEVICE} --force 2>&1 | tail -5

# =========================================
# Apply configuration patches
# =========================================
echo ""
echo "[3/5] Applying configuration..."

# Copy config files for the selected board
if [ -d "config" ]; then
    cp "${PATCHES_DIR}/sl_cpc_drv_uart_usart_vcom_config.h" config/ 2>/dev/null || true
    cp "${PATCHES_DIR}/sl_cpc_security_config.h" config/ 2>/dev/null || true
    # Substitute the requested baud into the CPC UART config header
    sed -i "s|^#define SL_CPC_DRV_UART_VCOM_BAUDRATE.*|#define SL_CPC_DRV_UART_VCOM_BAUDRATE                 ${BAUD}|" config/sl_cpc_drv_uart_usart_vcom_config.h
    # Apply the selected board's UART routing — a no-op (byte-identical header)
    # for the lidl reference, the override path for ported boards. The sw token
    # equals the none token: CPC has no software flow control (see top note).
    apply_uart_config config/sl_cpc_drv_uart_usart_vcom_config.h \
        SL_CPC_DRV_UART_VCOM usartHwFlowControlCtsAndRts usartHwFlowControlNone usartHwFlowControlNone
    echo "  - Copied UART config (baud=${BAUD}, board=${BOARD}, flow=${BOARD_UART_FLOW})"
    echo "  - Copied security config (CPC security disabled)"
fi

# =========================================
# Patch Makefile
# =========================================
echo ""
echo "[4/5] Patching Makefile..."
ARM_GCC_DIR=$(dirname $(dirname $(which arm-none-eabi-gcc)))
echo "  - Setting ARM_GCC_DIR to ${ARM_GCC_DIR}"

MAKEFILE="${PROJECT_NAME}.Makefile"
if [ -f "${MAKEFILE}" ]; then
    sed -i "s|^ARM_GCC_DIR_LINUX\s*=.*|ARM_GCC_DIR_LINUX = ${ARM_GCC_DIR}|" "${MAKEFILE}"

    # Add -Oz optimization
    if ! grep -q 'subst -Os,-Oz' "${MAKEFILE}"; then
        echo "  - Adding -Oz optimization"
        sed -i "/-include ${PROJECT_NAME}.project.mak/a\\
\\
# Override optimization flags for maximum size reduction\\
C_FLAGS := \$(subst -Os,-Oz,\$(C_FLAGS))\\
CXX_FLAGS := \$(subst -Os,-Oz,\$(CXX_FLAGS))" "${MAKEFILE}"
    fi
fi

# =========================================
# Compile
# =========================================
echo ""
echo "[5/5] Compiling firmware..."

# Set commander path for post-build if available
if command -v commander >/dev/null 2>&1; then
    COMMANDER_DIR=$(dirname $(which commander))
    export STUDIO_ADAPTER_PACK_PATH="${COMMANDER_DIR}"
    export POST_BUILD_EXE="${COMMANDER_DIR}/commander"
fi

make -f ${PROJECT_NAME}.Makefile -j$(nproc)

# =========================================
# Copy output files
# =========================================
echo ""
echo "Copying output files..."
mkdir -p "${OUTPUT_DIR}"

SRC_BASE="build/debug/${PROJECT_NAME}"
# Flow-control type in the filename (#145). CPC has no software flow control, so
# a sw board is built with flow NONE (see the clamp note above) — name it as the
# as-built value so it matches flash_efr32.sh's FIRMWARE_FLOW_CTRL record.
FLOW_TAG="${BOARD_UART_FLOW}"
[ "${BOARD_UART_FLOW}" = "sw" ] && FLOW_TAG=none
OUT_BASE="${PROJECT_NAME}-${BAUD}-${FLOW_TAG}${BOARD_SUFFIX}"

if [ -f "${SRC_BASE}.s37" ]; then
    # Only remove the specific files we're about to rewrite — preserve other
    # baud variants in firmware/ (the matrix lives here side-by-side).
    rm -f "${OUTPUT_DIR}/${OUT_BASE}".{s37,gbl,hex,bin} 2>/dev/null

    # Copy .s37 for J-Link flashing
    cp "${SRC_BASE}.s37" "${OUTPUT_DIR}/${OUT_BASE}.s37"

    # Create .gbl file using commander for UART flashing
    if command -v commander >/dev/null 2>&1; then
        echo "Creating .gbl file..."
        commander gbl create "${OUTPUT_DIR}/${OUT_BASE}.gbl" --app "${SRC_BASE}.s37"
    else
        echo "WARNING: commander not found, cannot create .gbl file"
    fi
fi

# =========================================
# Summary
# =========================================
echo ""
echo "========================================="
echo "  BUILD COMPLETE"
echo "========================================="
echo ""
echo "CPC Protocol Version: 5 (GSDK 4.5.0 native)"
echo ""
echo "Firmware size:"
if [ -f "${SRC_BASE}.out" ]; then
    arm-none-eabi-size "${SRC_BASE}.out"
fi
echo ""
echo "Output files:"
ls -lh "${OUTPUT_DIR}/${OUT_BASE}".{gbl,s37} 2>/dev/null
echo ""
echo "Flash commands:"
echo "  Via UART:   universal-silabs-flasher --device /dev/ttyUSB0 --firmware firmware/${OUT_BASE}.gbl"
echo "  Via J-Link: commander flash firmware/${OUT_BASE}.s37 --device ${TARGET_DEVICE}"
echo ""
echo "Host setup (Linux):"
echo "  1. Build and install cpcd (see cpcd/README.md)"
echo "  2. Build and install zigbeed (see zigbeed/README.md)"
echo "  3. Configure and start with rcp-stack (see rcp-stack/README.md)"
echo "  4. Connect Zigbee2MQTT to /tmp/ttyZ2M with adapter: ember"
echo ""
