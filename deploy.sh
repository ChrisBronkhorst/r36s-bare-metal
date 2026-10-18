#!/bin/bash
# Package a game as a .love file and copy it to the R36S SD card.
#   ./deploy.sh button-party
set -e
cd "$(dirname "$0")"
game="${1:?usage: ./deploy.sh <game-folder-name>}"
card="/Volumes/EASYROMS/love2d"

mkdir -p build
rm -f "build/$game.love"
# a .love file is just a zip with main.lua at the top level
(cd "games/$game" && zip -q -r -X "../../build/$game.love" . -x ".*")
echo "built build/$game.love"

if [ ! -d "$card" ]; then
  echo "SD card not found at $card, so only built the file (insert the card and re-run)."
  exit 0
fi

cp "build/$game.love" "$card/"
dot_clean -m "$card" 2>/dev/null || true # remove macOS ._ files the device would list as games
echo "copied to $card/$game.love"
echo "eject with:  diskutil eject /dev/\$(diskutil info /Volumes/EASYROMS | awk '/Part of Whole/ {print \$4}')"
