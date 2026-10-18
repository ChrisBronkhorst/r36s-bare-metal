-- Hungry Croc: a letter card floats up to the croc's mouth. Any button and he
-- chomps it, chews, and says its sound and word ("mmm ... monkey!") while the
-- picture shows in a thought bubble. Then "yum!" and the next card comes.
-- Letters go in teaching order. Start switches to numbers (he eats 0 to 9,
-- and the bubble shows that many dots).

local C = require("lib.common")
local Croc = require("lib.croc")

local FLOOR = 400
local CROC_X, CROC_SCALE = 230, 0.95
local CARD = 120
local CARD_X = 470              -- where the card waits, beside his mouth
local FLY_IN_SECONDS = 0.6
local EAT_SECONDS = 0.3         -- card flying into the mouth
local BUBBLE_SECONDS = 2.2

local croc
local mode = "letters"
local letterIndex = 0
local number = -1
local card      -- { value, state = "arriving"/"waiting"/"eaten", t, x, y }
local bubble    -- { value, mode, t }
local nextTimer -- counts down to the next card once this one is eaten

local function cardValue()
  if mode == "letters" then
    letterIndex = letterIndex % #C.letters + 1
    return C.letters[letterIndex].letter
  end
  number = (number + 1) % 10
  return number
end

local function newCard()
  local _, my = croc:mouthPosition()
  card = { value = cardValue(), mode = mode, state = "arriving", t = 0, y = my }
  croc.hungry = 1
  nextTimer = nil
end

local function sayCard(value, m)
  if m == "letters" then
    C.say({ "letters", value }, { "words", value }, { "croc", "yum" })
  else
    C.say({ "numbers", value }, { "croc", "yum" })
  end
end

local function eat()
  card.state = "eaten"
  card.t = 0
  croc.hungry = 0
  croc:chomp()
end

function love.load()
  C.load()
  croc = Croc.new(CROC_X, 440, CROC_SCALE)
  croc:update(0)
  newCard()
end

function love.keypressed(key)
  local b = C.button(key)
  if not b then return end
  if b == "start" then
    mode = mode == "letters" and "numbers" or "letters"
    C.tock(0.7)
    bubble = nil
    newCard()
    return
  end
  if card and card.state ~= "eaten" then
    eat()
  elseif bubble then
    -- still chewing: say it again, with a happy wiggle
    croc:giggle()
    sayCard(bubble.value, bubble.mode)
  end
end

function love.update(dt)
  C.update(dt)
  croc:update(dt)

  if card then
    card.t = card.t + dt
    if card.state == "arriving" and card.t >= FLY_IN_SECONDS then
      card.state = "waiting"
    elseif card.state == "eaten" and card.t >= EAT_SECONDS then
      -- gulp! show what it was and start chewing
      bubble = { value = card.value, mode = card.mode, t = 0 }
      card = nil
      croc:chew()
      sayCard(bubble.value, bubble.mode)
      nextTimer = 0
    end
  end

  if bubble then
    bubble.t = bubble.t + dt
  end
  if nextTimer then
    nextTimer = nextTimer + dt
    if nextTimer > BUBBLE_SECONDS and not C.speaking() then
      bubble = nil
      newCard()
    end
  end
end

local function drawCard(value, m, x, y, scale, alpha)
  if m == "letters" then
    C.letterCard(value, x, y, CARD, alpha, scale)
  else
    C.numberCard(value, x, y, CARD, alpha, scale)
  end
end

local function drawBubble()
  local a = math.min(1, bubble.t / 0.2)
  local pop = 1 - (1 - a) ^ 3
  local bx, by = 470, 150
  -- little thought dots leading up from his head
  love.graphics.setColor(1, 1, 1, a)
  love.graphics.circle("fill", 330, 170, 9 * pop)
  love.graphics.circle("fill", 355, 150, 13 * pop)
  C.card(bx, by, 250 * pop, 190 * pop, { 1, 1, 1 }, a)
  if bubble.mode == "letters" then
    C.text(bubble.value, bx - 65, by - 5, { size = 90, alpha = a, scale = pop })
    C.drawPicture(bubble.value, bx + 55, by - 5, 110, a, pop)
  elseif bubble.value > 0 then
    C.text(tostring(bubble.value), bx - 70, by - 5, { size = 90, alpha = a, scale = pop })
    C.dots(bubble.value, bx + 45, by, 12 * pop, a)
  else
    C.text("0", bx, by - 5, { size = 90, alpha = a, scale = pop })
  end
end

function love.draw()
  Croc.drawRoom(FLOOR)
  croc:draw()

  if card then
    local mx, my = croc:mouthPosition()
    if card.state == "arriving" then
      -- float in from the right, bobbing a little
      local t = 1 - (1 - math.min(1, card.t / FLY_IN_SECONDS)) ^ 3
      drawCard(card.value, card.mode, C.W + CARD + (CARD_X - C.W - CARD) * t, card.y, 1, 1)
    elseif card.state == "waiting" then
      drawCard(card.value, card.mode, CARD_X, card.y + math.sin(card.t * 3) * 6, 1, 1)
    else
      -- into the mouth, shrinking
      local t = math.min(1, card.t / EAT_SECONDS)
      drawCard(card.value, card.mode, CARD_X + (mx - CARD_X) * t, card.y + (my - card.y) * t, 1 - t * 0.8, 1 - t * 0.5)
    end
  end

  if bubble then drawBubble() end
  C.drawQuit()
end
