#!/usr/bin/env python3
"""Send a kernel to the running console over the USB network and start it.

    python3 push.py kernel.img          (or: make push)
    python3 push.py --try kernel.img    (or: make try) RAM only, not saved
    python3 push.py --reboot            (or: make reboot) restart the console

By default the console also saves it to the card, so it's the kernel the
next power-on starts: we send the 2 KB LOADER header that goes in front of
it there (pack-loader.py makes it), and the console writes both and reads
them back before switching.

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

import importlib.util

CONSOLE = ("192.168.36.1", 5001)
MAGIC = 0x4853504B  # "KPSH"
START, DATA, GO, HEADER, REBOOT = 1, 2, 3, 4, 5
PIECE = 1400
STATUS = {0: "ok", 1: "too big", 2: "bad piece", 3: "bad CRC",
          4: "not an ARM64 kernel image", 5: "no start (did the console reboot?)",
          6: "LOADER header doesn't match the kernel", 7: "card failed (see the log)",
          8: "the card has no kernel at sector 16384, so not writing there"}


def loader_header(image):
    # pack-loader.py's own code, so both ways of installing agree
    here = __file__.rsplit("/", 1)[0] if "/" in __file__ else "."
    spec = importlib.util.spec_from_file_location("pack_loader", here + "/pack-loader.py")
    pack_loader = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(pack_loader)
    return pack_loader.pack(image, copies=1)[:2048]


def ask(sock, kind, a, b, payload=b"", tries=20, wait=0.2):
    packet = struct.pack("<IIII", MAGIC, kind, a, b) + payload
    for _ in range(tries):
        sock.sendto(packet, CONSOLE)
        deadline = time.time() + wait
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


def show_log(log, seconds=8):
    """The console's log until it's back on the network"""
    end = time.time() + seconds
    while time.time() < end:
        try:
            data, _ = log.recvfrom(2000)
        except socket.timeout:
            continue
        text = data.decode(errors="replace")
        print(text, end="" if text.endswith("\n") else "\n")
        if "link up" in text or "DHCP gave" in text:
            end = min(end, time.time() + 1)


def listen_log():
    # SO_REUSEPORT lets other listeners on port 5000 keep theirs
    log = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    log.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    log.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    log.bind(("", 5000))
    log.settimeout(0.5)
    return log


def main():
    args = sys.argv[1:]
    if "--reboot" in args:
        log = listen_log()
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(0.2)
        ask(sock, REBOOT, 0, 0)
        print("push: the console is restarting")
        show_log(log, 15)   # a cold boot takes a few seconds
        return
    keep = "--try" not in args
    args = [a for a in args if a != "--try"]
    path = args[0] if args else "kernel.img"
    image = open(path, "rb").read()
    image += b"\0" * (-len(image) % 4)   # as on the card
    crc = zlib.crc32(image)

    # The log listener first, so we don't miss the new kernel's first lines
    log = listen_log()

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
    if keep:
        header = loader_header(image)
        for offset in (0, 1024):
            status = ask(sock, HEADER, offset, 1024, header[offset:offset + 1024])
            if status:
                sys.exit(f"push: header refused: {STATUS.get(status, status)}")
    took = time.time() - t
    # Writing the card takes a moment: wait longer for this answer
    status = ask(sock, GO, 0, 0, tries=10, wait=2)
    if status:
        sys.exit(f"push: refused to start it: {STATUS.get(status, status)}")
    print(f"push: {len(image)} bytes in {took * 1000:.0f} ms, CRC {crc:08x}, "
          + ("saved to the card" if keep else "not saved (RAM only)")
          + ": the new kernel is starting")

    show_log(log)


if __name__ == "__main__":
    main()
