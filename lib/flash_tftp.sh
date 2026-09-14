# Shared TFTP-upload helpers for the RTL8196E flash tooling.
#
# The RTL8196E V2.x bootloader auto-flashes an image to NOR the moment it has
# received it in full (then notifies on UDP:9999 and reboots). tftp-hpa can give
# up on a single stalled block mid-transfer (discussion #135) even when the
# bootloader's TFTP server is healthy. Retrying is the right response — but a
# blind retry is unsafe: a timeout can also mean the *final* ACK was lost, i.e.
# the image already landed and the bootloader is mid-write, where re-sending
# would collide with the flash write. So we retry only after re-probing the
# bootloader, and never re-send when its state is ambiguous.
#
# Sourced by flash_{kernel,rootfs,userdata,bootloader}.sh. Requires: tftp-hpa,
# timeout, mktemp.

# probe_tftp_wrq <ip>
#   0  the bootloader's TFTP server ACKed a 1-byte WRQ (idle at its prompt)
#   1  it did not answer within 3s (busy writing flash, or gone)
# A busy, single-threaded bootloader writing NOR cannot service the WRQ, so a
# non-answer is the signal "do not re-send". (Use PUT, not GET: the bootloader
# silently drops RRQ but ACKs a WRQ; the 1-byte payload fails its image
# signature check and is discarded.)
probe_tftp_wrq() {
    local ip="$1" probe_file rc=0
    # This tftp client resolves PUT sources relative to its current directory;
    # a /tmp path is treated as a remote name and never tests the target.
    probe_file=$(mktemp ./tftp-probe.XXXXXX)
    printf 'X' > "$probe_file"
    timeout 3 tftp -m binary "$ip" -c put "${probe_file#./}" >/dev/null 2>&1 || rc=$?
    rm -f "$probe_file"
    [ "$rc" -ne 124 ]
}

# tftp_put_safe <ip> <file> [tries] [per_try_timeout_s]
#   Uploads <file> to the bootloader, retrying a mid-transfer stall SAFELY.
#   Progress is written to stderr; exactly one status word is written to stdout:
#     OK        upload completed
#     AUTOFLASH a retry was unsafe — the bootloader went quiet after a timeout,
#               so the image likely landed and it is auto-flashing (a lost final
#               ACK looks like a timeout). The caller must NOT re-send; proceed
#               to its flash-write confirmation (the bootloader still notifies).
#     FAILED    every attempt stalled with the bootloader still idle (nothing
#               written) — the caller should abort.
#   Returns 0 for OK/AUTOFLASH, 1 for FAILED. Callers that only need go/no-go
#   can ignore stdout and test the return code:
#       if ! tftp_put_safe "$ip" img 3 30 >/dev/null; then ...abort...; fi
tftp_put_safe() {
    local ip="$1" file="$2" tries="${3:-3}" tmo="${4:-30}" attempt out
    for attempt in $(seq 1 "$tries"); do
        [ "$attempt" -gt 1 ] && echo "Retrying upload (attempt ${attempt}/${tries})..." >&2
        echo "Uploading..." >&2
        out=$(timeout "$tmo" tftp -m binary "$ip" -c put "$file" 2>&1) || true
        if ! echo "$out" | grep -qiE \
            "error|timeout|timed out|refused|failed|unknown host|access denied|disk full|illegal|not connected|unknown transfer"; then
            echo OK
            return 0
        fi
        echo "Upload attempt ${attempt} failed: ${out}" >&2
        # A timeout is either a mid-transfer stall (nothing written) or a lost
        # final ACK (image landed, bootloader now writing flash). Re-probe after
        # every failure — incl. the last — so a lost final ACK is never reported
        # as "nothing written".
        sleep 2
        if ! probe_tftp_wrq "$ip"; then
            echo "Bootloader went quiet after the upload — it may already be auto-flashing" >&2
            echo "the image (a lost final ACK looks like a timeout). Not re-sending." >&2
            echo AUTOFLASH
            return 0
        fi
        echo "Bootloader still idle (TFTP responding) — nothing was written." >&2
    done
    echo FAILED
    return 1
}
