#!/bin/bash
# Swap what Rockchip's loader starts, on the learning console's card:
#
#   ./card-loader.sh monitor  our kernel, and our monitor at EL3 (no U-Boot, no ATF)
#   ./card-loader.sh ours     our kernel instead of U-Boot, ARM Trusted Firmware at EL3
#   ./card-loader.sh uboot    the originals: U-Boot, ATF and OP-TEE
#
# The card's boot area (before the first partition):
#   sector    64  Rockchip's loader: memory setup, then loads the next two
#   sector 16384  "uboot.img": U-Boot, 4 copies      <- swapped: our kernel
#   sector 24576  "trust.img": ATF and OP-TEE, 2 copies <- swapped: our monitor
#   sector 32768  the BOOT partition
# The first time, the originals are saved in backup/.
set -e
cd "$(dirname "$0")"
what=$1
case "$what" in monitor|ours|uboot) ;; *) echo "usage: $0 monitor|ours|uboot"; exit 1 ;; esac

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
make uboot.img >/dev/null
make -C ../monitor trust.img >/dev/null
mkdir -p backup

# One slot: its name, offset and size (MB), our image, the backup of the
# original, and which of the two it should hold
swap() {
  local name=$1 at=$2 mb=$3 ours=$4 stock=$5 want=$6
  dd if=$raw of="$tmp/now" bs=1m skip=$at count=$mb 2>/dev/null
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
    dd if="$new" of=$raw bs=1m seek=$at 2>/dev/null
    dd if=$raw of="$tmp/check" bs=1m skip=$at count=$mb 2>/dev/null
    cmp -s "$tmp/check" "$new" || { echo "$name: read back differs! Don't boot from it; run again."; exit 1; }
    echo "$name: written and read back ($want)."
  fi
}
case "$what" in
  monitor) swap uboot.img 8 4 uboot.img backup/uboot-stock.img ours
           swap trust.img 12 4 ../monitor/trust.img backup/trust-stock.img ours ;;
  ours)    swap uboot.img 8 4 uboot.img backup/uboot-stock.img ours
           swap trust.img 12 4 ../monitor/trust.img backup/trust-stock.img stock ;;
  uboot)   swap uboot.img 8 4 uboot.img backup/uboot-stock.img stock
           swap trust.img 12 4 ../monitor/trust.img backup/trust-stock.img stock ;;
esac
sync
diskutil eject "$disk" >/dev/null && echo "Ejected. Put the card in the learning console."
