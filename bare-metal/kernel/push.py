#!/usr/bin/env python3
"""Send a kernel to the running console over the USB network and start it.

    python3 push.py kernel.img          (or: make push)

The console's loader (loader.c) listens on UDP port 5001 at 192.168.36.1.
Each packet is 16 bytes of header, little-endian: magic "KPSH", kind
(1 start, 2 data, 3 go), offset or size, CRC or length. It answers each
one with the same header and a status; we send the next piece only after
the answer ("stop and wait"), and send a piece again if no answer comes.

Afterwards, the new kernel's log (UDP broadcasts to port 5000) is shown
until it's back on the network.
"""
import socket
import struct
import sys
import time
import zlib

CONSOLE = ("192.168.36.1", 5001)
MAGIC = 0x4853504B  # "KPSH"
START, DATA, GO = 1, 2, 3
PIECE = 1400
STATUS = {0: "ok", 1: "too big", 2: "bad piece", 3: "bad CRC",
          4: "not an ARM64 kernel image", 5: "no start (did the console reboot?)"}


def ask(sock, kind, a, b, payload=b"", tries=20):
    packet = struct.pack("<IIII", MAGIC, kind, a, b) + payload
    for _ in range(tries):
        sock.sendto(packet, CONSOLE)
        deadline = time.time() + 0.2
        while time.time() < deadline:
            try:
                reply, _ = sock.recvfrom(64)
            except socket.timeout:
                break
            if len(reply) < 16:
                continue
            magic, rkind, ra, status = struct.unpack("<IIII", reply[:16])
            if magic == MAGIC and rkind == kind and ra == a:
                return status
    sys.exit(f"push: no answer from {CONSOLE[0]} (is the cable in and the console on?)")


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "kernel.img"
    image = open(path, "rb").read()
    crc = zlib.crc32(image)

    # The log listener first, so we don't miss the new kernel's first lines.
    # SO_REUSEPORT lets other listeners on port 5000 keep theirs.
    log = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    log.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    log.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    log.bind(("", 5000))
    log.settimeout(0.5)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(0.2)
    t = time.time()
    status = ask(sock, START, len(image), crc)
    if status:
        sys.exit(f"push: refused: {STATUS.get(status, status)}")
    for offset in range(0, len(image), PIECE):
        piece = image[offset:offset + PIECE]
        status = ask(sock, DATA, offset, len(piece), piece)
        if status:
            sys.exit(f"push: piece at {offset} refused: {STATUS.get(status, status)}")
    took = time.time() - t
    status = ask(sock, GO, 0, 0)
    if status:
        sys.exit(f"push: refused to start it: {STATUS.get(status, status)}")
    print(f"push: {len(image)} bytes in {took * 1000:.0f} ms, CRC {crc:08x}: "
          "the new kernel is starting")

    # Its log: from start-up until it's back on the network
    end = time.time() + 8
    while time.time() < end:
        try:
            data, _ = log.recvfrom(2000)
        except socket.timeout:
            continue
        text = data.decode(errors="replace")
        print(text, end="" if text.endswith("\n") else "\n")
        if "link up" in text or "DHCP gave" in text:
            end = min(end, time.time() + 1)


if __name__ == "__main__":
    main()
