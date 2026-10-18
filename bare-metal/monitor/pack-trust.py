#!/usr/bin/env python3
# Pack programs into Rockchip's "trust" image, the way their trust_merger
# tool packs ARM Trusted Firmware and OP-TEE, so their loader (on the card
# at sector 64) loads it and jumps in at EL3.
#
#   pack-trust.py monitor.bin trust.img     our monitor as BL31 at 0x10000
#
# The format (tools/rockchip/trust_merger.c in Rockchip's U-Boot):
#   2 KB header:
#     0x000  "BL3X", version (BCD, 0x100 = 1.00), flags (0x33: SHA-256 for
#            the hashes, and which RSA mode signing would use), size =
#            number of parts << 16 | (offset of the signature) / 4
#     0x020  768 bytes for an RSA key: zeros, the image isn't signed
#     0x320  per part: SHA-256 of it, load address, 0, 0, 0
#     then   256 bytes of signature (zeros), then per part: its name
#            ("BL31"), where it is (in 512-byte sectors from the start of
#            the image), how big (sectors), 0
#   then the parts, each padded to 2 KB.
# The whole thing padded to 2 MB and written twice.
import hashlib, struct, sys

def pack(parts, copies=2, slot=2 << 20):
    """parts: list of (name, load address, bytes)"""
    n = len(parts)
    sign_offset = 0x320 + n * 48
    head = bytearray(2048)
    struct.pack_into("<4sIII", head, 0, b"BL3X", 0x100, 0x33, n << 16 | sign_offset >> 2)
    body = b""
    for i, (name, addr, data) in enumerate(parts):
        data += b"\0" * (-len(data) % 2048)
        struct.pack_into("<32sIIII", head, 0x320 + i * 48, hashlib.sha256(data).digest(), addr, 0, 0, 0)
        struct.pack_into("<4sIII", head, sign_offset + 256 + i * 16, name,
                         (2048 + len(body)) // 512, len(data) // 512, 0)
        body += data
    one = bytes(head) + body
    assert len(one) <= slot, "too big"
    one += b"\0" * (slot - len(one))
    return one * copies

if __name__ == "__main__":
    data = open(sys.argv[1], "rb").read()
    out = pack([(b"BL31", 0x10000, data)])
    open(sys.argv[2], "wb").write(out)
    print("%s: %d bytes, BL31 at 0x10000, %d bytes of program" % (sys.argv[2], len(out), len(data)))
