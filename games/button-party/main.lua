-- Button Party: every button makes a burst of color and a happy note.
-- On the R36S, ArkOS's gptokeyb turns buttons into keys, so we only listen
-- for keys; the same keys work on a Mac keyboard for testing.

local W, H = 640, 480

-- key -> button name, color, note (pentatonic so any mash sounds nice)
local BUTTONS = {
  z      = { name = "A",      color = { 1.00, 0.27, 0.27 }, note = 0 },
  lshift = { name = "B",      color = { 1.00, 0.80, 0.10 }, note = 2 },
  rshift = { name = "B",      color = { 1.00, 0.80, 0.10 }, note = 2 },
  space  = { name = "X",      color = { 0.20, 0.60, 1.00 }, note = 4 },
  b      = { name = "Y",      color = { 0.20, 0.85, 0.35 }, note = 7 },
  w      = { name = "Up",     color = { 0.70, 0.40, 1.00 }, note = 9 },
  s      = { name = "Down",   color = { 1.00, 0.50, 0.80 }, note = -3 },
  a      = { name = "Left",   color = { 1.00, 0.55, 0.15 }, note = -5 },
  d      = { name = "Right",  color = { 0.10, 0.85, 0.85 }, note = 5 },
  l      = { name = "L1",     color = { 0.95, 0.95, 0.95 }, note = 12 },
  r      = { name = "R1",     color = { 0.95, 0.95, 0.95 }, note = 14 },
  x      = { name = "L2",     color = { 0.60, 0.90, 0.30 }, note = 16 },
  y      = { name = "R2",     color = { 0.60, 0.90, 0.30 }, note = 19 },
  up     = { name = "Stick Up",    color = { 0.70, 0.40, 1.00 }, note = 9 },
  down   = { name = "Stick Down",  color = { 1.00, 0.50, 0.80 }, note = -3 },
  left   = { name = "Stick Left",  color = { 1.00, 0.55, 0.15 }, note = -5 },
  right  = { name = "Stick Right", color = { 0.10, 0.85, 0.85 }, note = 5 },
  ["return"] = { name = "Start",  color = { 1.00, 1.00, 0.60 }, note = 21 },
  escape     = { name = "Select", color = { 0.60, 1.00, 1.00 }, note = 24 },
}

-- Grown-up exit: hold Start + Select for this long. Nothing else quits.
local QUIT_HOLD_SECONDS = 3

local bursts = {}
local sounds = {}
local bg = { 0.08, 0.08, 0.15 }
local bgTarget = { 0.08, 0.08, 0.15 }
local lastLabel = ""
local lastKey = ""
local labelTimer = 0
local quitTimer = 0
local font

local function makeNote(semitones)
  local rate = 44100
  local length = 0.45
  local freq = 440 * 2 ^ ((semitones - 9) / 12) -- 0 = middle C
  local data = love.sound.newSoundData(math.floor(rate * length), rate, 16, 1)
  for i = 0, data:getSampleCount() - 1 do
    local t = i / rate
    local attack = math.min(1, t / 0.01)
    local decay = math.exp(-t * 7)
    -- soft, bell-ish tone: fundamental plus a quiet octave
    local v = math.sin(2 * math.pi * freq * t) * 0.8
            + math.sin(4 * math.pi * freq * t) * 0.2
    data:setSample(i, v * attack * decay * 0.35) -- 0.35 keeps it gentle
  end
  return love.audio.newSource(data, "static")
end

local function spawnBurst(color)
  local x = love.math.random(100, W - 100)
  local y = love.math.random(90, H - 110)
  local burst = { x = x, y = y, color = color, age = 0, dots = {} }
  for i = 1, 14 do
    local angle = (i / 14) * math.pi * 2
    local speed = love.math.random(120, 260)
    table.insert(burst.dots, {
      dx = math.cos(angle) * speed,
      dy = math.sin(angle) * speed,
      size = love.math.random(10, 22),
    })
  end
  table.insert(bursts, burst)
end

function love.load()
  love.mouse.setVisible(false)
  font = love.graphics.newFont(28)
  for key, button in pairs(BUTTONS) do
    sounds[key] = makeNote(button.note)
  end
end

function love.keypressed(key)
  local button = BUTTONS[key]
  if not button then
    -- unknown key: still reward the press, and show its name for mapping
    button = { name = key, color = { 1, 1, 1 } }
  end

  spawnBurst(button.color)
  bgTarget = { button.color[1] * 0.3, button.color[2] * 0.3, button.color[3] * 0.3 }
  lastLabel = button.name
  lastKey = key
  labelTimer = 2

  local sound = sounds[key]
  if sound then
    sound:clone():play() -- clone so fast mashing overlaps instead of cutting off
  end
end

function love.update(dt)
  for i = #bursts, 1, -1 do
    local b = bursts[i]
    b.age = b.age + dt
    if b.age > 1.2 then table.remove(bursts, i) end
  end

  for i = 1, 3 do
    bg[i] = bg[i] + (bgTarget[i] - bg[i]) * math.min(1, dt * 4)
  end

  labelTimer = math.max(0, labelTimer - dt)

  if love.keyboard.isDown("return") and love.keyboard.isDown("escape") then
    quitTimer = quitTimer + dt
    if quitTimer >= QUIT_HOLD_SECONDS then love.event.quit() end
  else
    quitTimer = 0
  end
end

function love.draw()
  love.graphics.clear(bg[1], bg[2], bg[3])

  for _, b in ipairs(bursts) do
    local t = b.age
    local fade = 1 - t / 1.2
    local r, g, bl = b.color[1], b.color[2], b.color[3]

    -- big central blob that pops then shrinks
    local pop = 70 * math.sin(math.min(t * 6, math.pi / 2)) * fade
    love.graphics.setColor(r, g, bl, fade)
    love.graphics.circle("fill", b.x, b.y, pop)

    -- dots flying outward
    for _, d in ipairs(b.dots) do
      local drag = 1 - math.exp(-t * 3)
      love.graphics.circle("fill", b.x + d.dx * drag, b.y + d.dy * drag, d.size * fade)
    end
  end

  -- small label for the grown-up, so we can learn the button mapping
  if labelTimer > 0 then
    love.graphics.setFont(font)
    love.graphics.setColor(1, 1, 1, math.min(1, labelTimer))
    love.graphics.printf(lastLabel .. "  (" .. lastKey .. ")", 0, H - 50, W, "center")
  end

  if quitTimer > 0 then
    love.graphics.setColor(1, 1, 1, 0.8)
    love.graphics.rectangle("fill", 0, 0, W * quitTimer / QUIT_HOLD_SECONDS, 8)
  end
end
