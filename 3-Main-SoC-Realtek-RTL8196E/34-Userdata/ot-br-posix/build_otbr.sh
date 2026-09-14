#!/bin/sh
# build_otbr.sh — Build OpenThread Border Router (POSIX) with Lexra toolchain for RTL8196E
#
# ot-br-posix: OpenThread Border Router POSIX implementation
# Source: https://github.com/openthread/ot-br-posix
# License: BSD-3-Clause
#
# Usage:
#   ./build_otbr.sh [branch/tag/commit]
#
# Examples:
#   ./build_otbr.sh              # Default (pinned release tag below)
#   ./build_otbr.sh main         # Latest development branch
#   ./build_otbr.sh v2026.06.0   # An earlier upstream release
#
# Note: This is a complex project with many dependencies. This script is meant
# for experimentation and may require adjustments based on your needs.
#
# J. Nilo - Jan 2025

set -e

ORIG_DIR="$(pwd)"
trap 'cd "$ORIG_DIR"' EXIT

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# Project root is 4 levels up: ot-br-posix -> 34-Userdata -> 3-Main-SoC -> project root
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"

# Pinned revision for reproducible builds — an upstream release tag since
# v2026.07.0 (2026-06-25), which supersedes the earlier practice of pinning a
# main-branch commit (the last one was 717abf0d, 2026-05-01).
# Compatibility with Home Assistant:
#   - HA Core >= 2026.5 ships python-otbr-api >= 2.10.0 (PR #244,
#     merged 2026-04-27).  2.10.0 auto-detects the schema by probing
#     `GET /api/actions`: 200 → assume camelCase, 404 → assume
#     PascalCase.  Two patches below make both HA versions happy:
#       1. JSON keys rewritten to PascalCase (json.cpp).
#       2. /api/actions route handlers commented out
#          (rest_web_server.cpp), so the probe sees 404 and 2.10.0
#          falls back to PascalCase mode.
#   - HA Core <= 2026.4.x ships python-otbr-api 2.9.x or earlier,
#     which only accepts PascalCase — patch (1) alone is enough.
# Drop both patches only if/when the project pins a min HA Core
# version of 2026.5 AND switches its callers to camelCase.
# To update: check https://github.com/openthread/ot-br-posix/releases
#            or test with: ./build_otbr.sh main
OTBR_DEFAULT="v2026.09.0"  # released 2026-09-01, openthread submodule 2a8b4a57edb1
BRANCH="${1:-$OTBR_DEFAULT}"
SOURCE_DIR="${SCRIPT_DIR}/ot-br-posix"
BUILD_DIR="${SCRIPT_DIR}/build"

echo "========================================="
echo "  BUILDING OT-BR-POSIX"
echo "========================================="
echo ""
echo "Branch/Tag/Commit: ${BRANCH}"
echo ""

# Clone or update repository
# Note: We don't use --depth 1 because we may checkout a specific commit SHA
if [ ! -d "$SOURCE_DIR" ]; then
    echo "==> Cloning ot-br-posix repository..."
    git clone --single-branch https://github.com/openthread/ot-br-posix.git "$SOURCE_DIR"
    cd "$SOURCE_DIR"
    git checkout "$BRANCH"
    git submodule update --init --recursive
else
    echo "==> Source directory exists, updating..."
    cd "$SOURCE_DIR"
    # Drop the patches from the previous run so checkout is clean — the
    # submodules too (patch 3 below lands in the openthread submodule, which
    # `git reset` on the superproject does not touch).
    git reset --hard HEAD
    git submodule foreach --recursive --quiet 'git reset --hard --quiet HEAD'
    git fetch origin
    git checkout "$BRANCH"
    git submodule update --init --recursive
fi

# --------------------------------------------------------------------
# HA-compat REST patches (see header for rationale).
# --------------------------------------------------------------------

# Patch 1: REST API JSON keys camelCase → PascalCase.
# ot-br-posix main switched to camelCase (commit 2e8ccf2b, Sep 2025).
# python-otbr-api up to 2.9.x expects PascalCase exclusively;
# 2.10.0+ (HA Core 2026.5+) auto-detects but only accepts PascalCase
# when patch 2 below has neutralised the /api/actions probe.
# The key literals live in src/rest/names.hpp since v2026.08.0 (#3296,
# "standardize attribute naming"); older tags keep them in json.cpp.
JSON_CPP="${SOURCE_DIR}/src/rest/names.hpp"
[ -f "$JSON_CPP" ] || JSON_CPP="${SOURCE_DIR}/src/rest/json.cpp"
if [ -f "$JSON_CPP" ] && grep -q '"activeTimestamp"' "$JSON_CPP"; then
    echo "==> Patching REST JSON keys to PascalCase (HA compatibility)..."
    sed -i \
        -e 's/"activeTimestamp"/"ActiveTimestamp"/g' \
        -e 's/"networkKey"/"NetworkKey"/g' \
        -e 's/"networkName"/"NetworkName"/g' \
        -e 's/"extPanId"/"ExtPanId"/g' \
        -e 's/"meshLocalPrefix"/"MeshLocalPrefix"/g' \
        -e 's/"panId"/"PanId"/g' \
        -e 's/"channel"/"Channel"/g' \
        -e 's/"channelMask"/"ChannelMask"/g' \
        -e 's/"pskc"/"PSKc"/g' \
        -e 's/"securityPolicy"/"SecurityPolicy"/g' \
        -e 's/"seconds"/"Seconds"/g' \
        -e 's/"ticks"/"Ticks"/g' \
        -e 's/"authoritative"/"Authoritative"/g' \
        -e 's/"rotationTime"/"RotationTime"/g' \
        -e 's/"obtainNetworkKey"/"ObtainNetworkKey"/g' \
        -e 's/"nativeCommissioning"/"NativeCommissioning"/g' \
        -e 's/"externalCommissioning"/"ExternalCommissioning"/g' \
        -e 's/"commercialCommissioning"/"CommercialCommissioning"/g' \
        -e 's/"autonomousEnrollment"/"AutonomousEnrollment"/g' \
        -e 's/"networkKeyProvisioning"/"NetworkKeyProvisioning"/g' \
        -e 's/"tobleLink"/"TobleLink"/g' \
        -e 's/"routers"/"Routers"/g' \
        -e 's/"nonCcmRouters"/"NonCcmRouters"/g' \
        -e 's/"pendingTimestamp"/"PendingTimestamp"/g' \
        -e 's/"delay"/"Delay"/g' \
        -e 's/"activeDataset"/"ActiveDataset"/g' \
        "$JSON_CPP"
fi

# Patch 2: disable /api/actions REST routes.
# python-otbr-api 2.10.0+ probes `GET /api/actions` to detect the
# REST schema: 200 → camelCase mode, 404 → PascalCase mode.  Since
# patch 1 above forces PascalCase JSON, we must also force the probe
# to fail so the library picks the matching parser.  Comment out the
# route registrations.  HA Core itself does not call /api/actions
# (it only uses the older /node/* endpoints), so nothing of value is
# lost.  See AUDIT.md and CHANGELOG.md (v3.4.x) for the analysis.
# Two registration syntaxes: `mServer.Get(...)` up to v2026.07.0,
# `RegisterGet(...)` since the per-route method registry (#3546,
# v2026.09.0) — an unregistered path still answers 404 there
# (RoutingErrorHandler only upgrades to 405 when the path is known).
REST_CPP="${SOURCE_DIR}/src/rest/rest_web_server.cpp"
ACTIONS_ROUTE_RE='(mServer\.(Get|Post|Delete|Options|Put|Patch)|Register(Get|Post|Delete|Options|Put|Patch))\(OT_REST_ROUTE_ACTIONS'
if [ -f "$REST_CPP" ] && \
   grep -qE "^[[:space:]]*${ACTIONS_ROUTE_RE}" "$REST_CPP"; then
    echo "==> Disabling /api/actions routes (HA 2026.5+ camelCase fallback trigger)..."
    sed -i -E \
        "s#^([[:space:]]*)(${ACTIONS_ROUTE_RE})#\\1// PATCHED HA-compat: \\2#" \
        "$REST_CPP"
fi

# Patch 3: software flow control on the RCP UART (discussion #134, @hlyi).
# Upstream OpenThread only knows hardware flow control: OpenFile() calls
# cfmakeraw() (which clears IXON/IXOFF) and the sole flow-control URL parameter,
# uart-flow-control, sets CRTSCTS.  A board that does not wire RTS/CTS (the
# Sengled G4) runs its OT-RCP with the iostream backend, which DOES emit XON/XOFF
# — and on a tty without IXON those 0x11/0x13 bytes reach the HDLC decoder as
# frame data, fail the FCS, and drop the frame.  The patch adds the missing
# uart-sw-flow-control parameter (IXON|IXOFF), which S70otbr passes when
# radio.conf says FIRMWARE_FLOW_CTRL=sw.  Authored and field-validated by @hlyi.
OT_REPO="${SOURCE_DIR}/third_party/openthread/repo"
SW_FLOW_PATCH="${SCRIPT_DIR}/patches/openthread-uart-sw-flow-control.patch"
if [ -f "$SW_FLOW_PATCH" ] && \
   ! grep -q 'uart-sw-flow-control' "${OT_REPO}/src/posix/platform/hdlc_interface.cpp"; then
    echo "==> Patching OpenThread: uart-sw-flow-control (XON/XOFF) support (#134)..."
    git -C "$OT_REPO" apply "$SW_FLOW_PATCH"
fi

# Lexra toolchain (musl 1.2.5)
TOOLCHAIN_DIR="${PROJECT_ROOT}/x-tools/mips-lexra-linux-musl"
SYSROOT="${TOOLCHAIN_DIR}/mips-lexra-linux-musl/sysroot"

if [ ! -d "$TOOLCHAIN_DIR" ]; then
    echo "Error: Toolchain not found at ${TOOLCHAIN_DIR}"
    exit 1
fi

if ! command -v mips-lexra-linux-musl-gcc >/dev/null 2>&1; then
    export PATH="${TOOLCHAIN_DIR}/bin:$PATH"
fi
export CROSS_COMPILE="mips-lexra-linux-musl-"
export CC="${CROSS_COMPILE}gcc"
export CXX="${CROSS_COMPILE}g++"
export AR="${CROSS_COMPILE}ar"
export RANLIB="${CROSS_COMPILE}ranlib"
export STRIP="${CROSS_COMPILE}strip"

# Common flags for cross-compilation
export CFLAGS="-Os -fno-stack-protector -Wno-error=maybe-uninitialized -DOPENTHREAD_POSIX_CONFIG_DAEMON_SOCKET_BASENAME='\"/tmp/openthread-%s\"'"
export CXXFLAGS="-Os -fno-stack-protector -Wno-error=maybe-uninitialized -DOPENTHREAD_POSIX_CONFIG_DAEMON_SOCKET_BASENAME='\"/tmp/openthread-%s\"'"
export LDFLAGS="-static -Wl,-z,noexecstack,-z,relro,-z,now"

echo "==> Toolchain: ${TOOLCHAIN_DIR}"
echo "==> CC: ${CC}"
echo "==> CXX: ${CXX}"
echo ""

# Create build directory
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# CMake cross-compilation toolchain file
cat > toolchain-mips-lexra.cmake << 'EOF'
# CMake toolchain file for MIPS Lexra (RTL8196E)
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR mips)

# Toolchain paths (will be substituted)
set(TOOLCHAIN_DIR "$ENV{TOOLCHAIN_DIR}")
set(CMAKE_SYSROOT "${TOOLCHAIN_DIR}/mips-lexra-linux-musl/sysroot")

# Compilers
set(CMAKE_C_COMPILER "${TOOLCHAIN_DIR}/bin/mips-lexra-linux-musl-gcc")
set(CMAKE_CXX_COMPILER "${TOOLCHAIN_DIR}/bin/mips-lexra-linux-musl-g++")
set(CMAKE_AR "${TOOLCHAIN_DIR}/bin/mips-lexra-linux-musl-ar")
set(CMAKE_RANLIB "${TOOLCHAIN_DIR}/bin/mips-lexra-linux-musl-ranlib")
set(CMAKE_STRIP "${TOOLCHAIN_DIR}/bin/mips-lexra-linux-musl-strip")

# Compiler flags
set(CMAKE_C_FLAGS_INIT "-Os -fno-stack-protector -Wno-error=maybe-uninitialized -DOPENTHREAD_POSIX_CONFIG_DAEMON_SOCKET_BASENAME='\"/tmp/openthread-%s\"'")
set(CMAKE_CXX_FLAGS_INIT "-Os -fno-stack-protector -Wno-error=maybe-uninitialized -DOPENTHREAD_POSIX_CONFIG_DAEMON_SOCKET_BASENAME='\"/tmp/openthread-%s\"'")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -Wl,-z,noexecstack,-z,relro,-z,now")

# Override link command to handle circular dependencies between static libraries
# This wraps all libraries in --start-group/--end-group so the linker resolves
# circular references automatically (e.g., openthread-ftd <-> openthread-posix)
set(CMAKE_C_LINK_EXECUTABLE
    "<CMAKE_C_COMPILER> <FLAGS> <CMAKE_C_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> -Wl,--start-group <LINK_LIBRARIES> -Wl,--end-group")
set(CMAKE_CXX_LINK_EXECUTABLE
    "<CMAKE_CXX_COMPILER> <FLAGS> <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> -Wl,--start-group <LINK_LIBRARIES> -Wl,--end-group")

# Search paths
set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
EOF

# Substitute TOOLCHAIN_DIR in the toolchain file
sed -i "s|\$ENV{TOOLCHAIN_DIR}|${TOOLCHAIN_DIR}|g" toolchain-mips-lexra.cmake

echo "==> Running CMake configuration..."
echo ""
echo "Note: ot-br-posix has many optional features. Starting with minimal config."
echo "You may need to adjust CMAKE options based on your requirements."
echo ""

# Configure with CMake
# Configuration with Border Agent and built-in mDNS (OpenThread implementation)
# This enables:
#   - Border Agent: for Thread commissioning (Matter/HomeKit compatible)
#   - mDNS/DNS-SD: using OpenThread's built-in implementation (no external deps)
#   - SRP Advertising Proxy: automatic with OTBR_MDNS=openthread
#   - DNS-SD Discovery Proxy: automatic with OTBR_MDNS=openthread
cmake "$SOURCE_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="${BUILD_DIR}/toolchain-mips-lexra.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF \
    -DOTBR_DBUS=OFF \
    -DOTBR_WEB=OFF \
    -DOTBR_REST=ON \
    -DOTBR_MDNS=openthread \
    -DOTBR_BACKBONE_ROUTER=OFF \
    -DOTBR_BORDER_ROUTING=ON \
    -DOTBR_TREL=OFF \
    -DOTBR_NAT64=OFF \
    -DOTBR_DNS_UPSTREAM_QUERY=OFF \
    -DOTBR_BORDER_AGENT=ON \
    -DOT_POSIX_RCP_HDLC_BUS=ON \
    -DOT_FIREWALL=OFF \
    -DOT_CHANNEL_MANAGER=ON \
    -DOT_CHANNEL_MONITOR=ON \
    -DOT_POSIX_SETTINGS_PATH=\"/userdata/thread\" \
    "$@"

echo ""
echo "==> Configuration complete!"
echo ""

# Build
# Note: The toolchain file overrides CMAKE_CXX_LINK_EXECUTABLE to use
# --start-group/--end-group, which resolves circular dependencies between
# static libraries (openthread-ftd <-> openthread-posix, etc.)
echo "==> Building..."
make -j$(nproc)

echo "==> Stripping binaries..."
${STRIP} src/agent/otbr-agent
${STRIP} third_party/openthread/repo/src/posix/ot-ctl

# Install to skeleton
INSTALL_DIR="${SCRIPT_DIR}/../skeleton/usr/bin"
mkdir -p "$INSTALL_DIR"
cp src/agent/otbr-agent "$INSTALL_DIR/"
cp third_party/openthread/repo/src/posix/ot-ctl "$INSTALL_DIR/"
chmod +x "$INSTALL_DIR/otbr-agent" "$INSTALL_DIR/ot-ctl"

echo ""
echo "========================================="
echo "  BUILD COMPLETE"
echo "========================================="
echo ""
echo "  Binaries:"
ls -lh "$INSTALL_DIR/otbr-agent" "$INSTALL_DIR/ot-ctl"
echo ""
echo "  Features enabled:"
echo "    - Border Agent (Thread commissioning)"
echo "    - mDNS/DNS-SD (OpenThread built-in)"
echo "    - SRP Advertising Proxy"
echo "    - DNS-SD Discovery Proxy"
echo "    - Border Routing"
echo "    - Channel Manager / Monitor"
echo ""
echo "✅ otbr-agent and ot-ctl installed in $INSTALL_DIR"
