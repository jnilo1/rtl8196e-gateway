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
# Sourced by flash_{kernel,rootfs,userdata,bootloader}.sh, flash_remote.sh,
# flash_install_rtl8196e.sh, restore_gateway.sh and create_fullflash.sh.
# Requires: tftp-hpa, timeout, mktemp.

# Every upload leaves from one fixed source port. The V3.1 bootloader ties an
# upload in progress to the client port that started it and ignores a WRQ from
# any other port until that transfer completes or its peer sends an ERROR, with
# no idle timeout: an upload interrupted by a stall, a Ctrl-C or a per-try
# limit would lock its TFTP server until a power cycle, since every tftp run
# picks a new random port. From the same port a WRQ restarts the transfer, once
# 20 s have passed since its last DATA block (kick_tftpd() in the bootloader).
# V3.2 drops a transfer silent for 15 s whatever the port; the fixed port still
# gets our own retry through at once when a lost ACK 0 leaves the transfer it
# opened waiting for block 1 (the loader takes that WRQ as a retransmit).
# Loaders older than V3.1 do not care which port is used.
TFTP_SRC_PORT="${TFTP_SRC_PORT:-50069}"
TFTP_PIN=(-R "${TFTP_SRC_PORT}:${TFTP_SRC_PORT}")

# probe_tftp_wrq <ip>
#   0  the bootloader's TFTP server ACKed a 1-byte WRQ (idle at its prompt)
#   1  it did not answer within 3s (busy writing flash, gone, or still inside
#      the 20 s window after an interrupted upload, see TFTP_SRC_PORT)
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
    timeout 3 tftp "${TFTP_PIN[@]}" -m binary "$ip" -c put "${probe_file#./}" >/dev/null 2>&1 || rc=$?
    rm -f "$probe_file"
    [ "$rc" -ne 124 ] || return 1
    # The bootloader answers the rejected 1-byte image with FAIL on UDP:9999.
    # Let that datagram go by, so a caller that starts its flash-result
    # listener right after this probe does not read it as the flash result.
    sleep 1
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
#   Every upload the bootloader completes, including the 1-byte probe run
#   between attempts, makes it send OK or FAIL on UDP:9999: start the
#   flash-result listener only after this function returns.
#   Returns 0 for OK/AUTOFLASH, 1 for FAILED. Callers that only need go/no-go
#   can ignore stdout and test the return code:
#       if ! tftp_put_safe "$ip" img 3 30 >/dev/null; then ...abort...; fi
tftp_put_safe() {
    local ip="$1" file="$2" tries="${3:-3}" tmo="${4:-30}" attempt out rc
    for attempt in $(seq 1 "$tries"); do
        [ "$attempt" -gt 1 ] && echo "Retrying upload (attempt ${attempt}/${tries})..." >&2
        echo "Uploading..." >&2
        # Success needs a zero exit status AND no error text. The status matters
        # on its own: tftp killed by `timeout` (a per-try limit shorter than
        # tftp's own 25 s give-up, or a slow transfer) prints nothing.
        rc=0
        out=$(timeout "$tmo" tftp "${TFTP_PIN[@]}" -m binary "$ip" -c put "$file" 2>&1) || rc=$?
        if [ "$rc" -eq 0 ] && ! echo "$out" | grep -qiE \
            "error|timeout|timed out|refused|failed|unknown host|access denied|disk full|illegal|not connected|unknown transfer"; then
            echo OK
            return 0
        fi
        [ "$rc" -eq 124 ] && out="killed after ${tmo}s${out:+: $out}"
        echo "Upload attempt ${attempt} failed: ${out:-tftp exit status $rc}" >&2
        # A timeout is either a mid-transfer stall (nothing written) or a lost
        # final ACK (image landed, bootloader now writing flash). Re-probe after
        # every failure — incl. the last — so a lost final ACK is never reported
        # as "nothing written". When `timeout` cut tftp off, the last DATA block
        # may be seconds old: wait out the bootloader's 20 s window first, or
        # the probe of an idle, restartable bootloader would go unanswered and
        # read as AUTOFLASH. tftp's own give-up already comes after 25 s of
        # silence.
        if [ "$rc" -eq 124 ]; then sleep 21; else sleep 2; fi
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
