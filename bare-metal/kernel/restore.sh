#!/bin/bash
# Make the learning console boot Linux again.
set -e
boot=/Volumes/BOOT
[ -f "$boot/boot.ini.linux" ] || { echo "No $boot/boot.ini.linux to restore."; exit 1; }
cp "$boot/boot.ini.linux" "$boot/boot.ini"
dot_clean -m "$boot"
echo "boot.ini is back to Linux. (kernel.img is left on the card; it's harmless.)"
