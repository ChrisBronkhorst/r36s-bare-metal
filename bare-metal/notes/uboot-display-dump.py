# The display registers as U-Boot left them (2026-10-19), copied from photos
# of the kernel's display report (X on the info page). Each page's CRC-32
# matches the one on screen, so the copy is exact. What our own display
# setup has to reproduce. Still missing: PHY registers 0xe0-0xff (LVDS part).
# Run it to re-check: python3 uboot-display-dump.py
import zlib, struct
def crc(pairs): return zlib.crc32(b''.join(struct.pack('<II',a,v) for a,v in pairs))
def parse(s): 
    t=s.split(); return [(int(t[i],16),int(t[i+1],16)) for i in range(0,len(t),2)]
vop=parse("""00c 00711c08 018 00006000 020 01010001 028 09000040 02c 00000012 030 000001ed
03c 301d0000 054 00021220 05c 01400140 068 00ef013f 06c 00ef013f 070 000a000a
074 10001000 078 10001000 090 00000011 094 00000580 098 000001e0 0a0 3de00000
0a4 01df027f 0a8 0010002e 0e0 00000060 0ec 000a000a 100 02dc0002 104 002e02ae
108 02000002 10c 001001f0 160 00000012 170 12844821 174 21488412 178 a55a9696
17c 5aa56969 180 deb77bed 184 ed7bb7de 194 00000800 1f0 00000179 1f4 01000000""")
dsi=parse("""000 3133302a 004 1 008 a02 00c 0 010 5 014 6 018 00040004 01c 0 020 0
024 0 028 0 02c 1c 030 0 034 0 038 bf02 03c 280 040 0 044 0 048 2 04c 24 050 25e
054 2 058 e 05c 10 060 1e0 064 0 068 00030000 06c 2905 070 0 074 15 078 03e803e8
07c 0 080 0 084 0 088 0 08c d00 090 0 094 1 098 00400040 09c 14102710 0a0 7 0a4 2003
0a8 0 0ac 0 0b0 1529 0b4 0 0b8 0 0bc 0 0c0 800 0c4 0 0c8 0 0cc 0 0d0 0 0d4 0 0d8 0 0dc 0""")
misc=parse("""ff2b0040 2091 ff2b0044 1443 ff2b0048 1 ff2b0060 1063 ff2b0064 1442 ff2b0068 1
ff2b00a0 55 ff2b0114 13 ff2b0118 0bb8ea60 ff2b01c4 0b0b ff2b0208 0 ff2b020c 0
ff2b0240 0 ff2bc000 1032 ff2bc004 1441 ff2bc008 1 ff2bc020 1 ff2bc048 3131
ff2bc084 0 ff140438 0 ff270000 00010000 ff270004 00010000""")
lane="0=4b 1=05 3=40 4=80 5=c0 6=18 7=04 8=0b 9=23 a=09 b=0f c=0b d=ff e=01 f=10 10=03 11=02 12=04 16=01"
phy_s="""00=7d 01=e0 02=a0 03=01 04=10 08=4c 09=dd 0a=84 0b=5c 0c=f0 0d=80 10=03 12=20 15=03 16=ff 17=60 18=0b 20=1f 22=61"""
phy=[(int(a,16),int(v,16)) for a,v in (x.split('=') for x in phy_s.split())]
for base in (0x40,0x60,0x80,0xa0,0xc0):
    for x in lane.split():
        a,v=x.split('='); a=base+int(a,16); v=int(v,16)
        if base==0x40 and a==0x47: v=0x17
        phy.append((a,v))
for name,p,want in (("VOP",vop,0xe81cbb3d),("DSI",dsi,0x4c3c2a3d),("misc",misc,0x3bc89e69),("PHY first 114",phy[:114],0x3c7e48d2)):
    print(name, len(p), "ok" if crc(p)==want else "MISMATCH %08x"%crc(p))
