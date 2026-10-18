-- Number Pop: one number at a time, counting 0 to 9. The number rises to the
-- middle and waits; any button pops it, says its name, and it turns into that
-- many dots (3 -> three dots). After 9 the whole row 0-9 is counted through
-- once, calmly, then back to 0.
-- On the R36S, ArkOS's gptokeyb turns buttons into keys, so we only listen
-- for keys; the same keys work on a Mac keyboard for testing.

local W, H = 640, 480
local CX, CY = W / 2, H / 2

local COLORS = {
  { 0.95, 0.20, 0.20 }, -- red
  { 1.00, 0.55, 0.10 }, -- orange
  { 1.00, 0.80, 0.05 }, -- yellow
  { 0.20, 0.75, 0.30 }, -- green
  { 0.20, 0.50, 1.00 }, -- blue
  { 0.60, 0.30, 0.90 }, -- purple
  { 1.00, 0.45, 0.75 }, -- pink
}

local FONT_SIZE = 220
local RISE_SECONDS = 0.7   -- how long a new number takes to float up
local DOT_RADIUS = 24
local DOT_SPACING = 60
local DOTS_PER_ROW = 5     -- rows of five, like a ten-frame
local DOT_SECONDS = 1.8    -- how long the dots stay before the next number
local ROW_STEP = 0.7       -- after 9: time per number when counting the row

-- Grown-up exit: hold Start + Select for this long. Nothing else quits.
local QUIT_HOLD_SECONDS = 3

-- state is "rising", "waiting", "dots" or "row"
local state
local stateTime = 0
local digit = -1
local color
local dots = {}
local dotBounce = 0
local rowLit = -1     -- how far along the 0-9 row we are
local popAge, popY    -- set while the popped digit grows and fades
local voices = {}
local popSound
local currentVoice
local font, rowFont
local quitTimer = 0
local time = 0

local function makePop()
  local rate = 22050
  local data = love.sound.newSoundData(math.floor(rate * 0.15), rate, 16, 1)
  for i = 0, data:getSampleCount() - 1 do
    local t = i / rate
    local noise = love.math.random() * 2 - 1
    data:setSample(i, noise * math.exp(-t * 40) * 0.5)
  end
  return love.audio.newSource(data, "static")
end

local function sayDigit()
  -- one voice at a time, so fast mashing doesn't turn into a jumble
  if currentVoice then currentVoice:stop() end
  currentVoice = voices[digit]
  currentVoice:play()
end

local function setState(s)
  state = s
  stateTime = 0
end

local function nextNumber()
  digit = (digit + 1) % 10
  color = COLORS[digit % #COLORS + 1]
  dots = {}
  setState("rising")
end

local function digitY()
  if state == "rising" then
    local t = math.min(1, stateTime / RISE_SECONDS)
    t = 1 - (1 - t) ^ 3 -- ease out, so it slows as it arrives
    return H + FONT_SIZE / 2 + (CY - H - FONT_SIZE / 2) * t
  end
  return CY + math.sin(time * 2.5) * 8 -- gentle bob while waiting
end

local function drawDigit(x, y, scale, alpha)
  local text = tostring(digit)
  local ox, oy = font:getWidth(text) / 2, font:getHeight() / 2
  -- dark outline so the digit stands out against the sky
  love.graphics.setColor(0.15, 0.15, 0.3, alpha)
  for dx = -5, 5, 5 do
    for dy = -5, 5, 5 do
      love.graphics.print(text, x + dx, y + dy, 0, scale, scale, ox, oy)
    end
  end
  love.graphics.setColor(color[1], color[2], color[3], alpha)
  love.graphics.print(text, x, y, 0, scale, scale, ox, oy)
end

local function pop()
  local fromY = digitY()
  popAge, popY = 0, fromY

  -- lay the dots out in rows of five around the middle of the screen
  local rows = math.ceil(digit / DOTS_PER_ROW)
  for i = 1, digit do
    local row = math.floor((i - 1) / DOTS_PER_ROW)
    local col = (i - 1) % DOTS_PER_ROW
    local inRow = math.min(DOTS_PER_ROW, digit - row * DOTS_PER_ROW)
    table.insert(dots, {
      fromY = fromY,
      toX = CX + (col - (inRow - 1) / 2) * DOT_SPACING,
      toY = CY + (row - (rows - 1) / 2) * DOT_SPACING,
    })
  end

  popSound:clone():play()
  sayDigit()
  setState("dots")
end

local function startRow()
  rowLit = -1
  setState("row")
end

function love.load()
  love.mouse.setVisible(false)
  font = love.graphics.newFont("lib/fonts/rounded.ttf", FONT_SIZE)
  popSound = makePop()
  for d = 0, 9 do
    voices[d] = love.audio.newSource("lib/voice/numbers/" .. d .. ".wav", "static")
  end
  rowFont = love.graphics.newFont("lib/fonts/rounded.ttf", 64)
  nextNumber()
end

function love.keypressed(key)
  if state == "rising" or state == "waiting" then
    pop()
  elseif state == "dots" then
    -- pressing again while counting: say it again and bounce the dots
    sayDigit()
    dotBounce = 1
  end
  -- during the row every press just gives a soft pop
  if state == "row" then popSound:clone():play() end
end

function love.update(dt)
  time = time + dt
  stateTime = stateTime + dt
  dotBounce = math.max(0, dotBounce - dt * 3)
  if popAge then
    popAge = popAge + dt
    if popAge > 0.35 then popAge = nil end
  end

  if state == "rising" and stateTime >= RISE_SECONDS then
    setState("waiting")
  elseif state == "dots" and stateTime >= DOT_SECONDS then
    if digit == 9 then startRow() else nextNumber() end
  elseif state == "row" then
    -- light up and say each number in turn
    local step = math.floor(stateTime / ROW_STEP)
    if step > rowLit and step <= 9 then
      rowLit = step
      if currentVoice then currentVoice:stop() end
      currentVoice = voices[step]
      currentVoice:play()
    end
    if stateTime >= ROW_STEP * 10 + 1 then
      digit = -1 -- so nextNumber() starts again at 0
      nextNumber()
    end
  end

  if love.keyboard.isDown("return") and love.keyboard.isDown("escape") then
    quitTimer = quitTimer + dt
    if quitTimer >= QUIT_HOLD_SECONDS then love.event.quit() end
  else
    quitTimer = 0
  end
end

function love.draw()
  love.graphics.clear(0.55, 0.80, 1.00) -- sky
  love.graphics.setFont(font)

  if state == "rising" or state == "waiting" then
    drawDigit(CX, digitY(), 1, 1)
  end

  if popAge then
    local t = popAge / 0.35
    drawDigit(CX, popY, 1 + t * 0.8, 1 - t)
  end

  if state == "dots" then
    local fly = math.min(1, stateTime / 0.25)
    fly = 1 - (1 - fly) ^ 3 -- ease out
    local fade = math.min(1, (DOT_SECONDS - stateTime) / 0.4)
    local lift = math.sin(dotBounce * math.pi) * 20

    if digit == 0 then
      -- zero: an empty ring, "nothing here"
      love.graphics.setLineWidth(8)
      love.graphics.setColor(0.15, 0.15, 0.3, fade)
      love.graphics.circle("line", CX, CY - lift, 60 * fly + 4)
      love.graphics.setColor(color[1], color[2], color[3], fade)
      love.graphics.circle("line", CX, CY - lift, 60 * fly)
    end

    for _, d in ipairs(dots) do
      local x = CX + (d.toX - CX) * fly
      local y = d.fromY + (d.toY - d.fromY) * fly - lift
      love.graphics.setColor(0.15, 0.15, 0.3, fade)
      love.graphics.circle("fill", x, y, DOT_RADIUS + 4)
      love.graphics.setColor(color[1], color[2], color[3], fade)
      love.graphics.circle("fill", x, y, DOT_RADIUS)
    end
  end

  if state == "row" then
    love.graphics.setFont(rowFont)
    for d = 0, 9 do
      local x = 50 + d * 60
      local lit = d <= rowLit
      local c = COLORS[d % #COLORS + 1]
      local text = tostring(d)
      local ox, oy = rowFont:getWidth(text) / 2, rowFont:getHeight() / 2
      local scale = (d == rowLit) and 1.3 or 1
      love.graphics.setColor(0.15, 0.15, 0.3, lit and 1 or 0.25)
      for dx = -3, 3, 3 do
        for dy = -3, 3, 3 do
          love.graphics.print(text, x + dx, CY + dy, 0, scale, scale, ox, oy)
        end
      end
      love.graphics.setColor(c[1], c[2], c[3], lit and 1 or 0.25)
      love.graphics.print(text, x, CY, 0, scale, scale, ox, oy)
    end
  end

  if quitTimer > 0 then
    love.graphics.setColor(1, 1, 1, 0.8)
    love.graphics.rectangle("fill", 0, 0, W * quitTimer / QUIT_HOLD_SECONDS, 8)
  end
end
