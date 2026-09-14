#!/bin/sh
# build_nano.sh — Build STATIC nano with Lexra toolchain (musl 1.2.5) for RTL8196E
#
# nano: GNU nano text editor
# Source: https://www.nano-editor.org/
# License: GPL-3.0
#
# Usage:
#   ./build_nano.sh [version]
#
# Examples:
#   ./build_nano.sh              # Default version (9.2)
#   ./build_nano.sh 7.2          # Specific version
#
# J. Nilo - Dec 2025

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
USERDATA_PART="${SCRIPT_DIR}/.."
# Project root is 4 levels up: nano -> 34-Userdata -> 3-Main-SoC -> project root
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"

# Parse version argument
VERSION="${1:-9.2}"
MAJOR_VERSION="${VERSION%%.*}"
SOURCE_DIR="${SCRIPT_DIR}/nano-${VERSION}"
INSTALL_DIR="${USERDATA_PART}/skeleton/usr/bin"
NCURSESW_DIR="${SCRIPT_DIR}/ncursesw-install"

echo "📦 Nano version: ${VERSION}"

# Build ncursesw first if not present
if [ ! -d "$NCURSESW_DIR" ]; then
    echo "Building ncursesw first..."
    "${SCRIPT_DIR}/build_ncursesw.sh"
fi

# Download if necessary (into script directory)
if [ ! -d "$SOURCE_DIR" ]; then
    echo "📥 Downloading nano-${VERSION}..."
    wget -qO- "https://www.nano-editor.org/dist/v${MAJOR_VERSION}/nano-${VERSION}.tar.xz" | tar xJ -C "${SCRIPT_DIR}"
fi

# Toolchain
TOOLCHAIN_DIR="${PROJECT_ROOT}/x-tools/mips-lexra-linux-musl"
if ! command -v mips-lexra-linux-musl-gcc >/dev/null 2>&1; then
    export PATH="${TOOLCHAIN_DIR}/bin:$PATH"
fi
export CROSS_COMPILE="mips-lexra-linux-musl-"
export CC="${CROSS_COMPILE}gcc"
export AR="${CROSS_COMPILE}ar"
export RANLIB="${CROSS_COMPILE}ranlib"
export STRIP="${CROSS_COMPILE}strip"
export CFLAGS="-Os -fno-stack-protector -I${NCURSESW_DIR}/include -I${NCURSESW_DIR}/include/ncursesw"
export LDFLAGS="-static -L${NCURSESW_DIR}/lib -Wl,-z,noexecstack,-z,relro,-z,now"
export PKG_CONFIG_PATH="${NCURSESW_DIR}/lib/pkgconfig"
export NCURSESW_CFLAGS="-I${NCURSESW_DIR}/include/ncursesw"
export NCURSESW_LIBS="-L${NCURSESW_DIR}/lib -lncursesw"

# Build
cd "$SOURCE_DIR"
[ -f Makefile ] && make clean
rm -f "$INSTALL_DIR"/nano

./configure \
  --host=mips-lexra-linux-musl \
  --prefix=/usr \
  --disable-nls \
  --enable-utf8 \
  --disable-speller \
  --disable-browser \
  --disable-help \
  --disable-justify \
  --disable-tabcomp \
  --disable-wrapping \
  --disable-mouse \
  --disable-operatingdir \
  --disable-histories \
  --disable-libmagic \
  --enable-tiny

make
${STRIP} src/nano

mkdir -p "$INSTALL_DIR"
cp src/nano "$INSTALL_DIR"/

# Create vi -> nano symlink (OpenVi doesn't support UTF-8/emojis)
ln -sf nano "$INSTALL_DIR/vi"

echo ""
echo "📊 Build summary:"
echo "  • Version: ${VERSION}"
echo "  • Binary: $(ls -lh src/nano | awk '{print $5}')"
echo "  • Installation: ${INSTALL_DIR}"
echo "  • Symlink: vi -> nano"
echo ""
echo "✅ nano installed in $INSTALL_DIR"
