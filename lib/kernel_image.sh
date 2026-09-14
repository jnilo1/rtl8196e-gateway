# lib/kernel_image.sh — resolve the pre-built per-board images: the kernel for
# a (board, kernel) pair to the kernel-img/<board>/kernel-<kernel>.img layout
# under 32-Kernel/, and the bootloader for a board to the
# boot-img/<board>/boot.bin layout under 31-Bootloader/.
#
# Sourced by the flash/build scripts that consume a pre-built image:
#   - build_fullflash.sh, create_fullflash.sh         (repo root)
#   - flash_install_rtl8196e.sh                        (repo root, via build_fullflash)
#   - 3-Main-SoC-Realtek-RTL8196E/flash_remote.sh
#   - 3-Main-SoC-Realtek-RTL8196E/32-Kernel/flash_kernel.sh
#   - 3-Main-SoC-Realtek-RTL8196E/31-Bootloader/flash_bootloader.sh
#
# Single source of truth for the supported boards and kernel lines; keep in
# sync with 32-Kernel/build_kernel.sh and 31-Bootloader/build_bootloader.sh,
# which produce the images into the same per-board slots. Intentionally not
# executable; this file is only meant to be sourced.

# Supported values. The default board is lidl and the default kernel line 6.18;
# together they reproduce the historical single-image (kernel-6.18.img) path, so
# a Lidl user who sets neither BOARD nor KERNEL gets exactly the old behaviour.
KERNEL_IMG_KNOWN_BOARDS="lidl sengled-e39-g8c"
KERNEL_IMG_KNOWN_KERNELS="6.18 7.2"
KERNEL_IMG_DEFAULT_BOARD="lidl"
KERNEL_IMG_DEFAULT_KERNEL="6.18"

# Absolute path to the kernel-img/ tree, computed from this file's location
# (lib/ sits at the repo root, 32-Kernel is a fixed relative path). Works
# regardless of the caller's CWD or how deep the sourcing script lives.
_kernel_img_lib_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_IMG_ROOT="$(cd "${_kernel_img_lib_dir}/.." && pwd)/3-Main-SoC-Realtek-RTL8196E/32-Kernel/kernel-img"
BOOT_IMG_ROOT="$(cd "${_kernel_img_lib_dir}/.." && pwd)/3-Main-SoC-Realtek-RTL8196E/31-Bootloader/boot-img"
# Per-board bootloader headers, read by the board guard below for the DRAM
# bring-up constants — the board facts live there, not here.
BOARD_H_ROOT="$(cd "${_kernel_img_lib_dir}/.." && pwd)/3-Main-SoC-Realtek-RTL8196E/31-Bootloader/boards"
unset _kernel_img_lib_dir

# _kernel_img_in_list <needle> <space-separated-haystack> — 0 if present.
_kernel_img_in_list() {
    local needle="$1" item
    for item in $2; do
        [ "$item" = "$needle" ] && return 0
    done
    return 1
}

# kernel_image_validate <board> <kernel> — vet the pair without touching disk.
# Empty arguments fall back to the defaults. Prints an actionable message on
# >&2 and returns non-zero for an unknown board or kernel line.
kernel_image_validate() {
    local board="${1:-$KERNEL_IMG_DEFAULT_BOARD}"
    local kernel="${2:-$KERNEL_IMG_DEFAULT_KERNEL}"
    if ! _kernel_img_in_list "$board" "$KERNEL_IMG_KNOWN_BOARDS"; then
        echo "Error: unknown BOARD '$board' (known: ${KERNEL_IMG_KNOWN_BOARDS})." >&2
        return 1
    fi
    if ! _kernel_img_in_list "$kernel" "$KERNEL_IMG_KNOWN_KERNELS"; then
        echo "Error: unknown KERNEL '$kernel' (known: ${KERNEL_IMG_KNOWN_KERNELS})." >&2
        return 1
    fi
    return 0
}

# resolve_kernel_image <board> <kernel> — echo the absolute image path for the
# pair, validating the values and that the file exists. Empty arguments fall
# back to lidl / 6.18. On failure, nothing is echoed and a message explaining
# how to build the missing image goes to >&2.
resolve_kernel_image() {
    local board="${1:-$KERNEL_IMG_DEFAULT_BOARD}"
    local kernel="${2:-$KERNEL_IMG_DEFAULT_KERNEL}"

    kernel_image_validate "$board" "$kernel" || return 1

    local img="${KERNEL_IMG_ROOT}/${board}/kernel-${kernel}.img"
    if [ ! -f "$img" ]; then
        echo "Error: kernel image not found: ${img}" >&2
        echo "  Build it: (cd 3-Main-SoC-Realtek-RTL8196E/32-Kernel && BOARD=${board} KERNEL=${kernel} ./build_kernel.sh)" >&2
        return 1
    fi
    printf '%s\n' "$img"
}

# resolve_boot_image <board> — echo the absolute path of the pre-built
# bootloader for the board, validating the value and that the file exists.
# An empty argument falls back to lidl. The bootloader has no kernel-line
# dimension: one boot.bin per board, board-specific DRAM bring-up baked in —
# flashing the wrong board's bootloader bricks the gateway, which is why the
# binary is resolved from BOARD here instead of being a shared mutable file
# (discussion #140). On failure, nothing is echoed and a message explaining
# how to build the missing image goes to >&2.
resolve_boot_image() {
    local board="${1:-$KERNEL_IMG_DEFAULT_BOARD}"
    if ! _kernel_img_in_list "$board" "$KERNEL_IMG_KNOWN_BOARDS"; then
        echo "Error: unknown BOARD '$board' (known: ${KERNEL_IMG_KNOWN_BOARDS})." >&2
        return 1
    fi
    local img="${BOOT_IMG_ROOT}/${board}/boot.bin"
    if [ ! -f "$img" ]; then
        echo "Error: bootloader image not found: ${img}" >&2
        echo "  Build it: (cd 3-Main-SoC-Realtek-RTL8196E/31-Bootloader && BOARD=${board} ./build_bootloader.sh)" >&2
        return 1
    fi
    printf '%s\n' "$img"
}

# --- board identity guard -----------------------------------------------------
#
# Confirm the gateway is the board whose images are about to be written. Two
# sources, cheapest first:
#
#   1. /proc/device-tree/model — per-board since the device tree began
#      describing board wiring. Conclusive when it names a board we know.
#   2. the DDR controller registers 0x18001004 / 0x18001008 — btcode/start.S
#      writes them at bring-up from boards/<board>/board.h and nothing rewrites
#      them afterwards, so they identify the board on EVERY firmware generation.
#      This is what makes the guard work on old gateways: releases older than
#      the per-board model strings all report the generic "Realtek RTL8196E SoC"
#      and were refused outright, which blocked the documented upgrade command
#      on every gateway that had not been updated since. The registers answer
#      where the string cannot.
#
# The register values are read out of board.h at call time and never copied
# here: adding a board means adding its board.h, not editing a second list.
# Note what the registers do and do not prove — they describe the bring-up the
# INSTALLED BOOTLOADER performs, which is the thing a full flash replaces and
# the thing that bricks when it does not match the DRAM. A board already
# running someone else's bring-up reports that one; re-flashing the same board
# keeps it where it is, and moving it needs --force, deliberately.

# board_guard_model_sig <board> — substring identifying the board in
# /proc/device-tree/model.
board_guard_model_sig() {
    case "$1" in
        lidl)            echo "Lidl" ;;
        sengled-e39-g8c) echo "Sengled" ;;
    esac
}

# board_guard_ddr_pair <board> — "0X1004VALUE 0X1008VALUE" from board.h, upper
# case for comparison. Nothing echoed (and non-zero) when board.h is missing.
board_guard_ddr_pair() {
    local bh="${BOARD_H_ROOT}/${1}/board.h" a b
    [ -f "$bh" ] || return 1
    a="$(awk '$2=="BOARD_DDR_REG_1004"{print $3; exit}' "$bh")"
    b="$(awk '$2=="BOARD_DDR_REG_1008"{print $3; exit}' "$bh")"
    [ -n "$a" ] && [ -n "$b" ] || return 1
    printf '%s %s' "$a" "$b" | tr '[:lower:]' '[:upper:]'
}

# _board_guard_refuse <board> <what we found> <the board it points at>
_board_guard_refuse() {
    echo "Error: board mismatch — selected BOARD='$1', but $2." >&2
    echo "  A board-specific kernel will not boot correctly, and a mismatched" >&2
    echo "  bootloader bricks the gateway (per-board DRAM bring-up)." >&2
    echo "  Re-run with BOARD=$3, or pass --force to override." >&2
}

# board_guard_check <board> <ssh-target> [ssh-opts...]
#   0 — confirmed, proceed
#   1 — positive mismatch: something on the gateway names ANOTHER known board
#   2 — inconclusive: nothing on the gateway names any board we know. The
#       caller decides; the two entry points differ and both are deliberate.
board_guard_check() {
    local board="$1" target="$2"; shift 2
    local model sig other osig want pair

    sig="$(board_guard_model_sig "$board")"
    # cat runs on the gateway (BusyBox has no tr); the trailing NUL is stripped
    # host-side.
    model="$(ssh_retry "$@" "$target" "cat /proc/device-tree/model" 2>/dev/null | tr -d '\0' || true)"

    if [ -n "$model" ] && [ -n "$sig" ] && printf '%s' "$model" | grep -q "$sig"; then
        return 0
    fi

    if [ -n "$model" ]; then
        for other in $KERNEL_IMG_KNOWN_BOARDS; do
            [ "$other" = "$board" ] && continue
            osig="$(board_guard_model_sig "$other")"
            [ -n "$osig" ] || continue
            if printf '%s' "$model" | grep -q "$osig"; then
                _board_guard_refuse "$board" "the gateway's device tree says \"$model\"" "$other"
                return 1
            fi
        done
    fi

    # The model names no board we know — an older release, where it is the
    # generic SoC string. Ask the DRAM controller the bootloader configured.
    want="$(board_guard_ddr_pair "$board" || true)"
    pair="$(ssh_retry "$@" "$target" 'devmem 0x18001004; devmem 0x18001008' 2>/dev/null \
            | tr -d '\r' | tr '\n' ' ' | tr '[:lower:]' '[:upper:]' | sed 's/[[:space:]]*$//')"

    if [ -n "$want" ] && [ "$pair" = "$want" ]; then
        echo "Board confirmed by its DRAM bring-up ($pair): ${board}."
        echo "  (this firmware predates per-board model strings — it reports \"$model\")"
        return 0
    fi

    if [ -n "$pair" ]; then
        for other in $KERNEL_IMG_KNOWN_BOARDS; do
            [ "$other" = "$board" ] && continue
            if [ "$pair" = "$(board_guard_ddr_pair "$other" || true)" ]; then
                _board_guard_refuse "$board" \
                    "its DRAM controller is brought up for ${other} ($pair)" "$other"
                return 1
            fi
        done
    fi

    echo "Note: cannot confirm this gateway is a '${board}' board." >&2
    if [ -n "$model" ]; then
        echo "  Its device tree reports \"$model\", which names no board this tree knows" >&2
        echo "  (releases older than per-board model strings all report the generic SoC)." >&2
    else
        echo "  Its device tree model could not be read." >&2
    fi
    if [ -n "$pair" ]; then
        echo "  Its DRAM bring-up ($pair) matches no board either." >&2
    else
        echo "  Its DRAM bring-up could not be read (no devmem?)." >&2
    fi
    return 2
}
