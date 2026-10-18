-- Counting Beads: every press adds one bead to the bar and the voice counts it.
-- The bar takes the Montessori bead-stair color of its length (3 = pink bar),
-- and the numeral shows above it. After 9 the bar slides away and we start
-- again at 1.

local C = require("lib.common")

local BEAD_RADIUS = 28
local BAR_Y = 350
local CARD_Y = 150
local MAX = 9

local count = 0
local beadAge = 0     -- time since the newest bead was added
local cardBounce = 0
local leaving         -- the finished bar sliding away: { count, age }

local function beadX(i, n)
  return C.CX + (i - (n + 1) / 2) * BEAD_RADIUS * 2
end

function love.load()
  C.load()
end

function love.keypressed(key)
  if not C.button(key) then return end
  if count == MAX then
    leaving = { count = count, age = 0 }
    count = 0
  end
  count = count + 1
  beadAge = 0
  cardBounce = 1
  C.tock(0.8 + count * 0.06)
  C.say({ "numbers", count })
end

function love.update(dt)
  C.update(dt)
  beadAge = beadAge + dt
  cardBounce = math.max(0, cardBounce - dt * 3)
  if leaving then
    leaving.age = leaving.age + dt
    if leaving.age > 0.35 then leaving = nil end
  end
end

local function drawBar(n, offsetX, alpha, newest)
  if n == 0 then return end
  local color = C.BEADS[n]
  local left, right = beadX(1, n) - BEAD_RADIUS - 14, beadX(n, n) + BEAD_RADIUS + 14

  -- the wire
  love.graphics.setColor(C.INK[1], C.INK[2], C.INK[3], alpha)
  love.graphics.setLineWidth(4)
  love.graphics.line(left + offsetX, BAR_Y, right + offsetX, BAR_Y)

  for i = 1, n do
    local x, y = beadX(i, n) + offsetX, BAR_Y
    if newest and i == n then
      -- the new bead drops onto the wire
      local t = math.min(1, beadAge / 0.2)
      y = BAR_Y - (1 - t) ^ 2 * 90
    end
    love.graphics.setColor(C.INK[1], C.INK[2], C.INK[3], alpha)
    love.graphics.circle("fill", x, y, BEAD_RADIUS + 3)
    love.graphics.setColor(color[1], color[2], color[3], alpha)
    love.graphics.circle("fill", x, y, BEAD_RADIUS)
    love.graphics.setColor(1, 1, 1, 0.35 * alpha)
    love.graphics.circle("fill", x - 7, y - 8, 6)
  end
end

function love.draw()
  C.clear()

  if leaving then
    local t = leaving.age / 0.35
    drawBar(leaving.count, -t * C.W, 1 - t)
  end

  if count > 0 then
    local scale = 1 + math.sin(cardBounce * math.pi) * 0.12
    C.numberCard(count, C.CX, CARD_Y, 190, 1, scale)
    drawBar(count, 0, 1, true)
  else
    -- before the first press: an empty wire waiting for beads
    love.graphics.setColor(C.INK[1], C.INK[2], C.INK[3], 0.4)
    love.graphics.setLineWidth(4)
    love.graphics.line(C.CX - 120, BAR_Y, C.CX + 120, BAR_Y)
  end

  C.drawQuit()
end
