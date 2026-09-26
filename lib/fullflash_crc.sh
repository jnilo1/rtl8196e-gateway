#!/bin/bash
# fullflash_crc.sh — the CRC trailer of a raw 16 MiB fullflash image
#
# The bootloader (V3.1, audit S3.2) checks a 16-byte trailer in the last 16
# bytes of the bootloader partition, 0x1FFF0..0x1FFFF — a region the loader
# never uses and JFFS2 never touches:
#
#   "FFCS" | image length (BE32) | CRC-32 of the image minus these 16 bytes (BE32) | ~CRC (BE32)
#
# The V3.1 loader requires the trailer: an image without one (16 x 0xFF),
# with foreign bytes there, or whose CRC does not match is refused before
# anything is erased.  Loaders older than V3.1 ignore the trailer and write
# it to flash as ordinary data, so a signed image suits every loader.  An
# older unsigned image (one built or backed up before V3.1) is made
# acceptable by `lib/fullflash_crc.sh write FILE`.
#
# Used by build_fullflash.sh / create_fullflash.sh (write), backup_gateway.sh
# (write — the flash content changed since the image was built, so the
# trailer read back from the chip is stale and must be recomputed) and
# restore_gateway.sh (check before sending).
#
# Sourced: ffcrc_write FILE, ffcrc_check FILE, ffcrc_crc32 FILE.
# Standalone: lib/fullflash_crc.sh write|check|crc FILE.
#
# The CRC-32 is the ISO 3309 / zlib one.  It is taken from gzip's own
# trailer (CRC-32 + size, little-endian, last 8 bytes of a .gz), which makes
# gzip the only dependency — always present where dd and od are.

FFCRC_OFFSET=$((0x1FFF0))
FFCRC_LEN=16
FFCRC_IMAGE_SIZE=$((16 * 1024 * 1024))

# ffcrc_crc32 FILE — CRC-32 of the image minus the trailer bytes, 8 hex digits
ffcrc_crc32() {
    { head -c "$FFCRC_OFFSET" "$1"; tail -c +$((FFCRC_OFFSET + FFCRC_LEN + 1)) "$1"; } \
        | gzip -1 -c | tail -c 8 | od -An -tx1 -v \
        | awk '{printf "%s%s%s%s\n", $4, $3, $2, $1}'
}

# _ffcrc_be32 VALUE — the 4 bytes of a 32-bit value, big-endian
_ffcrc_be32() {
    printf "\\x$(printf %02x $((($1 >> 24) & 255)))\\x$(printf %02x $((($1 >> 16) & 255)))\\x$(printf %02x $((($1 >> 8) & 255)))\\x$(printf %02x $(($1 & 255)))"
}

# _ffcrc_size_ok FILE — 0 when the file is exactly 16 MiB
_ffcrc_size_ok() {
    [ "$(stat -c%s "$1" 2>/dev/null || echo 0)" -eq "$FFCRC_IMAGE_SIZE" ]
}

# ffcrc_write FILE — compute the CRC and write the trailer in place
ffcrc_write() {
    local file="$1" crc
    _ffcrc_size_ok "$file" || { echo "fullflash_crc: $file is not a 16 MiB image" >&2; return 1; }
    crc=$((16#$(ffcrc_crc32 "$file")))
    { printf 'FFCS'; _ffcrc_be32 "$FFCRC_IMAGE_SIZE"; _ffcrc_be32 "$crc"; _ffcrc_be32 $(( (~crc) & 0xFFFFFFFF )); } \
        | dd of="$file" bs=1 seek="$FFCRC_OFFSET" conv=notrunc 2>/dev/null
    printf '%08x\n' "$crc"
}

# ffcrc_check FILE — 0 valid trailer, 1 no trailer (refused by the V3.1
# loader, accepted by older ones), 2 CRC mismatch, 3 foreign data where the
# trailer should be (2 and 3 are refused by the loader).  Prints one line on
# stdout.
# Callers under set -e: use `if note=$(ffcrc_check f); then rc=0; else rc=$?; fi`.
ffcrc_check() {
    local file="$1" t magic len crc ncrc got
    _ffcrc_size_ok "$file" || { echo "fullflash_crc: $file is not a 16 MiB image" >&2; return 2; }
    t=$(dd if="$file" bs=1 skip="$FFCRC_OFFSET" count="$FFCRC_LEN" 2>/dev/null | od -An -tx1 -v | tr -d ' \n')
    if [ "$t" = "ffffffffffffffffffffffffffffffff" ]; then
        echo "no CRC trailer (built before V3.1, or a backup of such a flash): a V3.1 loader will refuse the image; sign it with 'lib/fullflash_crc.sh write' if you trust it (older loaders accept it as is)"
        return 1
    fi
    magic=${t:0:8}; len=$((16#${t:8:8})); crc=$((16#${t:16:8})); ncrc=$((16#${t:24:8}))
    if [ "$magic" != "46464353" ] || [ "$len" -ne "$FFCRC_IMAGE_SIZE" ] || [ $(( (crc ^ ncrc) & 0xFFFFFFFF )) -ne $((0xFFFFFFFF)) ]; then
        echo "unrecognised data at 0x1FFF0 (not a CRC trailer): the loader will refuse the image; sign it with 'lib/fullflash_crc.sh write' if you trust it"
        return 3
    fi
    got=$((16#$(ffcrc_crc32 "$file")))
    if [ "$got" -ne "$crc" ]; then
        printf 'CRC trailer MISMATCH: image %08x, trailer %08x (corrupted, or modified after the trailer was written)\n' "$got" "$crc"
        return 2
    fi
    printf 'CRC trailer OK (%08x)\n' "$crc"
    return 0
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    case "${1:-}" in
        write) ffcrc_write "$2" ;;
        check) ffcrc_check "$2" ;;
        crc)   ffcrc_crc32 "$2" ;;
        *) echo "Usage: $0 write|check|crc FILE" >&2; exit 64 ;;
    esac
fi
