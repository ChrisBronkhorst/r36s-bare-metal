#!/usr/bin/env python3
# Wrap a program in Rockchip's "second-level loader" format, the way their
# loaderimage tool packs u-boot.bin into uboot.img, so Rockchip's first-stage
# loader (on the card at sector 64) will load and run it in U-Boot's place.
#
#   pack-loader.py kernel.img uboot.img [load address, default 0x200000]
#
# The format (tools/rockchip/loaderimage.c in Rockchip's U-Boot):
#   a 2 KB header: "LOADER  ", version, load address, size, CRC-32 and
#   SHA-256 of the program, then the program; the whole thing padded to
#   1 MB and written 4 times, so a bad sector in one copy doesn't stop boot.
import hashlib, struct, sys

def crc32_rk(data):
    # Rockchip's CRC-32: polynomial 0x04c10db7, most significant bit first,
    # starting from 0 (not the zlib one)
    table = []
    for i in range(256):
        c = i << 24
        for _ in range(8):
            c = ((c << 1) ^ 0x04c10db7) if c & 0x80000000 else c << 1
        table.append(c & 0xffffffff)
    crc = 0
    for b in data:
        crc = ((crc << 8) & 0xffffffff) ^ table[((crc >> 24) ^ b) & 0xff]
    return crc

def pack(program, load_addr=0x200000, copies=4, slot=1 << 20):
    program += b"\0" * (-len(program) % 4)   # a multiple of 4 bytes
    size = len(program)
    assert 2048 + size <= slot, "too big for one 1 MB copy"
    hash_len = 32
    sha = hashlib.sha256(program + struct.pack("<III", load_addr, size, hash_len)).digest()
    header = struct.pack("<8sIIIIII32s", b"LOADER  ", 0, 0, load_addr, size,
                         crc32_rk(program), hash_len, sha)
    header += b"\0" * (2048 - len(header))
    one = header + program
    one += b"\0" * (slot - len(one))
    return one * copies

if __name__ == "__main__":
    data = open(sys.argv[1], "rb").read()
    addr = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x200000
    out = pack(data, addr)
    open(sys.argv[2], "wb").write(out)
    print("%s: %d bytes, load address %#x, %d bytes of program" % (sys.argv[2], len(out), addr, len(data)))
