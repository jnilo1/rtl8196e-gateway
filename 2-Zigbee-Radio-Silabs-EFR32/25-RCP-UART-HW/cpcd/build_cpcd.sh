#!/bin/bash
# build_cpcd.sh - Build and install cpcd from Silicon Labs GitHub
#
# Portable script for x86_64, ARM64 (Raspberry Pi 4/5), etc.
#
# Prerequisites:
#   sudo apt install cmake build-essential
#
# Usage:
#   ./build_cpcd.sh              # Build + prompt (TTY) or local (non-TTY)
#   ./build_cpcd.sh --local      # Build + install to /usr/local
#   ./build_cpcd.sh --deb        # Build + generate .deb (/usr)
#   ./build_cpcd.sh clean        # Remove source directory
#
# J. Nilo - January 2026

prepare_cmake() {
    cat << 'EOF' > cmakeLists.patch
--- CMakeLists.txt
+++ CMakeLists.txt
@@ -185,11 +185,18 @@
 endif()
 
 install(TARGETS cpc
+  COMPONENT development
   LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
   PUBLIC_HEADER DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
-install(
-  FILES "${PROJECT_BINARY_DIR}/libcpc.pc"
+
+install(FILES "${PROJECT_BINARY_DIR}/libcpc.pc"
+  COMPONENT development
   DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")
 install(TARGETS cpcd
+  COMPONENT runtime
   RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
-install(FILES "./cpcd.conf" DESTINATION "${CMAKE_INSTALL_FULL_SYSCONFDIR}")
+
+install(FILES "./cpcd.conf"
+  COMPONENT runtime
+  DESTINATION "${CMAKE_INSTALL_FULL_SYSCONFDIR}")
+include(CPack)
EOF

patch -b CMakeLists.txt < cmakeLists.patch
rm cmakeLists.patch
}

# Apply the native TCP-bus patch (adds bus_type: TCP so cpcd connects straight to
# the gateway's UART<->TCP bridge, no socat PTY shim). Idempotent: skips if the
# patch is already applied to the clone.
apply_tcp_bus_patch() {
    local patch_file="${SCRIPT_DIR}/tcp-bus.patch"
    [ -f "$patch_file" ] || { echo "WARNING: ${patch_file} missing, skipping TCP bus patch"; return 0; }
    if git -C "$CPCD_SRC" apply --reverse --check "$patch_file" 2>/dev/null; then
        echo "TCP bus patch already applied."
    elif git -C "$CPCD_SRC" apply --check "$patch_file" 2>/dev/null; then
        git -C "$CPCD_SRC" apply "$patch_file"
        echo "Applied TCP bus patch."
    else
        echo "ERROR: tcp-bus.patch does not apply cleanly to this cpcd checkout." >&2
        exit 1
    fi
}

prepare_deb_files() {
    echo "Creating build helper scripts..."
    # Debian Maintainer Scripts
    cat << 'EOF' > preinst
#!/bin/sh
set -e
echo "Stopping cpc-daemon if running..."
pkill -f cpcd || true
EOF

    cat << 'EOF' > prerm
#!/bin/sh
set -e
echo "Stopping cpc-daemon before removal..."
pkill -f cpcd || true
EOF

    chmod +x preinst prerm
}

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CPCD_SRC="${SCRIPT_DIR}/cpc-daemon"

CPCD_REPO="https://github.com/SiliconLabs/cpc-daemon.git"
CPCD_VERSION="v4.6.2"

REPO_OWNER=$(git remote get-url origin 2>/dev/null | sed -E 's/.*[:\/](.*)\/.*\..*/\1/') || true
REPO_OWNER="${REPO_OWNER:-unknown}"

# Parse arguments
INSTALL_MODE=""
case "${1:-}" in
    clean)
        echo "Cleaning..."
        rm -rf "${CPCD_SRC}"
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
        read -p "Install CPC-daemon local or build DEB-File? ((l)ocal/(d)eb): " answer
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

# Set install prefix based on mode
if [ "$INSTALL_MODE" = "local" ]; then
    CMAKE_INSTALL_PREFIX=/usr/local
else
    CMAKE_INSTALL_PREFIX=/usr
fi

echo "========================================="
echo "  cpcd ${CPCD_VERSION}"
echo "  Architecture: $(uname -m)"
echo "========================================="

# Clone or update
if [ -d "${CPCD_SRC}" ]; then
    echo "Using existing source"
else
    echo "Cloning ${CPCD_REPO}..."
    git clone --branch "${CPCD_VERSION}" --depth 1 "${CPCD_REPO}" "${CPCD_SRC}"
fi

# Build
cd "${CPCD_SRC}"
apply_tcp_bus_patch
prepare_cmake

cmake -B build -S . \
    -DCMAKE_INSTALL_PREFIX=${CMAKE_INSTALL_PREFIX} \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_EXE_LINKER_FLAGS='-static-libgcc -static-libstdc++' \
    -DBUILD_SHARED_LIBS=OFF `#include libcpc in binary` \
    -DENABLE_ENCRYPTION=OFF `#disable encryption, we dont use it` \
    -DCMAKE_C_FLAGS="-s" \
    -DCPACK_GENERATOR=DEB \
    -DCPACK_DEBIAN_PACKAGE_MAINTAINER=$REPO_OWNER \
    -DCPACK_DEBIAN_PACKAGE_SHLIBDEPS=OFF \
    -DCPACK_PACKAGE_DESCRIPTION_SUMMARY='cpc-daemon - statically linked no mbedTLS' \
    -DCPACK_DEBIAN_PACKAGE_DEPENDS='socat' \
    -DCPACK_DEBIAN_PACKAGE_CONTROL_EXTRA="preinst;prerm" \
    -DCPACK_OUTPUT_FILE_PREFIX="${SCRIPT_DIR}/packages" \
    -DCPACK_COMPONENTS_ALL=runtime \
    -DCPACK_DEB_COMPONENT_INSTALL=ON
cmake --build build

# cleanup
mv CMakeLists.txt.orig CMakeLists.txt

# Install
cd build

case "$INSTALL_MODE" in
    local)
        echo "Installing locally..."
        sudo cmake -DCOMPONENT=runtime -P cmake_install.cmake
        echo "Done."
        ;;
    deb)
        echo "Generating DEB-Package..."
        prepare_deb_files
        cpack -G DEB
        echo "Done."
        echo "use dpkg -i cpc-daemon.deb to install"
        ;;
esac
