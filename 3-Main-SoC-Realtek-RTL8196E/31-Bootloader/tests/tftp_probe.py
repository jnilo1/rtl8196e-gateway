#!/usr/bin/env python3
"""tftp_probe.py - send malformed TFTP/ICMP frames to a bootloader in download mode.

Exercises the length checks of boot/net/tftpd.c (audit items S1 and S9).
Every probe must leave the board answering `ping`; probes 4 and 5 run inside
a real upload and also check what the loader does with it.  Needs raw
sockets, hence root:

    sudo ./tests/tftp_probe.py 192.168.1.6

Run `AUTOBURN 0` on the console first: the uploads then stay in RAM.  With
AUTOBURN 1 the auto-flash preflight refuses them and answers FAIL on
UDP:9999; nothing is written either way.

Probes (each one is a legal frame with a lie inside):
  1. UDP length field 8 on a DATA frame (below the TFTP header)   -> dropped
  2. UDP length field 65535 on a 60-byte frame                    -> dropped
  3. IP total length 1500 on a 60-byte ICMP echo                  -> no reply
     (sent as a raw Ethernet frame: a raw IP socket gets its IP length
     rewritten by the kernel; skipped if that frame cannot be sent)
  4. In an open upload, DATA block 1 with UDP length 12+513
     (one byte over the block)                   -> TFTP ERROR 4, upload aborted
  5. In an open upload, DATA block 1 with UDP length 11
     (no room for the TFTP header)               -> dropped, the upload goes on
Then a normal 4-block upload proves the server still works.  Every transfer
opened here is closed, so the server is left idle.
"""
import socket
import struct
import subprocess
import sys
import time

ACK, DATA, ERROR = 4, 3, 5


def ip_checksum(data):
    if len(data) % 2:
        data += b"\0"
    s = sum(struct.unpack("!%dH" % (len(data) // 2), data))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF


def ip_header(src, dst, proto, total_len):
    hdr = struct.pack("!BBHHHBBH4s4s", 0x45, 0, total_len, 0, 0, 64, proto, 0,
                      socket.inet_aton(src), socket.inet_aton(dst))
    return hdr[:10] + struct.pack("!H", ip_checksum(hdr)) + hdr[12:]


def udp(src, dst, sport, dport, data, udp_len=None):
    if udp_len is None:
        udp_len = 8 + len(data)
    return ip_header(src, dst, 17, 20 + 8 + len(data)) + struct.pack(
        "!HHHH", sport, dport, udp_len, 0) + data


def icmp_echo(src, dst, data, ip_len):
    body = struct.pack("!BBHHH", 8, 0, 0, 1, 1) + data
    body = body[:2] + struct.pack("!H", ip_checksum(body)) + body[4:]
    return ip_header(src, dst, 1, ip_len) + body


def alive(dst):
    return subprocess.run(["ping", "-c", "1", "-W", "2", dst],
                          stdout=subprocess.DEVNULL).returncode == 0


def route(dst):
    words = subprocess.run(["ip", "-o", "route", "get", dst], capture_output=True,
                           text=True).stdout.split()
    return words[words.index("src") + 1], words[words.index("dev") + 1]


def neighbour_mac(dst, dev):
    words = subprocess.run(["ip", "neigh", "show", dst, "dev", dev],
                           capture_output=True, text=True).stdout.split()
    return words[words.index("lladdr") + 1] if "lladdr" in words else None


def mac_bytes(mac):
    return bytes(int(b, 16) for b in mac.split(":"))


def wait_for(u, opcode, number, timeout):
    """Wait for a TFTP packet (opcode, number), skipping late duplicates."""
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        u.settimeout(max(0.01, end - time.monotonic()))
        try:
            pkt, addr = u.recvfrom(1024)
        except socket.timeout:
            break
        if len(pkt) >= 4:
            op, num = struct.unpack("!HH", pkt[:4])
            if op == opcode and (number is None or num == number):
                return pkt, addr
    return None, None


def open_upload(u, dst, name):
    for _ in range(5):
        u.sendto(struct.pack("!H", 2) + name + b"\0octet\0", (dst, 69))
        _, server = wait_for(u, ACK, 0, 1.0)
        if server:
            return server
    return None


def send_block(u, server, number, payload):
    for _ in range(5):
        u.sendto(struct.pack("!HH", DATA, number) + payload, server)
        if wait_for(u, ACK, number, 1.0)[0]:
            return True
    return False


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    dst = sys.argv[1]
    src, dev = route(dst)
    raw = socket.socket(socket.AF_INET, socket.SOCK_RAW, socket.IPPROTO_RAW)
    raw.setsockopt(socket.IPPROTO_IP, socket.IP_HDRINCL, 1)
    # Replies to the raw frames (TFTP ACK/ERROR) come back to this port.
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.bind(("", 0))
    sport = u.getsockname()[1]
    data1 = struct.pack("!HH", DATA, 1) + b"A" * 512
    ok = True

    def report(name, outcome, good):
        nonlocal ok
        time.sleep(0.5)
        up = alive(dst)
        ok = ok and up and good
        print("%-40s %s, %s" % (name, "board alive" if up else "NO PING - FAILED", outcome))

    raw.sendto(udp(src, dst, sport, 69, data1, udp_len=8), (dst, 0))
    report("UDP len 8 on a DATA frame", "dropped", True)

    raw.sendto(udp(src, dst, sport, 69, data1[:20], udp_len=65535), (dst, 0))
    report("UDP len 65535 on a short frame", "dropped", True)

    name = "IP len 1500 on a 60-byte ping"
    alive(dst)                        # make sure the neighbour entry is fresh
    mac = neighbour_mac(dst, dev)
    try:
        eth = socket.socket(socket.AF_PACKET, socket.SOCK_RAW)
        eth.bind((dev, 0))
        own = open("/sys/class/net/%s/address" % dev).read().strip()
        frame = (mac_bytes(mac) + mac_bytes(own) + b"\x08\x00" +
                 icmp_echo(src, dst, b"B" * 18, ip_len=1500))
        eth.send(frame)
        report(name, "sent as an Ethernet frame", True)
    except (OSError, TypeError, AttributeError) as e:
        print("%-40s SKIPPED (%s)" % (name, e))

    name = "upload: DATA with UDP len 12+513"
    server = open_upload(u, dst, b"probe4")
    if not server:
        report(name, "NO ACK 0 to the WRQ - FAILED", False)
    else:
        raw.sendto(udp(src, dst, sport, server[1], data1 + b"C"), (dst, 0))
        err, _ = wait_for(u, ERROR, None, 1.5)
        code = struct.unpack("!H", err[2:4])[0] if err else None
        report(name, "TFTP ERROR %s" % code if err else "NO ERROR - FAILED", code == 4)

    name = "upload: DATA with UDP len 11"
    server = open_upload(u, dst, b"probe5")
    if not server:
        report(name, "NO ACK 0 to the WRQ - FAILED", False)
    else:
        raw.sendto(udp(src, dst, sport, server[1], data1, udp_len=11), (dst, 0))
        answered = wait_for(u, ERROR, None, 1.0)[0] or wait_for(u, ACK, 1, 0.1)[0]
        goes_on = send_block(u, server, 1, b"end")    # a short block closes it
        report(name, "%s, upload %s" % ("answered - FAILED" if answered else "dropped",
                                        "completed" if goes_on else "NOT completed - FAILED"),
               not answered and goes_on)

    name = "normal 4-block upload"
    server = open_upload(u, dst, b"probe-ok")
    done = bool(server) and all(send_block(u, server, n, b"D" * 512) for n in (1, 2, 3)) \
        and send_block(u, server, 4, b"last")
    report(name, "completed" if done else "NOT completed - FAILED", done)

    print("probe result:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
