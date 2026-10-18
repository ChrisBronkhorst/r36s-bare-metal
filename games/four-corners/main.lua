-- Four Corners: four cards in a diamond, laid out like the buttons (and the
-- D-pad). Pressing a button names the card in that spot, so he can choose
-- which one he wants to hear, the device version of pointing. Cards he
-- hasn't tried yet gently breathe; once all four are heard, a new set comes.
-- Start switches between letters and numbers.

local C = require("lib.common")

local CARD = 150
local POS = {
  up    = { C.CX, 95 },
  left  = { 130, C.CY + 10 },
  right = { C.W - 130, C.CY + 10 },
  down  = { C.CX, C.H - 95 },
}
local ORDER = { "up", "left", "right", "down" }
local SHOW_SECONDS = 1.6  -- how long the picture or dots stay in the middle
local NEXT_SET_DELAY = 1.5

local mode = "letters"
local cards = {}          -- side -> { value, heard, bounce }
local letterSet = 0
local nextNumber = C.bag({ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 })
local shown               -- { side, age }: what's showing in the middle
local nextSetTimer
local enter = 0           -- 0..1 as a new set arrives
local time = 0

local function newSet()
  cards = {}
  if mode == "letters" then
    -- letters in teaching order, four at a time
    local count = #C.letters
    for i, side in ipairs(ORDER) do
      local item = C.letters[(letterSet * 4 + i - 1) % count + 1]
      cards[side] = { value = item.letter, bounce = 0 }
    end
    letterSet = (letterSet + 1) % math.ceil(count / 4)
  else
    local used = {}
    for _, side in ipairs(ORDER) do
      local n
      repeat n = nextNumber() until not used[n]
      used[n] = true
      cards[side] = { value = n, bounce = 0 }
    end
  end
  shown, nextSetTimer, enter = nil, nil, 0
end

local function name(side)
  local card = cards[side]
  card.heard = true
  card.bounce = 1
  shown = { side = side, age = 0 }
  if mode == "letters" then
    C.say({ "letters", card.value }, { "words", card.value })
  else
    C.say({ "numbers", card.value })
  end

  local all = true
  for _, s in ipairs(ORDER) do all = all and cards[s].heard end
  if all and not nextSetTimer then nextSetTimer = 0 end
end

function love.load()
  C.load()
  newSet()
end

function love.keypressed(key)
  local b = C.button(key)
  if not b then return end
  if b == "start" then
    mode = mode == "letters" and "numbers" or "letters"
    C.tock(0.7)
    newSet()
  elseif C.SIDE[b] then
    name(C.SIDE[b])
  elseif shown then
    name(shown.side) -- select: say the last one again
  end
end

function love.update(dt)
  C.update(dt)
  time = time + dt
  enter = math.min(1, enter + dt * 3)
  for _, side in ipairs(ORDER) do
    cards[side].bounce = math.max(0, cards[side].bounce - dt * 3)
  end
  if shown then
    shown.age = shown.age + dt
    if shown.age > SHOW_SECONDS then shown = nil end
  end
  if nextSetTimer then
    nextSetTimer = nextSetTimer + dt
    if nextSetTimer > NEXT_SET_DELAY and not C.speaking() then newSet() end
  end
end

function love.draw()
  C.clear()
  local e = 1 - (1 - enter) ^ 3

  for i, side in ipairs(ORDER) do
    local card = cards[side]
    local x, y = POS[side][1], POS[side][2]
    local scale = e * (1 + math.sin(card.bounce * math.pi) * 0.12)
    if not card.heard then
      -- gentle breathing invites a press
      scale = scale * (1 + math.sin(time * 3 + i) * 0.03)
    end
    if mode == "letters" then
      C.letterCard(card.value, x, y, CARD, 1, scale)
    else
      C.numberCard(card.value, x, y, CARD, 1, scale)
    end
  end

  -- the answer in the middle: the word's picture, or that many dots
  if shown then
    local alpha = math.min(1, (SHOW_SECONDS - shown.age) / 0.3)
    local pop = math.min(1, shown.age / 0.15)
    local value = cards[shown.side].value
    if mode == "letters" then
      C.drawPicture(value, C.CX, C.CY + 5, 100, alpha, pop)
    elseif value > 0 then
      C.dots(value, C.CX, C.CY + 10, 11 * pop, alpha)
    else
      love.graphics.setColor(C.INK[1], C.INK[2], C.INK[3], alpha)
      love.graphics.setLineWidth(5)
      love.graphics.circle("line", C.CX, C.CY + 10, 30 * pop)
    end
  end

  C.drawQuit()
end
