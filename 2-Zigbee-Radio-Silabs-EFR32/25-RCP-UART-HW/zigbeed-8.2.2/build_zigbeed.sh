#!/bin/bash
# build_zigbeed.sh - Build and install zigbeed from SiSDK 2025.6.3 / EZSP 18
#
# Portable script for x86_64, ARM64 (Raspberry Pi 4/5), ARM32.
# Automatically downloads Simplicity SDK 2025.6.3 if not present.
#
# Prerequisites:
#   - slc-cli in PATH (install via 1-Build-Environment/)
#   - libcpc installed (build cpcd first)
#
# Usage:
#   ./build_zigbeed.sh              # Build + prompt (TTY) or local (non-TTY)
#   ./build_zigbeed.sh --local      # Build + install to /usr/local
#   ./build_zigbeed.sh --deb        # Build + generate .deb (/usr)
#   ./build_zigbeed.sh clean        # Clean build directory
#
# J. Nilo - January 2026

prepare_deb_files() {
    echo "Creating build helper scripts..."
    # Debian Maintainer Scripts
    cat << 'EOF' > preinst
#!/bin/sh
set -eu
echo "Stopping zigbeed if running..."
pkill -f zigbeed || true
EOF

cat << 'EOF' > prerm
#!/bin/sh
set -e
echo "Stopping zigbeed before removal..."
pkill -f zigbeed || true
EOF

    chmod +x preinst prerm
}

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
CPC_DIR="${SCRIPT_DIR}/../cpcd/cpc-daemon"

# Project paths
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
SILABS_TOOLS="${PROJECT_ROOT}/silabs-tools"
SIMPLICITY_SDK="${SILABS_TOOLS}/simplicity_sdk_2025.6.3"
ZIGBEED_SAMPLE="${SIMPLICITY_SDK}/protocol/zigbee/app/projects/zigbeed"
ADAPTER_INSTALLER="${SCRIPT_DIR}/install_serial_adapter.sh"

REPO_OWNER=$(git remote get-url origin 2>/dev/null | sed -E 's/.*[:\/](.*)\/.*\..*/\1/') || true
REPO_OWNER="${REPO_OWNER:-unknown}"

# Parse arguments
INSTALL_MODE=""
case "${1:-}" in
    clean)
        echo "Cleaning..."
        rm -rf "${BUILD_DIR}"
        exit 0
        ;;
    --local)
        INSTALL_MODE=local
        ;;
    --deb)
        INSTALL_MODE=deb
        ;;
    "")
        ;;
    *)
        echo "Usage: $0 [--local | --deb | clean]"
        exit 1
        ;;
esac

# If no mode specified, prompt (TTY) or default (non-TTY)
if [ -z "$INSTALL_MODE" ]; then
    if [ -t 0 ]; then
        read -p "Install zigbeed local or build DEB-File? ((l)ocal/(d)eb): " answer
        case ${answer:0:1} in
            l|L) INSTALL_MODE=local ;;
            d|D) INSTALL_MODE=deb ;;
            *)
                echo "Installation canceled."
                exit 0
                ;;
        esac
    else
        echo "Non-interactive mode: defaulting to local install (/usr/local)"
        INSTALL_MODE=local
    fi
fi

echo "========================================="
echo "  zigbeed builder (SiSDK 2025.6.3 / EZSP 18)"
echo "  Architecture: $(uname -m)"
echo "========================================="

# =========================================
# Check prerequisites
# =========================================

# SDK version configuration
SIMPLICITY_SDK_VERSION="2025.6.3"
SIMPLICITY_SDK_TAG="v${SIMPLICITY_SDK_VERSION}"

# Check/install SDK
if [ ! -d "${SIMPLICITY_SDK}/protocol/zigbee" ]; then
    echo "Simplicity SDK not found at ${SIMPLICITY_SDK}"
    echo "Downloading from GitHub (this may take a while)..."
    echo ""
    mkdir -p "${SILABS_TOOLS}"
    git clone --depth 1 --branch "${SIMPLICITY_SDK_TAG}" \
        https://github.com/SiliconLabs/simplicity_sdk.git \
        "${SIMPLICITY_SDK}" \
        || { echo "ERROR: Failed to clone Simplicity SDK"; exit 1; }
    echo "Simplicity SDK ${SIMPLICITY_SDK_VERSION} installed"
    echo ""
fi

# Check slc
if [ -d "${SILABS_TOOLS}/slc_cli" ]; then
    export PATH="${SILABS_TOOLS}/slc_cli:$PATH"
    export JAVA_TOOL_OPTIONS="-Duser.home=${SILABS_TOOLS}"
fi

if ! command -v slc >/dev/null 2>&1; then
    echo "ERROR: slc not found in PATH"
    exit 1
fi
if [ ! -x "${ADAPTER_INSTALLER}" ]; then
    echo "ERROR: project serial adapter installer is missing or not executable: ${ADAPTER_INSTALLER}"
    exit 1
fi

# Check libcpc
if [ ! -s "${CPC_DIR}/build/libcpc.a" ]; then
    echo "ERROR: libcpc not found. Build cpcd first: ../cpcd/build_cpcd.sh"
    exit 1
fi

# =========================================
# Detect architecture and set slc parameters
# =========================================
case "$(uname -m)" in
    x86_64)
        ARCH="x86-64"
        ZIGBEE_COMP="zigbee_x86_64"
        LINUX_ARCH="linux_arch_64"
        ;;
    aarch64)
        ARCH="arm64v8"
        ZIGBEE_COMP="zigbee_arm64"
        LINUX_ARCH="linux_arch_64"
        ;;
    armv7l)
        ARCH="arm32v7"
        ZIGBEE_COMP="zigbee_arm32"
        LINUX_ARCH="linux_arch_32"
        ;;
    *)
        echo "ERROR: Unsupported architecture: $(uname -m)"
        exit 1
        ;;
esac

echo "Target: ${ARCH} (${ZIGBEE_COMP}, ${LINUX_ARCH})"

# =========================================
# Generate project
# =========================================
echo ""
echo "[1/2] Generating project..."

rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

# Copy sample sources
cp "${ZIGBEED_SAMPLE}"/*.c .
cp "${ZIGBEED_SAMPLE}"/*.h .
cp "${ZIGBEED_SAMPLE}/zigbeed.slcp" .

# Trust SDK
slc signature trust --sdk "${SIMPLICITY_SDK}" 2>/dev/null || true

# Generate with correct architecture components (like Nerivec)
# --with selects architecture-specific libraries automatically
# --without prevents slc from auto-selecting wrong architecture
slc generate zigbeed.slcp \
    --sdk "${SIMPLICITY_SDK}" \
    --with="${ZIGBEE_COMP},${LINUX_ARCH}" \
    --without=zigbee_recommended_linux_arch \
    -o makefile \
    --force 2>&1 | tail -5

"${ADAPTER_INSTALLER}" "${BUILD_DIR}"

# Replace partial SDK copy with symlink (slc copies some files, but not all headers)
rm -rf simplicity_sdk_2025.6.3
ln -s "${SIMPLICITY_SDK}" simplicity_sdk_2025.6.3

# =========================================
# Build
# =========================================
echo ""
echo "[2/2] Building..."

echo "Patching zigbeed.project.mak..."
sed -i "/platform\/service\/cpc\/daemon\/lib/a \  -I${CPC_DIR}/lib \\\\" zigbeed.project.mak
sed -i "/-lcpc/i \  -L${CPC_DIR}/build \\\\" zigbeed.project.mak

make -f zigbeed.Makefile -j$(nproc)

if ! LC_ALL=C grep -aFq "EZSP transport: TCP listen" build/debug/zigbeed; then
    echo "ERROR: Zigbeed binary does not contain the project serial adapter marker" >&2
    exit 1
fi

# =========================================
# Strip and install
# =========================================
echo ""
echo "Stripping binary..."

strip build/debug/zigbeed

# Install
case "$INSTALL_MODE" in
    local)
        echo "Installing to /usr/local/bin..."
        sudo install -m 0755 build/debug/zigbeed /usr/local/bin/
        echo "Done."
        ;;
    deb)
        echo "Generating DEB-Package..."
        APP_NAME="zigbeed"
        VERSION="1.0.0"
        DEPLOY_DIR="${BUILD_DIR}/tmp"

        install -D build/debug/zigbeed ${DEPLOY_DIR}/usr/bin/zigbeed

        prepare_deb_files
        cpack -G DEB \
          -D CPACK_GENERATOR="DEB" \
          -D CPACK_DEBIAN_PACKAGE_ARCHITECTURE="$ARCH" \
          -D CPACK_PACKAGE_NAME="$APP_NAME" \
          -D CPACK_PACKAGE_VERSION="$VERSION" \
          -D CPACK_PACKAGE_FILE_NAME="${APP_NAME}-${VERSION}-${ARCH}" \
          -D CPACK_PACKAGE_DESCRIPTION="zigbeed" \
          -D CPACK_DEBIAN_PACKAGE_MAINTAINER=$REPO_OWNER \
          -D CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA="preinst;prerm" \
          -D CPACK_OUTPUT_FILE_PREFIX="${SCRIPT_DIR}/packages" \
          -D CPACK_INSTALLED_DIRECTORIES="${DEPLOY_DIR};/" \

        rm -r ${DEPLOY_DIR}
        echo "Done."
        echo "use dpkg -i zigbeed.deb to install"
        ;;
esac

echo ""
echo "========================================="
echo "  Done! (SiSDK 2025.6.3 / EZSP 18)"
echo "========================================="
echo ""
echo "Usage:"
echo "  zigbeed -r 'spinel+cpc://cpcd_0?iid=1&iid-list=0' -p tcp-listen://127.0.0.1:9999    # Local TCP listener"
echo ""
echo "Zigbee2MQTT config:"
echo "  serial:"
echo "    port: tcp://localhost:9999"
echo "    adapter: ember"
