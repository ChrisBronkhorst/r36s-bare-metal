-- What Is This?: a game to play together. One card shows, and nothing is
-- said, so you can ask "What's this?" and he answers. The next press gives
-- the answer (the sound and word with its picture, or the number with its
-- dots), and the press after that brings a new card.
-- Start switches between letters and numbers.

local C = require("lib.common")

local CARD = 250
local SLIDE_SECONDS = 0.35

local mode = "letters"
local value
local revealed = false
local revealAge = 0
local slideAge = SLIDE_SECONDS
local previous                 -- { value, mode } sliding out
local nextLetter, nextNumber

local function newCard()
  if value ~= nil then previous = { value = value, mode = mode } end
  value = mode == "letters" and nextLetter() or nextNumber()
  revealed = false
  slideAge = 0
end

local function reveal()
  revealed = true
  revealAge = 0
  if mode == "letters" then
    C.say({ "letters", value }, { "words", value })
  else
    C.say({ "numbers", value })
  end
end

function love.load()
  C.load()
  local letters = {}
  for _, item in ipairs(C.letters) do table.insert(letters, item.letter) end
  nextLetter = C.bag(letters)
  nextNumber = C.bag({ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 })
  newCard()
  slideAge = SLIDE_SECONDS
end

function love.keypressed(key)
  local b = C.button(key)
  if not b then return end
  if b == "start" then
    mode = mode == "letters" and "numbers" or "letters"
    C.tock(0.7)
    previous = nil
    newCard()
  elseif revealed then
    C.tock()
    newCard()
  else
    reveal()
  end
end

function love.update(dt)
  C.update(dt)
  revealAge = revealAge + dt
  slideAge = math.min(SLIDE_SECONDS, slideAge + dt)
  if slideAge >= SLIDE_SECONDS then previous = nil end
end

local function drawCard(v, m, x, alpha)
  if m == "letters" then
    C.letterCard(v, x, 200, CARD, alpha)
  else
    C.numberCard(v, x, 200, CARD, alpha)
  end
end

function love.draw()
  C.clear()
  local t = slideAge / SLIDE_SECONDS
  t = 1 - (1 - t) ^ 3 -- ease out

  -- while waiting for the answer, the card sits in the middle; once
  -- revealed it moves left to make room for the answer on the right
  local shift = revealed and -110 * math.min(1, revealAge / 0.25) or 0

  if previous then drawCard(previous.value, previous.mode, C.CX - 110 - t * C.W, 1 - t) end
  drawCard(value, mode, C.CX + shift + (1 - t) * C.W, 1)

  if revealed then
    local a = math.min(1, revealAge / 0.25)
    local x = C.CX + 150
    if mode == "letters" then
      C.drawPicture(value, x, 190, 170, a)
      C.text(C.letters[C.letterIndex[value]].word, x, 320, { size = 40, alpha = a })
    elseif value > 0 then
      C.dots(value, x, 200, 20, a)
    else
      love.graphics.setColor(C.INK[1], C.INK[2], C.INK[3], a)
      love.graphics.setLineWidth(6)
      love.graphics.circle("line", x, 200, 45)
    end
  else
    -- a soft question mark where the answer will go
    C.text("?", C.CX, 410, { size = 60, alpha = 0.25 })
  end

  C.drawQuit()
end
