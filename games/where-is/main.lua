-- Where Is...?: two cards, left and right. The voice asks "Where is mmm?"
-- Picking the right one names it with its word and moves on to a new pair.
-- Picking the other one simply names what he picked ("That's sss") and asks
-- again: no buzzer, no wrong-answer sound. Left side: D-pad left, Y, L1, L2.
-- Right side: D-pad right, A, R1, R2. Any other button repeats the question.
-- Start switches between letters and numbers.

local C = require("lib.common")

local CARD = 220
local X = { left = 170, right = C.W - 170 }
local Y = 250
local NEXT_DELAY = 1.2 -- after a right answer, before the next pair

local mode = "letters"
local cards = {}       -- side -> { value, bounce, wiggle }
local target           -- side of the card we're asking for
local found            -- time since the right card was picked
local enter = 0
local nextLetter, nextNumber

local function ask()
  local value = cards[target].value
  if mode == "letters" then
    C.say({ "prompts", "where-is" }, { "letters", value })
  else
    C.say({ "prompts", "where-is" }, { "numbers", value })
  end
end

local function newPair()
  local pick = mode == "letters" and nextLetter or nextNumber
  local a = pick()
  local b
  repeat b = pick() until b ~= a
  target = love.math.random() < 0.5 and "left" or "right"
  local other = target == "left" and "right" or "left"
  cards = {
    [target] = { value = a, bounce = 0, wiggle = 0 },
    [other] = { value = b, bounce = 0, wiggle = 0 },
  }
  found, enter = nil, 0
  ask()
end

local function choose(side)
  local card = cards[side]
  if side == target then
    card.bounce = 1
    found = 0
    if mode == "letters" then
      C.say({ "letters", card.value }, { "words", card.value })
    else
      C.say({ "numbers", card.value })
    end
  else
    card.wiggle = 1
    local clip = mode == "letters" and { "letters", card.value } or { "numbers", card.value }
    local wanted = cards[target].value
    local ask1 = mode == "letters" and { "letters", wanted } or { "numbers", wanted }
    C.say({ "prompts", "thats" }, clip, { "prompts", "where-is" }, ask1)
  end
end

function love.load()
  C.load()
  local letters = {}
  for _, item in ipairs(C.letters) do table.insert(letters, item.letter) end
  nextLetter = C.bag(letters)
  nextNumber = C.bag({ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 })
  newPair()
end

function love.keypressed(key)
  local b = C.button(key)
  if not b then return end
  if b == "start" then
    mode = mode == "letters" and "numbers" or "letters"
    C.tock(0.7)
    newPair()
    return
  end
  if found then return end -- already answered; the next pair is on its way
  local side = C.SIDE[b]
  if side == "left" or side == "right" then
    choose(side)
  else
    ask()
  end
end

function love.update(dt)
  C.update(dt)
  enter = math.min(1, enter + dt * 3)
  for _, card in pairs(cards) do
    card.bounce = math.max(0, card.bounce - dt * 2.5)
    card.wiggle = math.max(0, card.wiggle - dt * 2)
  end
  if found then
    found = found + dt
    if found > NEXT_DELAY and not C.speaking() then newPair() end
  end
end

function love.draw()
  C.clear()
  local e = 1 - (1 - enter) ^ 3

  for side, card in pairs(cards) do
    local x = X[side] + math.sin(card.wiggle * math.pi * 6) * 10 * card.wiggle
    local scale = e * (1 + math.sin(card.bounce * math.pi) * 0.15)
    local alpha = 1
    if found and side ~= target then alpha = math.max(0.25, 1 - found * 2) end
    if mode == "letters" then
      C.letterCard(card.value, x, Y, CARD, alpha, scale)
    else
      C.numberCard(card.value, x, Y, CARD, alpha, scale)
    end
  end

  -- once found, show what it means above the card
  if found then
    local value = cards[target].value
    local a = math.min(1, found / 0.2)
    if mode == "letters" then
      C.drawPicture(value, X[target], 60, 90, a)
    elseif value > 0 then
      C.dots(value, X[target], 60, 10, a)
    end
  end

  C.drawQuit()
end
