-- Draws the menu poster for our Love2d system and saves it as poster.png.
-- Matches the theme's style: dark on the left (where the theme's text goes),
-- art on the right.
local W, H = 640, 480
local COLORS = {
  { 0.95, 0.20, 0.20 }, { 1.00, 0.55, 0.10 }, { 1.00, 0.80, 0.05 },
  { 0.20, 0.75, 0.30 }, { 0.20, 0.50, 1.00 }, { 0.60, 0.30, 0.90 }, { 1.00, 0.45, 0.75 },
}

local function outlined(font, text, x, y, color, angle)
  love.graphics.setFont(font)
  local ox, oy = font:getWidth(text) / 2, font:getHeight() / 2
  love.graphics.setColor(0.15, 0.15, 0.3)
  for dx = -4, 4, 4 do for dy = -4, 4, 4 do
    love.graphics.print(text, x + dx, y + dy, angle, 1, 1, ox, oy)
  end end
  love.graphics.setColor(color)
  love.graphics.print(text, x, y, angle, 1, 1, ox, oy)
end

local function balloon(x, y, color)
  love.graphics.setColor(0.3, 0.3, 0.3)
  love.graphics.setLineWidth(2)
  love.graphics.line(x, y + 50, x - 4, y + 80, x + 3, y + 110)
  love.graphics.setColor(color)
  love.graphics.ellipse("fill", x, y, 40, 50)
  love.graphics.setColor(1, 1, 1, 0.45)
  love.graphics.ellipse("fill", x - 16, y - 20, 7, 12)
end

function love.load()
  local canvas = love.graphics.newCanvas(W, H)
  local big = love.graphics.newFont("rounded.ttf", 130)
  local mid = love.graphics.newFont("rounded.ttf", 90)
  love.graphics.setCanvas(canvas)
  love.graphics.clear(0.55, 0.80, 1.00)

  balloon(560, 90, COLORS[7])
  balloon(300, 380, COLORS[4])
  outlined(big, "1", 360, 120, COLORS[1], -0.15)
  outlined(big, "2", 480, 250, COLORS[3], 0.12)
  outlined(mid, "3", 580, 390, COLORS[5], -0.1)
  love.graphics.setColor(COLORS[2]); love.graphics.circle("fill", 420, 400, 22)
  love.graphics.setColor(COLORS[6]); love.graphics.circle("fill", 470, 420, 16)

  -- fade to black on the left, like the other posters
  for x = 0, W do
    local a = 1 - math.min(1, math.max(0, (x - 140) / 180))
    love.graphics.setColor(0, 0, 0, a)
    love.graphics.rectangle("fill", x, 0, 1, H)
  end

  love.graphics.setCanvas()
  canvas:newImageData():encode("png", "poster.png")
  love.event.quit()
end
