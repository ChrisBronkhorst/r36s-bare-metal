#!/bin/bash
# Swap what Rockchip's loader starts, on the learning console's card:
#
#   ./card-loader.sh ours     our kernel instead of U-Boot (no U-Boot at all)
#   ./card-loader.sh uboot    put the original U-Boot back
#
# The card's boot area (before the first partition):
#   sector    64  Rockchip's loader: memory setup, then loads the next two
#   sector 16384  "uboot.img": U-Boot, 4 copies     <- this script swaps it
#   sector 24576  "trust.img": ARM Trusted Firmware and OP-TEE
#   sector 32768  the BOOT partition
# The first time, the original U-Boot is saved in backup/uboot-stock.img.
set -e
cd "$(dirname "$0")"
what=$1
[ "$what" = ours ] || [ "$what" = uboot ] || { echo "usage: $0 ours|uboot"; exit 1; }

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
dd if=$raw of="$tmp/head" bs=1m count=13 2>/dev/null ||
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
dd if=$raw of="$tmp/now" bs=1m skip=8 count=4 2>/dev/null

make uboot.img >/dev/null
mkdir -p backup
if [ ! -f backup/uboot-stock.img ]; then
  cmp -s "$tmp/now" uboot.img && { echo "Our kernel is already there, and there's no backup of U-Boot."; exit 1; }
  cp "$tmp/now" backup/uboot-stock.img
  echo "Saved the original U-Boot as backup/uboot-stock.img"
fi
[ "$what" = ours ] && new=uboot.img || new=backup/uboot-stock.img
if cmp -s "$tmp/now" "$new"; then
  echo "Already there."
else
  diskutil unmountDisk "$disk" >/dev/null
  dd if="$new" of=$raw bs=1m seek=8 2>/dev/null
  dd if=$raw of="$tmp/check" bs=1m skip=8 count=4 2>/dev/null
  cmp -s "$tmp/check" "$new" || { echo "Read back differs! Don't boot from it; run again."; exit 1; }
  echo "Written and read back: $( [ "$what" = ours ] && echo "our kernel" || echo "U-Boot") starts next time."
fi
sync
diskutil eject "$disk" >/dev/null && echo "Ejected. Put the card in the learning console."
