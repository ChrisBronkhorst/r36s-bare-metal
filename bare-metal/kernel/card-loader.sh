#!/bin/bash
# Swap what Rockchip's loader starts, on the learning console's card:
#
#   ./card-loader.sh ddr      everything ours: RAM setup, SPL, monitor, kernel.
#                             Only the boot ROM inside the chip is left.
#   ./card-loader.sh spl      our SPL, monitor and kernel: only the boot ROM and
#                             Rockchip's RAM setup are left
#   ./card-loader.sh monitor  our kernel, and our monitor at EL3 (no U-Boot, no ATF)
#   ./card-loader.sh ours     our kernel instead of U-Boot, ARM Trusted Firmware at EL3
#   ./card-loader.sh uboot    the originals: U-Boot, ATF and OP-TEE
#   ./card-loader.sh ramdump  copy the RAM registers the kernel saved (RAM page,
#                             Y) to dumps/, and print them
#   ./card-loader.sh romdump  the same for the boot ROM (R1 page, Y), and
#                             disassemble it
#
# The card's boot area (before the first partition):
#   sector    64  Rockchip's RAM setup + loader      <- swapped: RAM setup + our SPL
#   sector 12288  the kernel's RAM register dump (32 KB), when saved
#   sector 12416  the kernel's boot ROM dump (64 KB + a header), when saved
#   sector 16384  "uboot.img": U-Boot, 4 copies      <- swapped: our kernel
#   sector 24576  "trust.img": ATF and OP-TEE, 2 copies <- swapped: our monitor
#   sector 32768  the BOOT partition
# The first time, the originals are saved in backup/.
# Spotlight sometimes holds the card for a moment after a write: retry
eject_card() {
  for try in 1 2 3 4 5; do
    diskutil eject "$1" >/dev/null 2>&1 && return 0
    sleep 2
  done
  echo "Couldn't eject $1 (something still has it open): try diskutil eject $1" >&2
  return 1
}

set -e
cd "$(dirname "$0")"
what=$1
case "$what" in ddr|spl|monitor|ours|uboot|ramdump|romdump) ;; *) echo "usage: $0 ddr|spl|monitor|ours|uboot|ramdump|romdump"; exit 1 ;; esac

boot=/Volumes/BOOT
[ -f "$boot/boot.ini" ] || { echo "No BOOT partition mounted at $boot. Is the card in?"; exit 1; }
if ls /Volumes/EASYROMS/love2d/hungry-croc.love >/dev/null 2>&1; then
  echo "This looks like the games console's card (it has Hungry Croc). Not touching it."
  exit 1
fi

# The whole disk BOOT is on, and make very sure it's an SD card
disk=$(diskutil info "$boot" | awk -F': *' '/Part of Whole/ {print $2}')
info=$(diskutil info "$disk")
echo "$info" | grep -qE "Protocol: *Secure Digital|Removable Media: *Removable" ||
  { echo "$disk doesn't look like an SD card. Not touching it."; exit 1; }
bytes=$(echo "$info" | sed -n 's/.*Disk Size:.*(\([0-9]*\) Bytes).*/\1/p')
[ -n "$bytes" ] && [ "$bytes" -lt 300000000000 ] || { echo "$disk is too big for the console's card. Not touching it."; exit 1; }
raw=/dev/r$disk

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
dd if=$raw of="$tmp/head" bs=1m count=16 2>/dev/null ||
  { echo "Can't read $raw. Try: sudo $0 $what"; exit 1; }
python3 -I - "$tmp/head" <<'PY'
import sys, struct
h = open(sys.argv[1], "rb").read()
first = struct.unpack_from("<I", h, 446 + 8)[0]   # partition 1's first sector
assert h[510:512] == b"\x55\xaa", "no partition table"
assert first >= 32768, "partition 1 starts at sector %d, inside the boot area" % first
assert h[16384 * 512:][:8] == b"LOADER  ", "no LOADER image at sector 16384"
assert h[24576 * 512:][:4] == b"BL3X", "no trust image at sector 24576"
PY
if [ "$what" = ramdump ]; then
  mkdir -p dumps
  out=dumps/ram-$(date +%Y%m%d-%H%M%S).bin
  dd if=$raw of="$out" bs=512 skip=12288 count=64 2>/dev/null
  [ -n "$SUDO_USER" ] && chown "$SUDO_USER" dumps "$out"
  python3 -I ram-dump.py "$out" > "${out%.bin}.txt" || { echo "No dump on the card yet."; rm -f "$out" "${out%.bin}.txt"; exit 1; }
  [ -n "$SUDO_USER" ] && chown "$SUDO_USER" "${out%.bin}.txt"
  echo "Saved $out and ${out%.bin}.txt"
  sync
  eject_card "$disk" && echo "Ejected."
  exit 0
fi
if [ "$what" = romdump ]; then
  mkdir -p dumps
  out=dumps/bootrom-$(date +%Y%m%d-%H%M%S)
  dd if=$raw of="$tmp/rom" bs=512 skip=12416 count=129 2>/dev/null
  python3 -I rom-dump.py "$tmp/rom" "$out.bin" > "$out.txt" || { echo "No ROM dump on the card yet."; rm -f "$out.bin" "$out.txt"; exit 1; }
  # AArch64 code, as if loaded at 0xffff0000
  llvm=$(xcrun -f llvm-objdump 2>/dev/null || echo objdump)
  python3 -I - "$out.bin" "$out.o" <<'PY'
import sys
# Wrap the raw ROM in a minimal ELF so objdump knows it's AArch64 code at 0xffff0000
rom = open(sys.argv[1], "rb").read()
import struct
shstr = b"\0.text\0.shstrtab\0"
eh = 64; text_off = eh; str_off = text_off + len(rom); sh_off = (str_off + len(shstr) + 7) & ~7
hdr = struct.pack("<4sBBBBB7xHHIQQQIHHHHHH", b"\x7fELF", 2, 1, 1, 0, 0, 1, 183, 1, 0, 0, sh_off, 0, 64, 0, 0, 64, 3, 2)
null = bytes(64)
text = struct.pack("<IIQQQQIIQQ", 1, 1, 6, 0xffff0000, text_off, len(rom), 0, 0, 4, 0)
strs = struct.pack("<IIQQQQIIQQ", 7, 3, 0, 0, str_off, len(shstr), 0, 0, 1, 0)
body = hdr + rom + shstr
body += bytes(sh_off - len(body)) + null + text + strs
open(sys.argv[2], "wb").write(body)
PY
  "$llvm" -d "$out.o" > "$out.s" 2>&1 || echo "(disassembly failed)"
  rm -f "$out.o"
  [ -n "$SUDO_USER" ] && chown "$SUDO_USER" dumps "$out".*
  cat "$out.txt" | head -12
  echo "Saved $out.bin (the ROM), .txt (its text) and .s (disassembly)"
  sync
  eject_card "$disk" && echo "Ejected."
  exit 0
fi
# Build as the user who ran sudo, so the build files stay theirs
as_user() { if [ -n "$SUDO_USER" ]; then sudo -u "$SUDO_USER" "$@"; else "$@"; fi; }
as_user make uboot.img >/dev/null
as_user make -C ../monitor trust.img >/dev/null
as_user make -C ../spl idbloader.img idbloader-ours.img >/dev/null
mkdir -p backup
# The boot ROM's slot runs from sector 64 to 2047 (31 x 32 KB): pad ours to that
idb=../spl/idbloader.img
[ "$what" = ddr ] && idb=../spl/idbloader-ours.img
head -c $((31 * 32768)) /dev/zero > "$tmp/idb"
dd if=$idb of="$tmp/idb" conv=notrunc 2>/dev/null

# One slot: its name, offset and size (in 32 KB blocks), our image, the
# backup of the original, and which of the two it should hold
swap() {
  local name=$1 at=$2 mb=$3 ours=$4 stock=$5 want=$6
  dd if=$raw of="$tmp/now" bs=32k skip=$at count=$mb 2>/dev/null
  if [ ! -f "$stock" ]; then
    cmp -s "$tmp/now" "$ours" && { echo "Ours is already in the $name slot, and there's no backup of the original."; exit 1; }
    cp "$tmp/now" "$stock"
    echo "Saved the original $name as $stock"
  fi
  [ "$want" = ours ] && new=$ours || new=$stock
  if cmp -s "$tmp/now" "$new"; then
    echo "$name: already there."
  else
    diskutil unmountDisk "$disk" >/dev/null
    dd if="$new" of=$raw bs=32k seek=$at 2>/dev/null
    dd if=$raw of="$tmp/check" bs=32k skip=$at count=$mb 2>/dev/null
    cmp -s "$tmp/check" "$new" || { echo "$name: read back differs! Don't boot from it; run again."; exit 1; }
    echo "$name: written and read back ($want)."
  fi
}
U="swap uboot.img 256 128 uboot.img backup/uboot-stock.img"
T="swap trust.img 384 128 ../monitor/trust.img backup/trust-stock.img"
I="swap idbloader 1 31 $tmp/idb backup/idb-stock.img"
case "$what" in
  ddr)     $U ours;  $T ours;  $I ours ;;
  spl)     $U ours;  $T ours;  $I ours ;;
  monitor) $U ours;  $T ours;  $I stock ;;
  ours)    $U ours;  $T stock; $I stock ;;
  uboot)   $U stock; $T stock; $I stock ;;
esac
sync
eject_card "$disk" && echo "Ejected. Put the card in the learning console."
