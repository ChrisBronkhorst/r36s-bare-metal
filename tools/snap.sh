#!/bin/bash
# Take screenshots of a game without playing it:
#   ./tools/snap.sh <game> <out-dir>
# Optional timing (seconds, comma-separated):
#   SNAP_PRESS="0.8,1.7:return" SNAP_SHOT="1.5,3" ./tools/snap.sh <game> <out-dir>
# Copies the game, presses keys at the press times ("time" presses z, which is
# the A button; "time:key" presses that key), saves shotN.png at the shot times.
set -e
cd "$(dirname "$0")/.."
game="$1"; out="$(cd "$2" && pwd)"
press="${SNAP_PRESS:-0.8,1.7,2.6}"
shot="${SNAP_SHOT:-1.5,3}"
tmp=$(mktemp -d)
cp -RL "games/$game/." "$tmp/"
sed -i '' 's/t.identity = ".*"/t.identity = "snap-'"$game"'"/' "$tmp/conf.lua"
cat >> "$tmp/main.lua" <<LUA

-- snapshot harness (added by tools/snap.sh)
local _update, _t = love.update, 0
local _press, _shot, _n = {}, { $shot }, 0
for p in ("$press"):gmatch("[^,]+") do
  local t, k = p:match("^([%d.]+):?(.*)$")
  table.insert(_press, { tonumber(t), k ~= "" and k or "z" })
end
function love.update(dt)
  _update(dt)
  _t = _t + dt
  if _press[1] and _t >= _press[1][1] then love.keypressed(table.remove(_press, 1)[2]) end
  if _shot[1] and _t >= _shot[1] then
    table.remove(_shot, 1); _n = _n + 1
    love.graphics.captureScreenshot("shot" .. _n .. ".png")
    if not _shot[1] then _quitAt = _t + 0.2 end
  end
  if _quitAt and _t >= _quitAt then love.event.quit() end
end
LUA
save="$HOME/Library/Application Support/LOVE/snap-$game"
rm -rf "$save"
./tools/love.app/Contents/MacOS/love "$tmp" 2>&1 | tail -5
mv "$save"/shot*.png "$out/"
rm -rf "$tmp" "$save"
ls "$out" | grep shot
