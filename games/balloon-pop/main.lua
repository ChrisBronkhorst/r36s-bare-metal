-- Balloon Pop: balloons float up; any button pops one and says its color.
-- On the R36S, ArkOS's gptokeyb turns buttons into keys, so we only listen
-- for keys; the same keys work on a Mac keyboard for testing.

local W, H = 640, 480

local COLORS = {
  { name = "red",    rgb = { 0.95, 0.20, 0.20 } },
  { name = "orange", rgb = { 1.00, 0.55, 0.10 } },
  { name = "yellow", rgb = { 1.00, 0.85, 0.10 } },
  { name = "green",  rgb = { 0.20, 0.80, 0.30 } },
  { name = "blue",   rgb = { 0.20, 0.50, 1.00 } },
  { name = "purple", rgb = { 0.60, 0.30, 0.90 } },
  { name = "pink",   rgb = { 1.00, 0.45, 0.75 } },
}

local BALLOON_COUNT = 4
local RX, RY = 56, 70 -- balloon radius

-- Grown-up exit: hold Start + Select for this long. Nothing else quits.
local QUIT_HOLD_SECONDS = 3

local balloons = {}
local confetti = {}
local voices = {}
local popSound
local currentVoice
local lastColor
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

local function pickColor()
  local color
  repeat
    color = COLORS[love.math.random(#COLORS)]
  until color ~= lastColor
  lastColor = color
  return color
end

local function spawnBalloon(y)
  local x
  -- try a few spots so balloons don't stack on top of each other
  for _ = 1, 10 do
    x = love.math.random(RX + 20, W - RX - 20)
    local clear = true
    for _, b in ipairs(balloons) do
      if math.abs(b.x - x) < RX * 2 and math.abs(b.y - y) < RY * 2 then clear = false end
    end
    if clear then break end
  end
  table.insert(balloons, {
    x = x,
    y = y,
    color = pickColor(),
    speed = love.math.random(35, 60),
    swayPhase = love.math.random() * math.pi * 2,
  })
end

local function drawX(b)
  return b.x + math.sin(time * 1.5 + b.swayPhase) * 12
end

local function pop(b)
  local x = drawX(b)
  for i = 1, 40 do
    local angle = love.math.random() * math.pi * 2
    local speed = love.math.random(80, 280)
    table.insert(confetti, {
      x = x, y = b.y,
      dx = math.cos(angle) * speed,
      dy = math.sin(angle) * speed - 60,
      size = love.math.random(10, 18),
      rgb = b.color.rgb,
      age = 0,
    })
  end

  popSound:clone():play()
  -- one voice at a time, so fast mashing doesn't turn into a jumble
  if currentVoice then currentVoice:stop() end
  currentVoice = voices[b.color.name]
  currentVoice:play()
end

function love.load()
  love.mouse.setVisible(false)
  popSound = makePop()
  for _, c in ipairs(COLORS) do
    voices[c.name] = love.audio.newSource("sounds/" .. c.name .. ".wav", "static")
  end
  -- start with balloons already spread over the screen
  for i = 1, BALLOON_COUNT do
    spawnBalloon(H - (i - 0.5) * (H / BALLOON_COUNT) + 40)
  end
end

function love.keypressed(key)
  -- pop the balloon nearest the middle of the screen, so it's easy to see
  local best, bestIndex
  for i, b in ipairs(balloons) do
    if b.y > RY and b.y < H - 20 then
      local d = math.abs(b.y - H / 2)
      if not best or d < best then best, bestIndex = d, i end
    end
  end

  if not bestIndex then
    -- nothing on screen: make one appear right away so every press counts
    spawnBalloon(H / 2)
    bestIndex = #balloons
  end

  pop(table.remove(balloons, bestIndex))
end

function love.update(dt)
  time = time + dt

  for i = #balloons, 1, -1 do
    local b = balloons[i]
    -- rise fast until fully on screen, so the screen never sits empty
    local speed = b.y > H - RY and b.speed * 4 or b.speed
    b.y = b.y - speed * dt
    if b.y < -RY * 2 then table.remove(balloons, i) end
  end
  while #balloons < BALLOON_COUNT do
    spawnBalloon(H + RY + love.math.random(0, 40))
  end

  for i = #confetti, 1, -1 do
    local c = confetti[i]
    c.age = c.age + dt
    c.dy = c.dy + 300 * dt -- gravity
    c.x = c.x + c.dx * dt
    c.y = c.y + c.dy * dt
    if c.age > 1.5 then table.remove(confetti, i) end
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

  for _, b in ipairs(balloons) do
    local x = drawX(b)
    local r, g, bl = unpack(b.color.rgb)

    -- string
    love.graphics.setColor(0.3, 0.3, 0.3)
    love.graphics.setLineWidth(2)
    local points = {}
    for s = 0, 8 do
      local sy = b.y + RY + s * 10
      table.insert(points, x + math.sin(time * 3 + s * 0.8 + b.swayPhase) * 4)
      table.insert(points, sy)
    end
    love.graphics.line(points)

    -- knot and body
    love.graphics.setColor(r * 0.8, g * 0.8, bl * 0.8)
    love.graphics.polygon("fill", x, b.y + RY - 4, x - 8, b.y + RY + 10, x + 8, b.y + RY + 10)
    love.graphics.setColor(r, g, bl)
    love.graphics.ellipse("fill", x, b.y, RX, RY)

    -- shine
    love.graphics.setColor(1, 1, 1, 0.45)
    love.graphics.ellipse("fill", x - RX * 0.4, b.y - RY * 0.4, RX * 0.18, RY * 0.25)
  end

  for _, c in ipairs(confetti) do
    local r, g, bl = unpack(c.rgb)
    love.graphics.setColor(r, g, bl, 1 - c.age / 1.5)
    love.graphics.rectangle("fill", c.x, c.y, c.size, c.size)
  end

  if quitTimer > 0 then
    love.graphics.setColor(1, 1, 1, 0.8)
    love.graphics.rectangle("fill", 0, 0, W * quitTimer / QUIT_HOLD_SECONDS, 8)
  end
end
