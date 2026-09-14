#!/bin/bash
# extract_patches.sh — Extract patches and new files from a patched Linux kernel.
#
# Compares a patched kernel tree against a vanilla tree of the same version
# and generates:
#   - ${OUTPUT_FILES}/    : new files added by the overlay
#   - ${OUTPUT_PATCHES}/  : unified diffs of modified files
#
# Usage:
#   ./scripts/extract_patches.sh [OPTIONS] [patched_kernel_dir]
#
# Options:
#   -v, --kernel-version V  Kernel version (default: 6.18)
#   -o, --output-files D    Output dir for new files (default: files)
#   -p, --output-patches D  Output dir for patches (default: patches)
#   -f, --force             Skip interactive confirmation (required for non-tty)
#   -h, --help              Show this help
#
# Environment (overridden by flags):
#   KERNEL_VERSION, OUTPUT_FILES, OUTPUT_PATCHES
#
# Examples:
#   # Extract 6.18 port (default)
#   ./scripts/extract_patches.sh -o files-6.18 -p patches-6.18 linux-6.18-rtl8196e
#
# Safeguards:
#   - Refuses to overwrite git-tracked content in OUTPUT_FILES/OUTPUT_PATCHES
#     without --force. This prevents accidents like running against the wrong
#     tree and clobbering production patches.
#   - Requires --force when stdin is not a tty (pipe/redirect) so that
#     confirmation cannot be silently bypassed.
#
# J. Nilo — December 2025, revised April 2026

set -e
export LC_ALL=C

# ── Defaults ───────────────────────────────────────────────────────────────

KERNEL_VERSION="${KERNEL_VERSION:-6.18}"
OUTPUT_FILES="${OUTPUT_FILES:-files-6.18}"
OUTPUT_PATCHES="${OUTPUT_PATCHES:-patches-6.18}"
FORCE=false
PATCHED_DIR=""

GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

usage() {
    sed -n '2,36p' "$0" | sed 's|^# \{0,1\}||'
    exit "${1:-0}"
}

# ── Argument parsing ───────────────────────────────────────────────────────

while [ $# -gt 0 ]; do
    case "$1" in
        -v|--kernel-version)
            KERNEL_VERSION="$2"; shift 2 ;;
        --kernel-version=*)
            KERNEL_VERSION="${1#*=}"; shift ;;
        -o|--output-files)
            OUTPUT_FILES="$2"; shift 2 ;;
        --output-files=*)
            OUTPUT_FILES="${1#*=}"; shift ;;
        -p|--output-patches)
            OUTPUT_PATCHES="$2"; shift 2 ;;
        --output-patches=*)
            OUTPUT_PATCHES="${1#*=}"; shift ;;
        -f|--force)
            FORCE=true; shift ;;
        -h|--help)
            usage 0 ;;
        -*)
            echo -e "${RED}Unknown option: $1${NC}" >&2; usage 1 ;;
        *)
            if [ -n "$PATCHED_DIR" ]; then
                echo -e "${RED}Too many positional arguments${NC}" >&2; usage 1
            fi
            PATCHED_DIR="$1"; shift ;;
    esac
done

# Derive KERNEL_MAJOR from KERNEL_VERSION ("6.18" → "6.x", "6.18.2" → "6.x")
KERNEL_MAJOR="${KERNEL_VERSION%%.*}.x"
KERNEL_TARBALL="linux-${KERNEL_VERSION}.tar.xz"
KERNEL_BASE_URL="${KERNEL_MIRROR:-https://cdn.kernel.org/pub/linux/kernel}"   # KERNEL_MIRROR: see build_kernel.sh
KERNEL_URL="${KERNEL_BASE_URL%/}/v${KERNEL_MAJOR}/${KERNEL_TARBALL}"
VANILLA_DIR="linux-${KERNEL_VERSION}"
PATCHED_DIR_DEFAULT="linux-${KERNEL_VERSION}-rtl8196e"
PATCHED_DIR="${PATCHED_DIR:-$PATCHED_DIR_DEFAULT}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
KERNEL_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "$KERNEL_DIR"

# ── Preflight ──────────────────────────────────────────────────────────────

if [ ! -d "$PATCHED_DIR" ]; then
    echo -e "${RED}Error: patched kernel directory not found: $PATCHED_DIR${NC}" >&2
    exit 1
fi
PATCHED_DIR="$(cd "$PATCHED_DIR" && pwd)"

# Refuse to operate from a non-tty stdin unless --force is explicit.
# This prevents `yes y | script` style bypass where env vars may be mis-propagated.
if [ ! -t 0 ] && [ "$FORCE" != true ]; then
    echo -e "${RED}Error: stdin is not a terminal; use --force to confirm overwrite.${NC}" >&2
    exit 1
fi

echo "==================================================================="
echo "  Extract patches from Linux ${KERNEL_VERSION}"
echo "==================================================================="
echo ""
echo "  Patched kernel : $PATCHED_DIR"
echo "  Vanilla kernel : ${KERNEL_DIR}/${VANILLA_DIR}"
echo "  Output files   : ${KERNEL_DIR}/${OUTPUT_FILES}/"
echo "  Output patches : ${KERNEL_DIR}/${OUTPUT_PATCHES}/"
echo ""

# ── Safety: refuse to clobber git-tracked content without --force ──────────

check_tracked() {
    local dir="$1"
    [ -d "$dir" ] || return 0
    # Are any files under $dir tracked by git?
    if command -v git >/dev/null 2>&1 && git -C "$KERNEL_DIR" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        local tracked
        tracked=$(git -C "$KERNEL_DIR" ls-files --error-unmatch -- "$dir" 2>/dev/null | head -1 || true)
        if [ -n "$tracked" ]; then
            return 1
        fi
    fi
    return 0
}

for d in "$OUTPUT_FILES" "$OUTPUT_PATCHES"; do
    if ! check_tracked "$d"; then
        if [ "$FORCE" != true ]; then
            echo -e "${RED}Error: '$d' contains git-tracked files.${NC}" >&2
            echo -e "${RED}Refusing to overwrite production content without --force.${NC}" >&2
            echo -e "${YELLOW}Hint: use distinct output dirs (e.g. -o files-6.18 -p patches-6.18)${NC}" >&2
            exit 1
        else
            echo -e "${YELLOW}WARNING: '$d' contains git-tracked files; --force given, proceeding.${NC}"
        fi
    fi
done

# Interactive confirmation only when tty AND at least one output dir exists
if [ -t 0 ] && { [ -d "$OUTPUT_FILES" ] || [ -d "$OUTPUT_PATCHES" ]; } && [ "$FORCE" != true ]; then
    echo -e "${YELLOW}Output directories exist and will be overwritten.${NC}"
    read -r -p "Continue? (y/n) " REPLY
    if [[ ! "$REPLY" =~ ^[Yy]$ ]]; then
        echo "Aborted."
        exit 0
    fi
fi

# ── Fetch + extract vanilla kernel ─────────────────────────────────────────

if [ ! -f "$KERNEL_TARBALL" ]; then
    echo -e "${YELLOW}Downloading vanilla Linux ${KERNEL_VERSION}...${NC}"
    wget -q --show-progress "$KERNEL_URL"
fi

echo -e "${YELLOW}Extracting vanilla kernel...${NC}"
rm -rf "$VANILLA_DIR"
tar xf "$KERNEL_TARBALL"
echo ""

# ── Prepare output dirs ────────────────────────────────────────────────────

rm -rf "$OUTPUT_FILES" "$OUTPUT_PATCHES"
mkdir -p "$OUTPUT_FILES" "$OUTPUT_PATCHES"

echo -e "${YELLOW}Comparing patched kernel with vanilla...${NC}"
echo ""

# ── should_skip: filter out build artifacts and generated files ────────────

should_skip() {
    local file="$1"
    local basename
    basename=$(basename "$file")

    # Hidden files (.config, .*.cmd, .version, etc.)
    [[ "$basename" =~ ^\. ]] && return 0

    case "$file" in
        # Build artifacts
        *.o|*.ko|*.mod|*.mod.c|*.a|*.cmd|*.rej|*.orig|*.tmp|*.tmp.*)
            return 0 ;;
        *.dtb|*.dtb.S|*.dtbo)
            return 0 ;;
        # vmlinux, System.map, modules.*
        vmlinux|vmlinux.*|System.map|modules.*)
            return 0 ;;
        */built-in.a|*/Module.symvers|*/modules.order)
            return 0 ;;
        # Generated lex/yacc
        *.lex.c|*.tab.c|*.tab.h)
            return 0 ;;
        # Kbuild-generated headers and object lists
        scripts/mod/devicetable-offsets.*|scripts/mod/elfconfig.h)
            return 0 ;;
        arch/*/boot/compressed/bswapsi.c)
            return 0 ;;
        # Various generated artifacts observed across 5.x and 6.x
        kernel/config_data*|lib/crc32table.h)
            return 0 ;;
        # 6.x additions
        drivers/of/empty_root.dtb.S)
            return 0 ;;
        init/utsversion-tmp.h)
            return 0 ;;
        kernel/sched/rq-offsets.s)
            return 0 ;;
        # Kbuild DTB list (6.x) — one per dir containing DT sources
        */dtbs-list|dtbs-list)
            return 0 ;;
        # Build outputs from arch/*/boot/compressed/ (vmlinux.bin.z etc.)
        arch/*/boot/compressed/vmlinux.*|arch/*/boot/compressed/piggy.*)
            return 0 ;;
        # Assembler output / kbuild intermediates
        *.s|*.S.tmp|*.builtin-dtbs|*.builtin-dtbs.*)
            return 0 ;;
    esac

    return 1
}

# Detect binary files that should not be diffed or copied
is_binary() {
    local f="$1"
    file -b "$f" 2>/dev/null | grep -qE '^ELF|^PE32|^Mach-O|^PNG|^JPEG|^GIF|^data$'
}

# ── Run diff -rq ───────────────────────────────────────────────────────────

DIFF_OUTPUT=$(mktemp)
trap 'rm -f "$DIFF_OUTPUT"' EXIT

# NOTE: --exclude matches basenames only (not paths)
diff -rq \
    --exclude='*.o' --exclude='*.ko' --exclude='*.mod' --exclude='*.mod.c' \
    --exclude='.*.cmd' --exclude='*.cmd' \
    --exclude='*.rej' --exclude='*.orig' --exclude='*.tmp' --exclude='.tmp_*' \
    --exclude='modules.order' --exclude='Module.symvers' \
    --exclude='.config' --exclude='.config.old' \
    --exclude='vmlinux' --exclude='vmlinux.bin' --exclude='vmlinux.bin.lzma' \
    --exclude='vmlinux.symvers' --exclude='vmlinux.lds' \
    --exclude='System.map' --exclude='*.img' \
    --exclude='auto.conf' --exclude='auto.conf.cmd' --exclude='tristate.conf' \
    --exclude='*.a' --exclude='built-in.a' \
    --exclude='*.dtb' --exclude='*.dtbo' \
    --exclude='asm-offsets.s' --exclude='rq-offsets.s' \
    --exclude='vdso-image.c' --exclude='vdso.lds' \
    --exclude='.version' --exclude='compile.h' --exclude='utsrelease.h' \
    --exclude='utsversion-tmp.h' --exclude='bounds.s' \
    --exclude='.missing-syscalls.d' \
    --exclude='.git' --exclude='.gitignore' \
    --exclude='generated' --exclude='config' \
    --exclude='modules.builtin*' --exclude='config_data*' --exclude='crc32table.h' \
    --exclude='*.lex.c' --exclude='*.tab.c' --exclude='*.tab.h' \
    --exclude='fixdep' --exclude='conf' --exclude='modpost' --exclude='dtc' \
    --exclude='empty_root.dtb.S' \
    --exclude='dtbs-list' \
    --exclude='vmlinux.bin.z' \
    --exclude='piggy.o' \
    --exclude='piggy.S' \
    "$VANILLA_DIR" "$PATCHED_DIR" > "$DIFF_OUTPUT" 2>/dev/null || true

# ── Process diff output ────────────────────────────────────────────────────

while IFS= read -r line; do
    if [[ "$line" =~ ^Only\ in\ ${PATCHED_DIR}(.*):[[:space:]](.+)$ ]]; then
        subdir="${BASH_REMATCH[1]}"
        filename="${BASH_REMATCH[2]}"
        rel_path="${subdir#/}/$filename"
        rel_path="${rel_path#/}"

        should_skip "$rel_path" && continue

        src_file="$PATCHED_DIR$subdir/$filename"
        is_binary "$src_file" && continue

        if [ -d "$src_file" ]; then
            find "$src_file" -type f | while read -r f; do
                rel="${f#$PATCHED_DIR/}"
                should_skip "$rel" && continue
                is_binary "$f" && continue
                dest_dir="${OUTPUT_FILES}/$(dirname "$rel")"
                mkdir -p "$dest_dir"
                cp "$f" "$dest_dir/"
                echo "  [NEW] $rel"
            done
        else
            dest_dir="${OUTPUT_FILES}${subdir}"
            mkdir -p "$dest_dir"
            cp "$src_file" "$dest_dir/"
            echo "  [NEW] $rel_path"
        fi

    elif [[ "$line" =~ ^Files\ (.+)\ and\ (.+)\ differ$ ]]; then
        vanilla_file="${BASH_REMATCH[1]}"
        patched_file="${BASH_REMATCH[2]}"
        rel_path="${patched_file#$PATCHED_DIR/}"

        should_skip "$rel_path" && continue
        is_binary "$patched_file" && continue

        # a/b/c.h -> a-b-c.h.patch
        patch_name="$(echo "$rel_path" | tr '/' '-').patch"

        diff -u "$vanilla_file" "$patched_file" | \
            sed "1s|^--- .*|--- a/$rel_path|; 2s|^+++ .*|+++ b/$rel_path|" \
            > "${OUTPUT_PATCHES}/$patch_name" 2>/dev/null || true

        echo "  [MOD] $rel_path"
    fi
done < "$DIFF_OUTPUT"

# ── Copy extra loose files (config, top-level scripts) from original files/ ─

ORIGINAL_FILES_DIR="$(dirname "$PATCHED_DIR")/files"
if [ -d "$ORIGINAL_FILES_DIR" ] && [ "$ORIGINAL_FILES_DIR" != "$(realpath "$OUTPUT_FILES" 2>/dev/null)" ]; then
    echo ""
    echo -e "${YELLOW}Copying extra files from original files/ directory...${NC}"
    for f in "$ORIGINAL_FILES_DIR"/config-*.txt "$ORIGINAL_FILES_DIR"/*.sh; do
        [ -f "$f" ] || continue
        cp "$f" "$OUTPUT_FILES/"
        echo "  [EXTRA] $(basename "$f")"
    done
fi

# ── Results ────────────────────────────────────────────────────────────────

NEW_FILES=$(find "$OUTPUT_FILES" -type f 2>/dev/null | wc -l)
MODIFIED_FILES=$(find "$OUTPUT_PATCHES" -name '*.patch' 2>/dev/null | wc -l)

echo ""
echo "==================================================================="
echo "  EXTRACTION COMPLETE"
echo "==================================================================="
echo ""
echo "  New files:      $NEW_FILES (in ${OUTPUT_FILES}/)"
echo "  Modified files: $MODIFIED_FILES (in ${OUTPUT_PATCHES}/)"
echo ""

# Cleanup vanilla sources
rm -rf "$VANILLA_DIR"
rm -f "$KERNEL_TARBALL"

echo -e "${GREEN}Done!${NC}"
