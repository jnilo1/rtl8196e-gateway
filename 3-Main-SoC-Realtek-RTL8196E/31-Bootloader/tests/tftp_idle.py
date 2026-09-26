#!/usr/bin/env python3
"""tftp_idle.py - check how the bootloader handles an abandoned TFTP transfer.

A TFTP transfer is bound to the client port that opened it.  When that client
goes silent (a stalled or interrupted tftp), the loader must drop the transfer
once it has been idle for TFTP_IDLE_JIFFIES (15 s) and serve the next request,
from any port, as if it were idle; a transfer that is slow but alive must never
be taken over.  This speaks raw TFTP over an ordinary UDP socket (no root).
After a stall the new client stays silent until the idle window has passed,
then sends one request per second: the request that makes the loader drop the
stale transfer must itself be served, so the stall tests pass only when the
first request is answered (a single lost packet on a Wi-Fi link can fail
them: run them again).

Run it against a loader in download mode with `AUTOBURN 0` typed on the console
first: every upload then stays in RAM and nothing is written to flash.  The
console shows `TFTP: idle transfer dropped` each time the loader drops one.

    ./tests/tftp_idle.py 192.168.1.6 upload-stall [BLOCKS]
        open an upload, send BLOCKS full blocks (default 50; 0 = die right
        after the WRQ), go silent; then WRQ from a new port every second.
        Expected: the first WRQ, sent 16 s after the last DATA, is answered;
        1-byte upload done.
    ./tests/tftp_idle.py 192.168.1.6 live-slow [INTERVAL] [DURATION]
        keep an upload alive with one block every INTERVAL s (default 2) for
        DURATION s (default 30) while an intruder sends a WRQ every 0.5 s.
        Expected: intruder never answered, upload completed.
    ./tests/tftp_idle.py 192.168.1.6 download-stall
        read block 1 of the RAM buffer (RRQ), never ACK it; then RRQ from a new
        port every second and download it all.  Needs data loaded (the last
        upload, or FLR).  Expected: the first RRQ, 16 s later, is answered and
        the download completes.
    ./tests/tftp_idle.py 192.168.1.6 peer-retx
        a client retransmitting its own WRQ before block 1 is still served.
"""
import socket
import struct
import sys
import time

ACK, DATA, ERROR = 4, 3, 5


def sock():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("", 0))
    return s


def request(opcode, name):
    return struct.pack("!H", opcode) + name + b"\0octet\0"


def recv(s, timeout):
    s.settimeout(max(0.01, timeout))
    try:
        return s.recvfrom(1024)
    except socket.timeout:
        return None, None


def wait_for(s, opcode, number, timeout):
    """Wait for packet (opcode, number), skipping late duplicates."""
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        pkt, addr = recv(s, end - time.monotonic())
        if pkt and len(pkt) >= 4:
            op, num = struct.unpack("!HH", pkt[:4])
            if op == opcode and num == number:
                return pkt, addr
            if op == ERROR:
                return None, None
    return None, None


def open_upload(s, host, name, tries=10):
    for _ in range(tries):
        s.sendto(request(2, name), (host, 69))
        _, peer = wait_for(s, ACK, 0, 1.0)
        if peer:
            return peer
    return None


def send_block(s, peer, number, payload, tries=5):
    for _ in range(tries):
        s.sendto(struct.pack("!HH", DATA, number) + payload, peer)
        if wait_for(s, ACK, number, 1.0)[0]:
            return True
    return False


IDLE_WAIT = 16.0  # the loader's idle window (15 s) plus a margin


def wait_new_client(host, opcode, name, first, since, limit=60):
    """After IDLE_WAIT of silence, a request from a new port every second.

    Returns (delay, attempts, sock, pkt, peer); delay is None if never answered.
    """
    s = sock()
    time.sleep(max(0.0, since + IDLE_WAIT - time.monotonic()))
    attempts = 0
    while time.monotonic() - since < limit:
        attempts += 1
        s.sendto(request(opcode, name), (host, 69))
        pkt, peer = wait_for(s, first[0], first[1], 1.0)
        if pkt:
            return time.monotonic() - since, attempts, s, pkt, peer
    return None, attempts, s, None, None


def upload_stall(host, blocks):
    a = sock()
    peer = open_upload(a, host, b"stalled.bin")
    if not peer:
        return "FAIL: no ACK 0 to the first WRQ"
    last = time.monotonic()
    for n in range(1, blocks + 1):
        if not send_block(a, peer, n, b"\xa5" * 512):
            return "FAIL: block %d never ACKed" % n
        last = time.monotonic()
    delay, attempts, b, _, peer_b = wait_new_client(host, 2, b"after-idle.bin", (ACK, 0), last)
    if delay is None:
        return "FAIL: the new client was never answered"
    done = send_block(b, peer_b, 1, b"X")
    ok = done and attempts == 1
    return "%s: new client answered %.1f s after the stall on WRQ #%d (%d blocks sent), 1-byte upload %s" % (
        "PASS" if ok else "FAIL", delay, attempts, blocks, "done" if done else "NOT done")


def live_slow(host, interval, duration):
    a, intruder = sock(), sock()
    peer = open_upload(a, host, b"slow.bin")
    if not peer:
        return "FAIL: no ACK 0"
    t0 = time.monotonic()
    n, next_data, answered = 1, t0, []
    while time.monotonic() - t0 < duration:
        if time.monotonic() >= next_data:
            if not send_block(a, peer, n, b"\x5a" * 512):
                return "FAIL: block %d never ACKed" % n
            n += 1
            next_data = time.monotonic() + interval
        intruder.sendto(request(2, b"intruder.bin"), (host, 69))
        if recv(intruder, 0.5)[0]:
            answered.append(round(time.monotonic() - t0, 1))
    done = send_block(a, peer, n, b"")
    ok = done and not answered
    return "%s: %d blocks, one every %.0f s for %.0f s; intruder answered at %s; upload %s" % (
        "PASS" if ok else "FAIL", n - 1, interval, duration,
        answered or "never", "completed" if done else "NOT completed")


def download_stall(host):
    a = sock()
    for _ in range(10):
        a.sendto(request(1, b"dump.bin"), (host, 69))
        if wait_for(a, DATA, 1, 1.0)[0]:
            break
    else:
        return "FAIL: no DATA 1 (nothing loaded, or the server is busy)"
    last = time.monotonic()
    delay, attempts, b, pkt, peer = wait_new_client(host, 1, b"dump.bin", (DATA, 1), last)
    if delay is None:
        return "FAIL: the new client was never answered"
    data, n = pkt[4:], 1
    while len(pkt) - 4 == 512:
        b.sendto(struct.pack("!HH", ACK, n), peer)
        pkt, _ = wait_for(b, DATA, n + 1, 3.0)
        if not pkt:
            return "FAIL: download stalled at block %d" % n
        data += pkt[4:]
        n += 1
    b.sendto(struct.pack("!HH", ACK, n), peer)
    return "%s: new client answered %.1f s after the stall on RRQ #%d, downloaded %d bytes (compare with the console)" % (
        "PASS" if attempts == 1 else "FAIL", delay, attempts, len(data))


def peer_retx(host):
    s = sock()
    acks, peer = 0, None
    for _ in range(2):
        s.sendto(request(2, b"retx.bin"), (host, 69))
        _, addr = wait_for(s, ACK, 0, 1.0)
        if addr:
            acks, peer = acks + 1, addr
    done = bool(peer) and send_block(s, peer, 1, b"Y")
    return "%s: ACK 0 for %d/2 WRQs from the same port, upload %s" % (
        "PASS" if acks == 2 and done else "FAIL", acks, "done" if done else "NOT done")


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    host, test, args = sys.argv[1], sys.argv[2], sys.argv[3:]
    if test == "upload-stall":
        print(upload_stall(host, int(args[0]) if args else 50))
    elif test == "live-slow":
        print(live_slow(host, float(args[0]) if args else 2, float(args[1]) if len(args) > 1 else 30))
    elif test == "download-stall":
        print(download_stall(host))
    elif test == "peer-retx":
        print(peer_retx(host))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
