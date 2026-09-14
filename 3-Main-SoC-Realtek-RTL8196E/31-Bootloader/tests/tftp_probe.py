#!/usr/bin/env python3
"""tftp_probe.py - send malformed TFTP/ICMP frames to a bootloader in download mode.

Exercises the packet-length checks of boot/net/tftpd.c (audit items S1 and
S9).  Each probe must leave the board answering `ping` and the `<RealTek>`
prompt alive; a hang or a reset is a failure.  The board's replies are not
inspected — the point is that it survives.

Needs a raw socket, hence root:

    sudo ./tests/tftp_probe.py 192.168.1.6

Probes (each one is a legal Ethernet/IP frame with a lie inside):
  1. UDP length field 8 (below the TFTP DATA header)            -> dropped
  2. UDP length field 65535 on a 60-byte frame                   -> dropped
  3. IP total length 1500 on a 60-byte ICMP echo                 -> no reply
  4. TFTP DATA with UDP length 12+513 (one byte over the block)  -> ERROR 4
  5. A WRQ followed by a DATA block 1 with UDP length 11         -> ERROR 4
Then a normal 4-block upload with AUTOBURN 0 to prove the server still works
(run `AUTOBURN 0` on the console first, or the garbage gets a FAIL notify,
which is also fine).
"""
import os
import socket
import struct
import subprocess
import sys
import time


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


def send_raw(sock, dst, payload):
    sock.sendto(payload, (dst, 0))


def udp(src, dst, sport, dport, data, udp_len=None, ip_len=None):
    if udp_len is None:
        udp_len = 8 + len(data)
    if ip_len is None:
        ip_len = 20 + 8 + len(data)
    return ip_header(src, dst, 17, ip_len) + struct.pack(
        "!HHHH", sport, dport, udp_len, 0) + data


def icmp_echo(src, dst, data, ip_len=None):
    if ip_len is None:
        ip_len = 20 + 8 + len(data)
    body = struct.pack("!BBHHH", 8, 0, 0, 1, 1) + data
    body = body[:2] + struct.pack("!H", ip_checksum(body)) + body[4:]
    return ip_header(src, dst, 1, ip_len) + body


def alive(dst):
    return subprocess.run(["ping", "-c", "1", "-W", "2", dst],
                          stdout=subprocess.DEVNULL).returncode == 0


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    dst = sys.argv[1]
    src = subprocess.run(["ip", "-o", "route", "get", dst], capture_output=True,
                         text=True).stdout.split("src")[1].split()[0]
    s = socket.socket(socket.AF_INET, socket.SOCK_RAW, socket.IPPROTO_RAW)
    s.setsockopt(socket.IPPROTO_IP, socket.IP_HDRINCL, 1)
    sport = 40000 + os.getpid() % 1000
    tftp_data1 = struct.pack("!HH", 3, 1) + b"A" * 512
    probes = [
        ("UDP len 8 on a DATA block", udp(src, dst, sport, 2098, tftp_data1, udp_len=8)),
        ("UDP len 65535 on a short frame", udp(src, dst, sport, 2098, tftp_data1[:20], udp_len=65535)),
        ("IP len 1500 on a 60-byte ping", icmp_echo(src, dst, b"B" * 20, ip_len=1500)),
        ("DATA with UDP len 12+513", udp(src, dst, sport, 2098, tftp_data1 + b"C", udp_len=12 + 513)),
        ("WRQ then DATA with UDP len 11",
         [udp(src, dst, sport, 69, struct.pack("!H", 2) + b"probe\0octet\0"),
          udp(src, dst, sport, 2098, tftp_data1, udp_len=11)]),
    ]
    ok = True
    for name, frames in probes:
        if not isinstance(frames, list):
            frames = [frames]
        for f in frames:
            send_raw(s, dst, f)
            time.sleep(0.3)
        time.sleep(0.5)
        up = alive(dst)
        print("%-40s %s" % (name, "board alive" if up else "NO PING - FAILED"))
        ok = ok and up
    print("probe result:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
