# Print a RAM register dump that the kernel saved to the card (RAM page, Y).
#   python3 ram-dump.py dumps/ram-....bin
import struct, sys

data = open(sys.argv[1], "rb").read()
magic, count, faults = struct.unpack_from("<QII", data, 0)
assert magic == 0x31504d554d4152, "not a RAM dump (no RAMDUMP1 magic)"
print("%d regions, %d reads refused" % (count, faults))
for i in range(count):
    name, base, size, offset, _ = struct.unpack_from("<8sIIII", data, 16 + 24 * i)
    name = name.rstrip(b"\0").decode()
    print("\n%s at %08x (%#x bytes); zero words left out" % (name, base, size))
    words = struct.unpack_from("<%dI" % (size // 4), data, offset)
    for j, w in enumerate(words):
        if w:
            print("  %-8s +%03x = %08x" % (name, j * 4, w))
