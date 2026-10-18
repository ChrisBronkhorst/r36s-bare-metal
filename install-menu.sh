#!/bin/bash
# Put our menu poster, system text and game list onto the SD card.
set -e
cd "$(dirname "$0")"
roms="/Volumes/EASYROMS"
theme="$roms/themes/R36S-theme-epic"
[ -d "$theme" ] || { echo "SD card or theme not found at $theme"; exit 1; }

mkdir -p "$theme/love2d" "$roms/love2d/images"
cp theme/love2d/theme.xml "$theme/love2d/theme.xml"
cp theme/love2d-poster.png "$theme/_art/posters/love2d.png"
cp theme/gamelist.xml "$roms/love2d/gamelist.xml"
for f in menu-art/*.png; do [ "$(basename "$f")" = love2d-poster.png ] || cp "$f" "$roms/love2d/images/"; done
dot_clean -m "$roms/love2d" "$theme" 2>/dev/null || true
echo "menu installed"
