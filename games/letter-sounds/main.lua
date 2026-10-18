-- Letter Sounds: one lowercase letter at a time, on a Montessori-style card
-- (pink for consonants, blue for vowels) next to a picture of its word.
-- Each press says the letter's sound and then the word: "mmm ... monkey".
-- A letter stays for a few presses, then the next one slides in.

local C = require("lib.common")

local PRESSES_PER_LETTER = 3
local CARD_X, PIC_X, Y = 210, 460, 215
local SLIDE_SECONDS = 0.4

local index = 1
local presses = 0
local bounce = 0
local slide           -- { from = old index, age } while the next letter slides in

local function current() return C.letters[index] end

local function sayLetter()
  local l = current().letter
  C.say({ "letters", l }, { "words", l })
  bounce = 1
end

function love.load()
  C.load()
end

function love.keypressed(key)
  if not C.button(key) then return end
  if presses >= PRESSES_PER_LETTER then
    slide = { from = index, age = 0 }
    index = index % #C.letters + 1
    presses = 0
  end
  presses = presses + 1
  sayLetter()
end

function love.update(dt)
  C.update(dt)
  bounce = math.max(0, bounce - dt * 3)
  if slide then
    slide.age = slide.age + dt
    if slide.age >= SLIDE_SECONDS then slide = nil end
  end
end

-- the word under the picture, with its first letter picked out in color
local function drawWord(word, x, y, alpha)
  local f = C.font(40)
  local first, rest = word:sub(1, 1), word:sub(2)
  local w = f:getWidth(word)
  local left = x - w / 2
  love.graphics.setFont(f)
  local c = C.isVowel(first) and { 0.25, 0.45, 0.85 } or { 0.85, 0.30, 0.40 }
  love.graphics.setColor(c[1], c[2], c[3], alpha)
  love.graphics.print(first, left, y)
  love.graphics.setColor(C.INK[1], C.INK[2], C.INK[3], alpha)
  love.graphics.print(rest, left + f:getWidth(first), y)
end

local function drawLetter(i, offsetX, alpha, scale)
  local item = C.letters[i]
  C.letterCard(item.letter, CARD_X + offsetX, Y, 260, alpha, scale)
  C.drawPicture(item.letter, PIC_X + offsetX, Y - 20, 170, alpha, scale)
  drawWord(item.word, PIC_X + offsetX, Y + 90, alpha)
end

function love.draw()
  C.clear()
  local scale = 1 + math.sin(bounce * math.pi) * 0.08

  if slide then
    local t = slide.age / SLIDE_SECONDS
    t = 1 - (1 - t) ^ 3 -- ease out
    drawLetter(slide.from, -t * C.W, 1 - t, 1)
    drawLetter(index, (1 - t) * C.W, 1, 1)
  else
    drawLetter(index, 0, 1, scale)
  end

  -- little dots show how many more presses this letter stays for
  for i = 1, PRESSES_PER_LETTER do
    local filled = i <= presses
    love.graphics.setColor(C.INK[1], C.INK[2], C.INK[3], filled and 0.5 or 0.15)
    love.graphics.circle("fill", C.CX + (i - 2) * 22, C.H - 30, 6)
  end

  C.drawQuit()
end
