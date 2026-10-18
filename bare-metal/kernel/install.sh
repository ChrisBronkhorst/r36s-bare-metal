#!/bin/bash
# Put our kernel on the learning console's SD card (the BOOT partition).
# The first time, the stock boot.ini is saved as boot.ini.linux;
# ./restore.sh puts it back and the console boots Linux again.
set -e
cd "$(dirname "$0")"
boot=/Volumes/BOOT

[ -f "$boot/boot.ini" ] || { echo "No BOOT partition mounted at $boot. Is the card in?"; exit 1; }
# both consoles' cards look the same, so refuse the toddler games card
if ls /Volumes/EASYROMS/love2d/hungry-croc.love >/dev/null 2>&1; then
  echo "This looks like the games console's card (it has Hungry Croc). Not touching it."
  exit 1
fi

make
if [ ! -f "$boot/boot.ini.linux" ]; then
  grep -q "^load mmc 1:1 \${loadaddr} Image" "$boot/boot.ini" || {
    echo "boot.ini isn't the stock Linux one, so not saving it as boot.ini.linux."; exit 1; }
  cp "$boot/boot.ini" "$boot/boot.ini.linux"
  echo "Saved the stock boot.ini as boot.ini.linux"
fi
cp kernel.img boot.ini "$boot/"
dot_clean -m "$boot"
echo "Installed. Eject the card, put it in the learning console, and power on."
