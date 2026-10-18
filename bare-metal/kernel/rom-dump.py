#!/usr/bin/env python3
# The boot ROM, as the kernel saved it (R1 page, Y): check the header, write
# the bare ROM next to the dump, and print what's in it.
#   rom-dump.py dump.bin rom.bin
import struct, sys, re

d = open(sys.argv[1], "rb").read()
magic, base, size, direct, via_el3, unreadable, refused = struct.unpack_from("<QIIIIII", d)
if magic != 0x31504d554d4f52:
    sys.exit("no ROM dump there")
rom = d[512:512 + size]
open(sys.argv[2], "wb").write(rom)

print(f"Boot ROM at {base:#x}, {size // 1024} KB window")
print(f"  words read directly: {direct}, via EL3: {via_el3}, unreadable: {unreadable}")
if refused != 0xffffffff:
    print(f"  first refused at +{refused:#x}")
end = 0
for b in range(0, size, 1024):
    blk = rom[b:b + 1024]
    if blk.count(blk[:4]) * 4 != len(blk):
        end = b + 1024
print(f"  content up to +{end:#x} ({end // 1024} KB)")
print("\nText in it (6+ printable characters):")
for m in re.finditer(rb"[\x20-\x7e]{6,}", rom):
    print(f"  +{m.start():05x}  {m.group().decode()}")
