#!/usr/bin/env python3
# Pack the image the boot ROM reads from the card's sector 64:
#
#   pack-idb.py ddr.bin spl.bin idbloader.img
#
# The format (tools/rkcommon.c in U-Boot, "header version 1"):
#   sector 0     a 512-byte header, scrambled with RC4 (a fixed key everyone
#                knows; it's not a secret, just tradition). In it:
#                  0x000 magic 0x0ff0aa55
#                  0x008 1 = the programs themselves are not scrambled
#                  0x00c where stage 1 starts, in sectors (4)
#                  0x1fa stage 1's size, in sectors
#                  0x1fc stages 1 and 2 together, in sectors
#   sectors 1-3  empty
#   sector 4     stage 1: Rockchip's RAM setup ("RK33" + code), run in SRAM
#   then         stage 2: us, loaded into RAM at 0 and run there
import struct, sys

KEY = bytes([124, 78, 3, 4, 85, 5, 9, 7, 45, 44, 123, 56, 23, 13, 23, 17])

def rc4(data):
    s = list(range(256)); j = 0
    for i in range(256):
        j = (j + s[i] + KEY[i % 16]) % 256
        s[i], s[j] = s[j], s[i]
    i = j = 0; out = bytearray()
    for b in data:
        i = (i + 1) % 256; j = (j + s[i]) % 256
        s[i], s[j] = s[j], s[i]
        out.append(b ^ s[(s[i] + s[j]) % 256])
    return bytes(out)

def pad(b):
    return b + b"\0" * (-len(b) % 512)

def pack(ddr, spl):
    assert ddr[:4] == b"RK33", "stage 1 should start with RK33"
    ddr, spl = pad(ddr), pad(spl)
    n1 = len(ddr) // 512
    n2 = n1 + len(spl) // 512
    head = bytearray(512)
    struct.pack_into("<IIIH", head, 0, 0x0ff0aa55, 0, 1, 4)
    struct.pack_into("<HH", head, 0x1fa, n1, n2)
    return rc4(bytes(head)) + b"\0" * 1536 + ddr + spl

if __name__ == "__main__":
    ddr = open(sys.argv[1], "rb").read()
    spl = open(sys.argv[2], "rb").read()
    out = pack(ddr, spl)
    open(sys.argv[3], "wb").write(out)
    print("%s: %d bytes: RAM setup %d, SPL %d" % (sys.argv[3], len(out), len(ddr), len(spl)))
