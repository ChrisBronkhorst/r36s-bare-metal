#!/bin/bash
# Press every R36S button (several times, in different orders) in each game
# and report any Lua error. Usage: ./tools/smoke.sh [game ...]  (default: all)
cd "$(dirname "$0")/.."
games=("$@"); [ ${#games[@]} -eq 0 ] && games=($(ls games))
keys='"w","a","s","d","up","down","left","right","z","lshift","space","b","l","r","x","y","return","escape"'
fail=0
for g in "${games[@]}"; do
  tmp=$(mktemp -d)
  cp -RL "games/$g/." "$tmp/"
  cat >> "$tmp/main.lua" <<LUA

-- smoke test (added by tools/smoke.sh)
local _update, _t, _i, _keys = love.update, 0, 0, { $keys }
function love.update(dt)
  _update(dt)
  _t = _t + dt
  -- every key, 3 rounds, alternating fast mashing and slower presses
  if _t > 0.3 + _i * ((_i % 7 < 3) and 0.05 or 0.35) then
    _i = _i + 1
    local k = _keys[(_i * 5) % #_keys + 1]
    if love.keypressed then love.keypressed(k) end
    if love.keyreleased then love.keyreleased(k) end
  end
  if _i >= #_keys * 3 then print("SMOKE OK"); love.event.quit() end
end
LUA
  ./tools/love.app/Contents/MacOS/love "$tmp" > "$tmp/log.txt" 2>&1 &
  pid=$!
  for _ in $(seq 1 60); do sleep 0.5; kill -0 $pid 2>/dev/null || break; done
  kill $pid 2>/dev/null
  if grep -q "SMOKE OK" "$tmp/log.txt" && ! grep -q "^Error" "$tmp/log.txt"; then
    echo "ok    $g"
  else
    echo "FAIL  $g"; grep -A3 "Error" "$tmp/log.txt" | head -6; fail=1
  fi
  rm -rf "$tmp"
done
exit $fail
