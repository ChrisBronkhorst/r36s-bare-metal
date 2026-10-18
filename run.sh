#!/bin/bash
# Run a game on the Mac:  ./run.sh button-party
set -e
cd "$(dirname "$0")"
game="${1:?usage: ./run.sh <game-folder-name>}"
exec ./tools/love.app/Contents/MacOS/love "games/$game"
