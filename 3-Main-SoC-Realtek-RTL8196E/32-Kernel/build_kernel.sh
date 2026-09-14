#!/bin/bash
# build_kernel.sh — Build Linux for Realtek RTL8196E (Lexra MIPS)
#
# Two coexisting kernel lines, selected by KERNEL (default 6.18):
#   6.18 (production)   → patches-6.18/ files-6.18/ config-6.18-realtek.txt
#   7.2  (experimental) → patches-7.2/  files-7.2/  config-7.2-realtek.txt
# Output: kernel-img/<board>/kernel-<KERNEL>.img (zboot; in-tree decompressor).
#
# Usage:
#   ./build_kernel.sh                        # 6.18 / lidl → kernel-img/lidl/kernel-6.18.img
#   KERNEL=7.2 ./build_kernel.sh             # build the 7.2 line
#   BOARD=sengled-e39-g8c ./build_kernel.sh  # build for the Sengled G4 board
#   ./build_kernel.sh clean                  # wipe build tree, rebuild from scratch
#   ./build_kernel.sh menuconfig             # open menuconfig
#   ./build_kernel.sh olddefconfig           # update .config non-interactively
#   ./build_kernel.sh vmlinux                # build vmlinux only (no packaging)
#   ./build_kernel.sh --help
#
#   BOARD=<name>   board devicetree built in (default: lidl; also sengled-e39-g8c).
#   KERNEL=<line>  kernel line (default: 6.18; also 7.2).
#   KERNEL_MIRROR=<url>  base of a kernel.org mirror, the directory that holds
#                  v6.x/ and v7.x/ (default: empty = https://cdn.kernel.org/pub/linux/kernel).
#                  e.g. KERNEL_MIRROR=https://mirrors.ircam.fr/pub/linux/kernel
#                  The tarball is always verified against the sha256sums.asc of the
#                  same source; a truncated or corrupt download is deleted, never
#                  extracted.
#   KERNEL_DL_MIN_RATE=<bytes/s>  a transfer slower than this for 30 s is dropped
#                  and resumed on a fresh connection (default 500000: one TCP flow
#                  out of several can land on a degraded path and crawl at
#                  100 KB/s while a new one runs at 40 MB/s). Lower it on a slow link.
#   KERNEL_CURL_OPTS=<opts>  extra curl options for the download (default empty),
#                  e.g. KERNEL_CURL_OPTS=-4 when the IPv6 path to the mirror is
#                  degraded (seen 2026-09-14: IPv6 RTT 2-5 s, IPv4 40 MB/s to the
#                  same host); the log prints the address actually used.
#   Add-a-board recipe: the realtek dts Makefile of the selected line.
#
# J. Nilo — February 2026, unified April 2026

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# ── Kernel line selection ─────────────────────────────────────────────────
# KERNEL picks the line (default: 6.18, the shipped production line).
# KERNEL_VERSION pins the exact stable point release (drives the tarball name);
# KERNEL_MAJOR_MINOR is the family used in file/dir names (patches-<MM>/,
# files-<MM>/, config-<MM>-realtek.txt, linux-<MM>-rtl8196e/) so point bumps
# don't churn paths.

KERNEL="${KERNEL:-6.18}"
case "$KERNEL" in
    6.18)
        KERNEL_VERSION="6.18.51"        # exact tarball version
        KERNEL_MAJOR_MINOR="6.18"       # stable family (paths, image name)
        KERNEL_MAJOR="6.x"              # kernel.org /pub/linux/kernel/v${MAJOR}/
        ;;
    7.2)
        KERNEL_VERSION="7.2.5"          # exact tarball version (linux-7.2.5.tar.xz)
        KERNEL_MAJOR_MINOR="7.2"
        KERNEL_MAJOR="7.x"
        ;;
    *)
        echo "ERROR: unknown KERNEL '$KERNEL' (known lines: 6.18, 7.2)" >&2
        exit 1
        ;;
esac
KERNEL_TARBALL="linux-${KERNEL_VERSION}.tar.xz"
# Optional mirror: the directory that holds v6.x/, v7.x/ (trailing slash tolerated).
KERNEL_MIRROR="${KERNEL_MIRROR:-}"
KERNEL_BASE_URL="${KERNEL_MIRROR:-https://cdn.kernel.org/pub/linux/kernel}"
KERNEL_BASE_URL="${KERNEL_BASE_URL%/}"
KERNEL_URL="${KERNEL_BASE_URL}/v${KERNEL_MAJOR}/${KERNEL_TARBALL}"
KERNEL_SUMS_URL="${KERNEL_BASE_URL}/v${KERNEL_MAJOR}/sha256sums.asc"
KERNEL_DL_MIN_RATE="${KERNEL_DL_MIN_RATE:-500000}"   # bytes/s over 30 s, else drop + resume
KERNEL_CURL_OPTS="${KERNEL_CURL_OPTS:-}"              # e.g. -4 (see header)
VANILLA_DIR="linux-${KERNEL_VERSION}"

PATCHES_DIR="${SCRIPT_DIR}/patches-${KERNEL_MAJOR_MINOR}"
FILES_DIR="${SCRIPT_DIR}/files-${KERNEL_MAJOR_MINOR}"
CONFIG_FILE="${KCONFIG_FILE:-${SCRIPT_DIR}/config-${KERNEL_MAJOR_MINOR}-realtek.txt}"
DEFAULT_IMEM_POLICY="${SCRIPT_DIR}/scripts/imem/policies/${KERNEL_VERSION}.tsv"
IMEM_POLICY="${IMEM_POLICY:-$DEFAULT_IMEM_POLICY}"
USE_IMEM_POLICY=false
if [ "${IMEM_POLICY_DISABLE:-0}" != "1" ] && [ -f "$IMEM_POLICY" ] && \
        [ "${IMEM_PROFILE:-0}" != "1" ] && [ "${IMEM_EMPTY:-0}" != "1" ]; then
    USE_IMEM_POLICY=true
fi
# Text placement layout (scripts/imem/text_layout.py): link-level pads that put
# this exact release's hot functions back on benchmarked I-cache colours.
# Production builds only — a layout is defined against the production link,
# so it follows the release I-MEM policy and is skipped with it.
DEFAULT_TEXT_LAYOUT_DIR="${SCRIPT_DIR}/scripts/imem/layouts/${KERNEL_VERSION}"
TEXT_LAYOUT_DIR="${TEXT_LAYOUT_DIR:-$DEFAULT_TEXT_LAYOUT_DIR}"
USE_TEXT_LAYOUT=false
if [ "${TEXT_LAYOUT_DISABLE:-0}" != "1" ] && [ "$USE_IMEM_POLICY" = true ] && \
        [ -f "$TEXT_LAYOUT_DIR/pads.patch" ]; then
    USE_TEXT_LAYOUT=true
fi
# IMAGE depends on BOARD too; defined after the board selection below.

TOOLCHAIN_DIR="${PROJECT_ROOT}/x-tools/mips-lexra-linux-musl"
# Add toolchain to PATH only if not already available (avoids GLIBC mismatch in Docker)
if ! command -v mips-lexra-linux-musl-gcc >/dev/null 2>&1; then
    export PATH="${TOOLCHAIN_DIR}/bin:$PATH"
fi
export ARCH=mips
export CROSS_COMPILE=mips-lexra-linux-musl-

# cvimg only — no lzma or lzma-loader needed (zboot uses in-tree decompressor)
BUILD_ENV="${PROJECT_ROOT}/1-Build-Environment/11-realtek-tools"
DOCKER_TOOLS="/home/builder/realtek-tools"

CVIMG=""
for dir in "$DOCKER_TOOLS" "$BUILD_ENV"; do
    [ -x "${dir}/bin/cvimg" ] && CVIMG="${dir}/bin/cvimg" && break
done
# Auto-build cvimg if not found
if [ -z "$CVIMG" ]; then
    CVIMG_SRC="${BUILD_ENV}/cvimg/cvimg.c"
    if [ -f "$CVIMG_SRC" ]; then
        if ! command -v gcc >/dev/null 2>&1; then
            echo "Error: gcc not found (needed to compile cvimg)." >&2
            echo "Install it with: sudo apt install gcc" >&2
            exit 1
        fi
        echo "cvimg not found — building it..."
        # Clear a dangling symlink left by the Docker entrypoint when reusing the
        # same workspace from the host (target /home/builder/... only exists in-container).
        [ -L "${BUILD_ENV}/bin" ] && [ ! -d "${BUILD_ENV}/bin" ] && rm -f "${BUILD_ENV}/bin"
        mkdir -p "${BUILD_ENV}/bin"
        if gcc -std=c99 -Wall -O2 -D_GNU_SOURCE -o "${BUILD_ENV}/bin/cvimg" "$CVIMG_SRC"; then
            CVIMG="${BUILD_ENV}/bin/cvimg"
            echo "cvimg built."
        fi
    fi
fi

CVIMG_BURN_ADDR="0x00020000"
SIGNATURE="cs6c"

# ── Board selection ───────────────────────────────────────────────────────
# One dtb per board (BUILTIN_DTB): BOARD maps to the matching entry of the
# "Devicetree selection" Kconfig choice. The committed config selects the
# Lidl board; other boards are flipped in by the .config fixup below.

BOARD="${BOARD:-lidl}"
case "$BOARD" in
    lidl) BOARD_DTB_SYM="CONFIG_DTB_RTL8196E_GEN" ;;
    sengled-e39-g8c) BOARD_DTB_SYM="CONFIG_DTB_RTL8196E_SENGLED_E39_G8C" ;;
    *)
        echo "ERROR: unknown BOARD '$BOARD' (known boards: lidl, sengled-e39-g8c)" >&2
        exit 1
        ;;
esac

# Pre-built image slot for this (board, kernel) pair. build_kernel.sh writes
# straight into the shippable kernel-img/<board>/kernel-<line>.img layout that
# the flash scripts resolve from BOARD/KERNEL (see lib/kernel_image.sh).
IMAGE="${IMAGE_OUT:-${SCRIPT_DIR}/kernel-img/${BOARD}/kernel-${KERNEL_MAJOR_MINOR}.img}"
case "$IMAGE" in
	/*) ;;
	*) IMAGE="${SCRIPT_DIR}/${IMAGE}" ;;
esac

# ── Option parsing ────────────────────────────────────────────────────────

DO_CLEAN=false
DO_MENUCONFIG=false
DO_OLDDEFCONFIG=false
BUILD_VMLINUX_ONLY=false

for arg in "$@"; do
    case "$arg" in
        clean)        DO_CLEAN=true ;;
        menuconfig)   DO_MENUCONFIG=true ;;
        olddefconfig) DO_OLDDEFCONFIG=true ;;
        vmlinux|no-package) BUILD_VMLINUX_ONLY=true ;;
        --help|-h)
            sed -n '2,16p' "$0" | sed 's|^# \{0,1\}||'
            exit 0
            ;;
        *) echo "Unknown option: $arg (use --help)"; exit 1 ;;
    esac
done

# Stamp the firmware release (../VERSION, the single source of truth) into
# the kernel localversion: `uname -r` reads e.g. 6.18.35-rtl8196e-v3.8.3,
# so the kernel partition self-identifies after a kernel-only reflash —
# /userdata/etc/version describes the userdata partition and legitimately
# goes stale on partial flashes (issue #120 feedback).
FW_VERSION="$(head -n1 "${SCRIPT_DIR}/../VERSION" 2>/dev/null)"
BUILD_TAG="${BUILD_TAG:-}"
case "$BUILD_TAG" in
    ""|*[!A-Za-z0-9._-]*)
        [ -z "$BUILD_TAG" ] || {
            echo "ERROR: BUILD_TAG may contain only letters, digits, '.', '_' and '-'" >&2
            exit 1
        }
        ;;
esac
export LOCALVERSION="-rtl8196e${FW_VERSION:+-v${FW_VERSION}}${BUILD_TAG:+-${BUILD_TAG}}"
BUILD_DIR="${SCRIPT_DIR}/linux-${KERNEL_MAJOR_MINOR}${BUILD_TAG:+-${BUILD_TAG}}-rtl8196e"

echo "==================================================================="
echo "  Linux ${KERNEL_VERSION} — RTL8196E — driver: rtl8196e"
echo "  Compression: arch/mips/boot/compressed/ (zboot)"
echo "==================================================================="
echo ""

# ── Preflight ──────────────────────────────────────────────────────────────

if ! command -v ${CROSS_COMPILE}gcc >/dev/null 2>&1; then
    echo "ERROR: Lexra toolchain not found: ${CROSS_COMPILE}gcc"
    echo "  Build it: cd ../../1-Build-Environment/10-lexra-toolchain && ./build_toolchain.sh"
    exit 1
fi
echo "Toolchain: $(${CROSS_COMPILE}gcc --version | head -1)"

if [ ! -d "$PATCHES_DIR" ]; then
    echo "ERROR: patches dir not found: $PATCHES_DIR"
    exit 1
fi
if [ ! -d "$FILES_DIR" ]; then
    echo "ERROR: files dir not found: $FILES_DIR"
    exit 1
fi

echo "Build dir:   $BUILD_DIR"
echo "Patches dir: $(basename "$PATCHES_DIR")"
echo "Files dir:   $(basename "$FILES_DIR")"
echo "Config file: $(basename "$CONFIG_FILE")"
echo "Board:       ${BOARD} (${BOARD_DTB_SYM})"
echo ""

# ── Clean ──────────────────────────────────────────────────────────────────

if [ "$DO_CLEAN" = true ] && [ -d "$BUILD_DIR" ]; then
    echo "Removing build tree: $BUILD_DIR"
    rm -rf "$BUILD_DIR"
    echo "Done."
    echo ""
fi

# ── Prepare tree ───────────────────────────────────────────────────────────

if [ ! -f "$BUILD_DIR/Makefile" ]; then
    echo "--- Preparing kernel tree ---"
    echo ""
    cd "$SCRIPT_DIR"

    # Download (resumable, retried: the kernel.org CDN can stall silently for
    # minutes) and verify against the source's own sha256sums.asc before
    # extracting. A tarball already present is verified too: a previous run
    # interrupted mid-download leaves a truncated file behind.
    echo "Fetching Linux ${KERNEL_VERSION} from ${KERNEL_BASE_URL}/v${KERNEL_MAJOR}/"
    # shellcheck disable=SC2086 # KERNEL_CURL_OPTS is a list of options by design
    curl -fsS $KERNEL_CURL_OPTS --retry 10 --retry-all-errors -o "${KERNEL_TARBALL}.sha256sums" "$KERNEL_SUMS_URL" || {
        echo "ERROR: cannot fetch $KERNEL_SUMS_URL" >&2; exit 1; }
    expected="$(grep " ${KERNEL_TARBALL}\$" "${KERNEL_TARBALL}.sha256sums" | awk '{print $1}')"
    [ -n "$expected" ] || { echo "ERROR: ${KERNEL_TARBALL} not listed in $KERNEL_SUMS_URL" >&2; exit 1; }
    if [ ! -f "$KERNEL_TARBALL" ] || [ "$(sha256sum "$KERNEL_TARBALL" | awk '{print $1}')" != "$expected" ]; then
        [ -f "$KERNEL_TARBALL" ] && echo "Resuming an incomplete ${KERNEL_TARBALL} ($(stat -c %s "$KERNEL_TARBALL") bytes)..."
        # shellcheck disable=SC2086
        curl -fS $KERNEL_CURL_OPTS -C - --speed-time 30 --speed-limit "$KERNEL_DL_MIN_RATE" --retry 30 --retry-all-errors \
            -w 'downloaded from %{remote_ip} at %{speed_download} B/s\n' \
            -o "$KERNEL_TARBALL" "$KERNEL_URL" || { echo "ERROR: download failed: $KERNEL_URL" >&2; exit 1; }
        if [ "$(sha256sum "$KERNEL_TARBALL" | awk '{print $1}')" != "$expected" ]; then
            echo "ERROR: ${KERNEL_TARBALL} sha256 mismatch against ${KERNEL_SUMS_URL} — removed" >&2
            rm -f "$KERNEL_TARBALL"; exit 1
        fi
    fi
    echo "sha256 OK: ${expected}"
    rm -f "${KERNEL_TARBALL}.sha256sums"

    echo "Extracting..."
    tar xf "$KERNEL_TARBALL"
    mv "$VANILLA_DIR" "$BUILD_DIR"
    rm -f "$KERNEL_TARBALL"

    cd "$BUILD_DIR"

    # Apply patches. A rejected or fuzzed hunk must abort the build — a
    # silently dropped hunk produces a subtly wrong kernel. Patches are kept
    # in sync with KERNEL_VERSION (offset 0, no fuzz); a .rej here means a
    # patch needs refreshing for the new point release.
    echo "Applying patches from $(basename "$PATCHES_DIR")..."
    shopt -s nullglob
    for patch in "$PATCHES_DIR"/*.patch; do
        [ -f "$patch" ] || continue
        echo "  $(basename "$patch")"
        patch_out="$(patch -p1 -f --no-backup-if-mismatch < "$patch" 2>&1)" || {
            printf '%s\n' "$patch_out" >&2
            echo "ERROR: patch failed to apply cleanly: $(basename "$patch")" >&2
            echo "  Refresh it against Linux ${KERNEL_VERSION} (see *.rej files)." >&2
            exit 1
        }
        printf '%s\n' "$patch_out"
        if printf '%s\n' "$patch_out" | \
                grep -Eiq 'fuzz|offset|warning|malformed|misordered|reversed|previously applied|FAILED|reject'; then
            echo "ERROR: patch applied with a warning, fuzz, or offset: $(basename "$patch")" >&2
            echo "  Refresh it against Linux ${KERNEL_VERSION}; only exact, warning-free hunks are accepted." >&2
            exit 1
        fi
    done
    shopt -u nullglob
    if find . -name '*.rej' | grep -q .; then
        echo "ERROR: rejected hunks present:" >&2
        find . -name '*.rej' >&2
        exit 1
    fi
    if [ "$USE_TEXT_LAYOUT" = true ]; then
        echo "Applying text layout pads: $(basename "$TEXT_LAYOUT_DIR")/pads.patch"
        patch_out="$(patch -p1 -f --no-backup-if-mismatch < "$TEXT_LAYOUT_DIR/pads.patch" 2>&1)" || {
            printf '%s\n' "$patch_out" >&2
            echo "ERROR: text layout pads failed to apply (regenerate with scripts/imem/propose_text_pads.py)" >&2
            exit 1
        }
        if printf '%s\n' "$patch_out" | \
                grep -Eiq 'fuzz|offset|warning|malformed|misordered|reversed|previously applied|FAILED|reject'; then
            echo "ERROR: text layout pads applied with a warning, fuzz, or offset" >&2
            exit 1
        fi
        sha256sum "$TEXT_LAYOUT_DIR/pads.patch" | awk '{print $1}' > "$BUILD_DIR/.text-layout.sha256"
    fi
    echo ""
else
    echo "Build tree already present: $BUILD_DIR"
    echo ""
fi

cd "$BUILD_DIR"

# Re-sync the overlay on every run. rsync -a preserves timestamps and only
# touches files that actually differ, so make's incremental rebuild stays
# correct: a file edited in files-6.18/ gets copied into the build tree,
# its mtime bumps, make rebuilds it. Files that didn't change are skipped.
# This closes the "edited files-6.18/X but build was a no-op" footgun.
echo "Syncing overlay from $(basename "$FILES_DIR")..."
[ -d "${FILES_DIR}/arch" ]    && rsync -a "${FILES_DIR}/arch/"    "$BUILD_DIR/arch/"
[ -d "${FILES_DIR}/drivers" ] && rsync -a "${FILES_DIR}/drivers/" "$BUILD_DIR/drivers/"
[ -d "${FILES_DIR}/include" ] && rsync -a "${FILES_DIR}/include/" "$BUILD_DIR/include/"
[ -d "${FILES_DIR}/Documentation" ] && \
	rsync -a "${FILES_DIR}/Documentation/" "$BUILD_DIR/Documentation/"
echo ""

# ── Config ─────────────────────────────────────────────────────────────────

if [ ! -f .config ]; then
    echo "Setting up .config..."
    if [ ! -f "$CONFIG_FILE" ]; then
        echo "ERROR: config file not found: $CONFIG_FILE" >&2
        exit 1
    fi
    cp "$CONFIG_FILE" .config
    make ARCH=$ARCH CROSS_COMPILE=$CROSS_COMPILE olddefconfig
    echo ""
else
    NEED_OLDDEFCONFIG=false

    if ! grep -q '^CONFIG_RTL8196E_ETH=y' .config; then
        echo "Fixing .config: enabling RTL8196E_ETH..."
        sed -i 's/^# CONFIG_RTL8196E_ETH is not set$/CONFIG_RTL8196E_ETH=y/' .config
        grep -q '^CONFIG_RTL8196E_ETH=y' .config || echo "CONFIG_RTL8196E_ETH=y" >> .config
        NEED_OLDDEFCONFIG=true
    fi

    if ! grep -q '^CONFIG_KERNEL_LZMA=y' .config; then
        echo "Fixing .config: enabling KERNEL_LZMA..."
        sed -i 's/^# CONFIG_KERNEL_LZMA is not set/CONFIG_KERNEL_LZMA=y/' .config
        grep -q '^CONFIG_KERNEL_LZMA=y' .config || echo "CONFIG_KERNEL_LZMA=y" >> .config
        NEED_OLDDEFCONFIG=true
    fi

    [ "$NEED_OLDDEFCONFIG" = true ] && make ARCH=$ARCH CROSS_COMPILE=$CROSS_COMPILE olddefconfig && echo ""
fi

# Board dtb: make the .config match BOARD (exactly one CONFIG_DTB_RTL8196E_*
# choice entry =y). No-op for the committed config + default BOARD=lidl.
if ! grep -q "^${BOARD_DTB_SYM}=y" .config; then
    echo "Fixing .config: selecting board dtb for BOARD=${BOARD}..."
    sed -i -E 's/^(CONFIG_DTB_RTL8196E_[A-Z0-9_]+)=y$/# \1 is not set/' .config
    sed -i "s/^# ${BOARD_DTB_SYM} is not set\$/${BOARD_DTB_SYM}=y/" .config
    grep -q "^${BOARD_DTB_SYM}=y" .config || echo "${BOARD_DTB_SYM}=y" >> .config
    make ARCH=$ARCH CROSS_COMPILE=$CROSS_COMPILE olddefconfig
    echo ""
fi

# Profiling reference for the I-MEM optimizer.  Keep it in an isolated build
# tree (BUILD_TAG=imem-profile) and make the window genuinely empty: the PC
# sampler only covers [_stext, _etext], so leaving historical residents in the
# window would assign them a false weight of zero.  The command line is extended
# rather than replaced so the board DT keeps its normal console/root settings.
if [ "${IMEM_PROFILE:-0}" = "1" ]; then
    echo "Fixing .config: I-MEM profiling reference (empty window, profile=4)..."
    scripts/config --enable RTL8196E_IMEM
    scripts/config --disable RTL8196E_IMEM_DEFAULT_PLACEMENT
    scripts/config --disable RTL8196E_IMEM_POC_IRAM
    scripts/config --enable PROFILING
    scripts/config --enable CMDLINE_BOOL
    scripts/config --disable CMDLINE_OVERRIDE
    scripts/config --disable MIPS_CMDLINE_FROM_DTB
    scripts/config --enable MIPS_CMDLINE_DTB_EXTEND
    scripts/config --set-str CMDLINE "profile=4"
    make ARCH=$ARCH CROSS_COMPILE=$CROSS_COMPILE olddefconfig
    grep -q '^CONFIG_PROFILING=y' .config || {
        echo "ERROR: CONFIG_PROFILING did not survive olddefconfig" >&2
        exit 1
    }
    grep -q '^# CONFIG_RTL8196E_IMEM_DEFAULT_PLACEMENT is not set' .config || {
        echo "ERROR: the profiling reference would retain historical I-MEM residents" >&2
        exit 1
    }
    grep -q '^CONFIG_CMDLINE="profile=4"' .config || {
        echo "ERROR: profile=4 is absent from the profiling reference command line" >&2
        exit 1
    }
    echo ""
fi

# Production-layout reference for a reopened I-MEM campaign.  Unlike the
# profiling image this changes no generic profiling option and adds no boot
# argument; it only keeps the hardware window enabled while making it empty.
if [ "${IMEM_EMPTY:-0}" = "1" ]; then
    echo "Fixing .config: empty production I-MEM window..."
    scripts/config --enable RTL8196E_IMEM
    scripts/config --disable RTL8196E_IMEM_DEFAULT_PLACEMENT
    scripts/config --disable RTL8196E_IMEM_POC_IRAM
    make ARCH=$ARCH CROSS_COMPILE=$CROSS_COMPILE olddefconfig
    grep -q '^# CONFIG_RTL8196E_IMEM_DEFAULT_PLACEMENT is not set' .config || {
        echo "ERROR: the production reference would retain historical I-MEM residents" >&2
        exit 1
    }
    echo ""
fi

# A qualified release policy uses the same empty production layout as the
# measured candidate. The selected input sections are moved only after the
# first complete link, below, so their original local slots can be retained.
if [ "$USE_IMEM_POLICY" = true ]; then
    echo "Fixing .config: release I-MEM policy $(basename "$IMEM_POLICY")..."
    scripts/config --enable RTL8196E_IMEM
    scripts/config --disable RTL8196E_IMEM_DEFAULT_PLACEMENT
    scripts/config --disable RTL8196E_IMEM_POC_IRAM
    make ARCH=$ARCH CROSS_COMPILE=$CROSS_COMPILE olddefconfig
    grep -q '^# CONFIG_RTL8196E_IMEM_DEFAULT_PLACEMENT is not set' .config || {
        echo "ERROR: release policy would retain historical I-MEM residents" >&2
        exit 1
    }
    echo ""
fi

# ── Special modes ──────────────────────────────────────────────────────────

if [ "$DO_OLDDEFCONFIG" = true ]; then
    make ARCH=$ARCH CROSS_COMPILE=$CROSS_COMPILE olddefconfig
    exit 0
fi

if [ "$DO_MENUCONFIG" = true ]; then
    make ARCH=$ARCH CROSS_COMPILE=$CROSS_COMPILE menuconfig
    exit 0
fi

# ── Build ──────────────────────────────────────────────────────────────────

POLICY_STAMP="$BUILD_DIR/.imem-policy.sha256"
POLICY_HASH=""
if [ "$USE_IMEM_POLICY" = true ]; then
    POLICY_HASH="$(sha256sum "$IMEM_POLICY" | awk '{print $1}')"
    if [ -f "$POLICY_STAMP" ] && [ "$(cat "$POLICY_STAMP")" != "$POLICY_HASH" ]; then
        echo "ERROR: I-MEM policy changed in an existing build tree; rebuild with clean" >&2
        exit 1
    fi
    # A previous incremental build leaves the transformed objects in place.
    # Restore their pristine forms before make so changed sources can rebuild;
    # apply_local_holes refreshes those backups after make, before relinking.
    python3 "${SCRIPT_DIR}/scripts/imem/apply_local_holes.py" \
        --policy "$IMEM_POLICY" --build-dir "$BUILD_DIR" --reset
elif [ -f "$POLICY_STAMP" ]; then
    echo "ERROR: this build tree contains a release I-MEM policy; rebuild with clean" >&2
    exit 1
fi

JOBS=$(nproc)
echo "Building with $JOBS parallel jobs..."
echo ""

# zboot needs xz-utils lzma, not Realtek SDK lzma (incompatible CLI)
SYS_LZMA=$(command -v lzma 2>/dev/null || true)
if [ -n "$SYS_LZMA" ] && ! "$SYS_LZMA" --help 2>&1 | grep -q "XZ Utils"; then
    SYS_LZMA=""
fi
if [ -z "$SYS_LZMA" ] && [ -x /usr/bin/lzma ]; then
    SYS_LZMA="/usr/bin/lzma"
fi
if [ -z "$SYS_LZMA" ]; then
    echo "ERROR: xz-utils not found (provides lzma for kernel compression)"
    echo "  Install: sudo apt-get install xz-utils"
    exit 1
fi

IMEM_KBUILD_LDFLAGS=""
for root in ${IMEM_HOLE_ROOTS:-}; do
    case "$root" in
        __imem_hole_[0-9][0-9][0-9][0-9]) ;;
        *) echo "ERROR: invalid I-MEM hole root: $root" >&2; exit 1 ;;
    esac
    IMEM_KBUILD_LDFLAGS="${IMEM_KBUILD_LDFLAGS} -u ${root}"
done
# Text placement pads (link-level, never executed): kept alive by -u like the
# holes, under their own name so verify_policy's hole census is unaffected.
# A tree prepared with a layout must be relinked with its roots, and a tree
# prepared without one cannot grow pads incrementally: both need `clean`.
LAYOUT_STAMP="$BUILD_DIR/.text-layout.sha256"
if [ "$USE_TEXT_LAYOUT" = true ]; then
    if [ ! -f "$LAYOUT_STAMP" ] || \
       [ "$(cat "$LAYOUT_STAMP")" != "$(sha256sum "$TEXT_LAYOUT_DIR/pads.patch" | awk '{print $1}')" ]; then
        echo "ERROR: text layout $(basename "$TEXT_LAYOUT_DIR") is not the one this tree was prepared with; rebuild with clean" >&2
        exit 1
    fi
    if [ -z "${TEXT_PAD_ROOTS:-}" ]; then
        TEXT_PAD_ROOTS="$(grep -oE '__text_pad_[0-9]{4}' "$TEXT_LAYOUT_DIR/pads.patch" | sort -u | tr '\n' ' ')"
    fi
elif [ -f "$LAYOUT_STAMP" ]; then
    echo "ERROR: this build tree carries text layout pads but the layout is disabled or absent; rebuild with clean" >&2
    exit 1
fi
for root in ${TEXT_PAD_ROOTS:-}; do
    case "$root" in
        __text_pad_[0-9][0-9][0-9][0-9]) ;;
        *) echo "ERROR: invalid text pad root: $root" >&2; exit 1 ;;
    esac
    IMEM_KBUILD_LDFLAGS="${IMEM_KBUILD_LDFLAGS} -u ${root}"
done
if [ "${VMLINUX_LINK_MAP:-0}" = "1" ]; then
    # One map per linker output: make expands the preserved automatic $@ in
    # each final-link recipe, so the zboot link cannot overwrite the vmlinux
    # map as a plain fixed -Map path would.
    IMEM_KBUILD_LDFLAGS="${IMEM_KBUILD_LDFLAGS} -Map=\$@.map"
fi

if ! make ARCH=$ARCH CROSS_COMPILE=$CROSS_COMPILE LZMA="$SYS_LZMA" \
        KBUILD_LDFLAGS="$IMEM_KBUILD_LDFLAGS" -j"$JOBS"; then
    echo ""
    echo "=== BUILD FAILED ==="
    exit 1
fi

if [ "$USE_IMEM_POLICY" = true ]; then
    python3 "${SCRIPT_DIR}/scripts/imem/apply_local_holes.py" \
        --policy "$IMEM_POLICY" --build-dir "$BUILD_DIR" \
        --report "$BUILD_DIR/.imem-policy-report.json"
    policy_count="$(awk -F '\t' '!/^#/ && NF == 2 {n++} END {print n+0}' "$IMEM_POLICY")"
    policy_roots=""
    index=0
    while [ "$index" -lt "$policy_count" ]; do
        policy_roots="$policy_roots -u $(printf '__imem_hole_%04d' "$index")"
        index=$((index + 1))
    done
    echo "Relinking with release I-MEM policy ($policy_count sections)..."
    if ! make ARCH=$ARCH CROSS_COMPILE=$CROSS_COMPILE LZMA="$SYS_LZMA" \
            KBUILD_LDFLAGS="$IMEM_KBUILD_LDFLAGS $policy_roots" -j"$JOBS"; then
        echo "ERROR: I-MEM policy relink failed" >&2
        exit 1
    fi
    python3 "${SCRIPT_DIR}/scripts/imem/verify_policy.py" \
        --build-dir "$BUILD_DIR" --policy "$IMEM_POLICY" \
        --report "$BUILD_DIR/.imem-policy-report.json" --cross "$CROSS_COMPILE"
    printf '%s\n' "$POLICY_HASH" >"$POLICY_STAMP"
fi

if [ "$USE_TEXT_LAYOUT" = true ] && [ "${TEXT_LAYOUT_RECORD:-0}" != "1" ]; then
    if [ ! -f "$TEXT_LAYOUT_DIR/layout.json" ]; then
        echo "ERROR: $(basename "$TEXT_LAYOUT_DIR")/layout.json is missing — record it from an accepted build" >&2
        echo "  (TEXT_LAYOUT_RECORD=1 builds without the guard for that purpose)" >&2
        exit 1
    fi
    echo "Verifying text layout $(basename "$TEXT_LAYOUT_DIR")..."
    python3 "${SCRIPT_DIR}/scripts/imem/text_layout.py" verify \
        --build-dir "$BUILD_DIR" --layout "$TEXT_LAYOUT_DIR/layout.json" \
        --pads-patch "$TEXT_LAYOUT_DIR/pads.patch" --cross "$CROSS_COMPILE" || exit 1
fi

echo ""
echo "=== COMPILATION OK ==="
echo ""

# I-MEM is filled once from its SDRAM shadow at boot and is not coherent with
# later text writes.  A runtime patch site in the window would therefore be
# changed in SDRAM while the CPU kept executing the stale SRAM instruction.
# This is a hardware invariant, so gate every I-MEM build, independently of
# whether it uses the historical placement or an optimized manifest.
if grep -q '^CONFIG_RTL8196E_IMEM=y' .config; then
    python3 "${SCRIPT_DIR}/scripts/imem/scan_dynamic_code.py" \
        --reference "$BUILD_DIR" --config "$BUILD_DIR/.config" \
        --cross "$CROSS_COMPILE" --gate-window
    echo ""
fi

# ── Staleness guard ────────────────────────────────────────────────────────
# "COMPILATION OK" only means make had nothing left to do — NOT that the
# overlay actually reached the binary. The overlay is re-synced with
# `rsync -a`, which PRESERVES mtimes: an object left behind by a manual
# `make drivers/.../foo.o` can be NEWER than the freshly synced source, so
# make skips it and the image silently ships the old code (hit 2026-07-25:
# a v3.4 driver shipped inside an image believed to be v3.5).
# Assert here on the linked binary, not on the sources.
# Usage: EXPECT_IN_VMLINUX='some string' ./build_kernel.sh
if [ -n "${EXPECT_IN_VMLINUX:-}" ]; then
    if strings -a vmlinux | grep -qF -- "$EXPECT_IN_VMLINUX"; then
        echo "vmlinux contient bien: $EXPECT_IN_VMLINUX"
    else
        echo ""
        echo "=== STALE BUILD ==="
        echo "vmlinux ne contient PAS: $EXPECT_IN_VMLINUX"
        echo "L'objet concerne est probablement plus recent que sa source."
        echo "Remede: rm l'objet dans $BUILD_DIR, touch la source dans"
        echo "${FILES_DIR}, puis relancer."
        exit 1
    fi
fi

if [ "$BUILD_VMLINUX_ONLY" = true ]; then
    ls -lh vmlinux
    exit 0
fi

# ── Packaging (zboot) ──────────────────────────────────────────────────────

if [ -z "$CVIMG" ]; then
    echo "WARNING: cvimg not found; skipping image creation."
    echo "  Build cvimg in: ${BUILD_ENV}/"
    exit 0
fi

mkdir -p "$(dirname "$IMAGE")"
rm -f "$IMAGE"
echo "Packaging (zboot)..."

VMLINUZ_ELF="vmlinuz"

if [ ! -f "$VMLINUZ_ELF" ]; then
    echo "ERROR: vmlinuz not found — is CONFIG_SYS_SUPPORTS_ZBOOT active?"
    exit 1
fi

# Extract entry point; normalize to 32 bits (readelf may sign-extend on x86-64)
VMLINUZ_ENTRY_RAW=$(${CROSS_COMPILE}readelf -h "$VMLINUZ_ELF" \
    | awk '/Entry point address/ {print $NF}')
VMLINUZ_ENTRY=$(printf "0x%08x" $(( VMLINUZ_ENTRY_RAW & 0xffffffff )) 2>/dev/null \
    || python3 -c "print(hex(int('${VMLINUZ_ENTRY_RAW}',16)&0xffffffff))")

echo "  vmlinuz ELF  : $VMLINUZ_ELF"
echo "  vmlinuz entry: $VMLINUZ_ENTRY"

${CROSS_COMPILE}objcopy -O binary \
    -R .reginfo -R .note -R .comment -R .mdebug -S \
    "$VMLINUZ_ELF" vmlinuz.bin

vmlinuz_size=$(stat -c%s vmlinuz.bin)
vmlinux_size=$(stat -c%s vmlinux)

$CVIMG \
    -i vmlinuz.bin \
    -o "$IMAGE" \
    -s "$SIGNATURE" \
    -e "$VMLINUZ_ENTRY" \
    -b "$CVIMG_BURN_ADDR" \
    -a 4k >/dev/null

img_size=$(stat -c%s "$IMAGE")

echo ""
echo "  vmlinux      : $(numfmt --to=iec-i --suffix=B "$vmlinux_size")"
echo "  vmlinuz.bin  : $(numfmt --to=iec-i --suffix=B "$vmlinuz_size")  (decompressor + LZMA kernel)"
echo "  Final image  : $(numfmt --to=iec-i --suffix=B "$img_size")"
echo ""
echo "Image ready: $IMAGE"
if [ "$BOARD" = "lidl" ] && [ "$KERNEL_MAJOR_MINOR" = "6.18" ]; then
    echo "Flash with:  ./flash_kernel.sh"
else
    echo "Flash with:  BOARD=${BOARD} KERNEL=${KERNEL_MAJOR_MINOR} ./flash_kernel.sh"
fi
