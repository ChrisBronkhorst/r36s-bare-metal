-- Shared toolkit for our toddler games. Each game links this folder in as
-- "lib" (games/<name>/lib -> ../../shared), so use it with:
--   local C = require("lib.common")
-- and call C.load(), C.update(dt), C.drawQuit() from the game's callbacks.

local C = {}

C.W, C.H = 640, 480 -- R36S screen
C.CX, C.CY = C.W / 2, C.H / 2

-- Calm Montessori-ish look: warm paper background, soft ink.
C.BG = { 0.97, 0.94, 0.88 }
C.INK = { 0.22, 0.19, 0.24 }
C.CONSONANT = { 0.96, 0.72, 0.76 } -- sandpaper letters: consonants on pink
C.VOWEL = { 0.64, 0.79, 0.96 }     -- and vowels on blue

-- Montessori bead stair colors, 1 to 10
C.BEADS = {
  { 0.90, 0.22, 0.22 }, -- 1 red
  { 0.30, 0.70, 0.35 }, -- 2 green
  { 0.98, 0.62, 0.75 }, -- 3 pink
  { 1.00, 0.84, 0.20 }, -- 4 yellow
  { 0.55, 0.80, 0.98 }, -- 5 light blue
  { 0.70, 0.55, 0.90 }, -- 6 lilac
  { 1.00, 1.00, 1.00 }, -- 7 white
  { 0.60, 0.40, 0.25 }, -- 8 brown
  { 0.15, 0.25, 0.65 }, -- 9 dark blue
  { 0.95, 0.75, 0.25 }, -- 10 gold
}

---------------------------------------------------------------- buttons
-- ArkOS's gptokeyb turns R36S buttons into these keys (the Mac keyboard
-- sends the same ones, so testing on the Mac just works).
local KEYS = {
  w = "up", s = "down", a = "left", d = "right",
  up = "up", down = "down", left = "left", right = "right", -- left stick
  z = "a", lshift = "b", rshift = "b", space = "x", b = "y",
  l = "l1", r = "r1", x = "l2", y = "r2",
  ["return"] = "start", escape = "select",
}

-- Returns the R36S button name for a key, or nil for keys it doesn't have.
function C.button(key)
  return KEYS[key]
end

-- Where each button sits, as a direction. The face buttons form a diamond
-- (X top, Y left, A right, B bottom), just like the D-pad.
C.SIDE = {
  up = "up", x = "up",
  left = "left", y = "left", l1 = "left", l2 = "left",
  right = "right", a = "right", r1 = "right", r2 = "right",
  down = "down", b = "down",
}

---------------------------------------------------------------- content
C.letters = {} -- in teaching order, from letters.txt
C.letterIndex = {}

local function loadLetters()
  for line in love.filesystem.lines("lib/letters.txt") do
    if line ~= "" and line:sub(1, 1) ~= "#" then
      local letter, _, word = line:match("^([^|]+)|([^|]*)|([^|]*)|")
      table.insert(C.letters, { letter = letter, word = word })
      C.letterIndex[letter] = #C.letters
    end
  end
end

function C.isVowel(letter)
  return letter == "a" or letter == "e" or letter == "i" or letter == "o" or letter == "u"
end

-- shuffle bag: every item comes once, in random order, before any repeats
function C.bag(items)
  local b = {}
  return function()
    if #b == 0 then
      for _, v in ipairs(items) do table.insert(b, v) end
      for i = #b, 2, -1 do
        local j = love.math.random(i)
        b[i], b[j] = b[j], b[i]
      end
    end
    return table.remove(b)
  end
end

---------------------------------------------------------------- assets
local fonts, pictures, voices = {}, {}, {}

function C.font(size)
  if not fonts[size] then fonts[size] = love.graphics.newFont("lib/fonts/rounded.ttf", size) end
  return fonts[size]
end

function C.picture(letter)
  if not pictures[letter] then
    pictures[letter] = love.graphics.newImage("lib/pictures/" .. letter .. ".png")
    pictures[letter]:setFilter("linear", "linear")
  end
  return pictures[letter]
end

-- category is "numbers", "letters", "words" or "prompts"
local function voice(category, name)
  local key = category .. "/" .. name
  if not voices[key] then
    voices[key] = love.audio.newSource("lib/voice/" .. key .. ".wav", "static")
  end
  return voices[key]
end

---------------------------------------------------------------- speaking
-- C.say({ "letters", "m" }, { "words", "m" }) speaks the clips one after
-- another. Saying something new cuts off whatever was being said, so fast
-- button mashing never turns into a jumble.
local queue, current, gap = {}, nil, 0

function C.say(...)
  if current then current:stop() end
  for _, q in ipairs(queue) do q:stop() end
  queue, current, gap = {}, nil, 0
  for _, clip in ipairs({ ... }) do
    table.insert(queue, voice(clip[1], tostring(clip[2])))
  end
end

function C.speaking()
  return (current and current:isPlaying()) or #queue > 0
end

local function updateSpeech(dt)
  if current and current:isPlaying() then return end
  if #queue == 0 then current = nil return end
  if current then -- short breath between clips
    gap = gap + dt
    if gap < 0.25 then return end
  end
  gap = 0
  current = table.remove(queue, 1)
  current:seek(0)
  current:play()
end

-- soft wooden "tock", for beads and cards
local tock
function C.tock(pitch)
  if not tock then
    local rate = 22050
    local data = love.sound.newSoundData(math.floor(rate * 0.12), rate, 16, 1)
    for i = 0, data:getSampleCount() - 1 do
      local t = i / rate
      local v = math.sin(2 * math.pi * 900 * t) * 0.6 + math.sin(2 * math.pi * 1500 * t) * 0.2
      data:setSample(i, v * math.exp(-t * 45) * 0.4)
    end
    tock = love.audio.newSource(data, "static")
  end
  local s = tock:clone()
  s:setPitch(pitch or 1)
  s:play()
end

---------------------------------------------------------------- drawing
function C.clear()
  love.graphics.clear(C.BG)
end

-- centered text; opts: size, color, alpha, scale, outline (true for a dark rim)
function C.text(str, x, y, opts)
  opts = opts or {}
  local f = C.font(opts.size or 64)
  local scale = opts.scale or 1
  local alpha = opts.alpha or 1
  local ox, oy = f:getWidth(str) / 2, f:getHeight() / 2
  love.graphics.setFont(f)
  if opts.outline then
    local o = math.max(3, (opts.size or 64) / 40)
    love.graphics.setColor(C.INK[1], C.INK[2], C.INK[3], alpha)
    for dx = -o, o, o do
      for dy = -o, o, o do
        love.graphics.print(str, x + dx, y + dy, 0, scale, scale, ox, oy)
      end
    end
  end
  local c = opts.color or C.INK
  love.graphics.setColor(c[1], c[2], c[3], alpha)
  love.graphics.print(str, x, y, 0, scale, scale, ox, oy)
end

-- rounded card centered on x, y with a soft shadow
function C.card(x, y, w, h, color, alpha, scale)
  alpha, scale = alpha or 1, scale or 1
  w, h = w * scale, h * scale
  local r = math.min(w, h) * 0.12
  love.graphics.setColor(0, 0, 0, 0.12 * alpha)
  love.graphics.rectangle("fill", x - w / 2 + 4, y - h / 2 + 6, w, h, r, r)
  love.graphics.setColor(color[1], color[2], color[3], alpha)
  love.graphics.rectangle("fill", x - w / 2, y - h / 2, w, h, r, r)
end

-- picture for a letter's word, centered, fitted into a size x size box
function C.drawPicture(letter, x, y, size, alpha, scale)
  local img = C.picture(letter)
  local s = size / math.max(img:getWidth(), img:getHeight()) * (scale or 1)
  love.graphics.setColor(1, 1, 1, alpha or 1)
  love.graphics.draw(img, x, y, 0, s, s, img:getWidth() / 2, img:getHeight() / 2)
end

-- a letter on its sandpaper-style card: pink for consonants, blue for vowels
function C.letterCard(letter, x, y, size, alpha, scale)
  scale = scale or 1
  local color = C.isVowel(letter) and C.VOWEL or C.CONSONANT
  C.card(x, y, size * 0.8, size, color, alpha, scale)
  C.text(letter, x, y - size * 0.06 * scale, { size = math.floor(size * 0.72), scale = scale, alpha = alpha })
end

-- a numeral on a plain card
function C.numberCard(n, x, y, size, alpha, scale)
  C.card(x, y, size * 0.8, size, { 1, 1, 1 }, alpha, scale)
  C.text(tostring(n), x, y, { size = math.floor(size * 0.72), scale = scale, alpha = alpha })
end

-- n bead-stair dots in rows of five, centered on x, y
function C.dots(n, x, y, radius, alpha, lift)
  local spacing = radius * 2.5
  local rows = math.ceil(n / 5)
  local color = C.BEADS[math.max(1, math.min(10, n))]
  for i = 1, n do
    local row, col = math.floor((i - 1) / 5), (i - 1) % 5
    local inRow = math.min(5, n - row * 5)
    local dx = x + (col - (inRow - 1) / 2) * spacing
    local dy = y + (row - (rows - 1) / 2) * spacing - (lift or 0)
    love.graphics.setColor(C.INK[1], C.INK[2], C.INK[3], alpha or 1)
    love.graphics.circle("fill", dx, dy, radius + 3)
    love.graphics.setColor(color[1], color[2], color[3], alpha or 1)
    love.graphics.circle("fill", dx, dy, radius)
  end
end

---------------------------------------------------------------- quitting
-- Grown-up exit: hold Start + Select for 3 seconds. Nothing else quits.
local QUIT_HOLD_SECONDS = 3
local quitTimer = 0

function C.drawQuit()
  if quitTimer > 0 then
    love.graphics.setColor(C.INK[1], C.INK[2], C.INK[3], 0.6)
    love.graphics.rectangle("fill", 0, 0, C.W * quitTimer / QUIT_HOLD_SECONDS, 8)
  end
end

---------------------------------------------------------------- lifecycle
function C.load()
  love.mouse.setVisible(false)
  loadLetters()
end

function C.update(dt)
  updateSpeech(dt)
  if love.keyboard.isDown("return") and love.keyboard.isDown("escape") then
    quitTimer = quitTimer + dt
    if quitTimer >= QUIT_HOLD_SECONDS then love.event.quit() end
  else
    quitTimer = 0
  end
end

return C
