-- Our crocodile friend, drawn in code so every part can move.
--   local Croc = require("lib.croc")
--   local croc = Croc.new(x, groundY, scale)
--   croc:update(dt); croc:draw()
-- Actions (each interrupts the last): chomp, jump, wave, dance, giggle,
-- rawr, chew, look(dx, dy). croc.hungry (0..1) holds the mouth a bit open.

local Croc = {}
Croc.__index = Croc

local GREEN = { 0.16, 0.62, 0.24 }
local DARK = { 0.09, 0.45, 0.16 }
local SNOUT = { 0.30, 0.76, 0.30 }
local BELLY = { 0.74, 0.90, 0.28 }
local SCALES = { 0.86, 0.96, 0.48 }
local MOUTH = { 0.55, 0.10, 0.14 }
local TONGUE = { 0.95, 0.45, 0.50 }
local CREST = { 1.00, 0.55, 0.15 }
local INK = { 0.12, 0.10, 0.12 }

---------------------------------------------------------------- sounds
local sounds = {}

local function synth(seconds, fn)
  local rate = 22050
  local data = love.sound.newSoundData(math.floor(rate * seconds), rate, 16, 1)
  for i = 0, data:getSampleCount() - 1 do
    data:setSample(i, fn(i / rate))
  end
  return love.audio.newSource(data, "static")
end

local function loadSounds()
  if sounds.clack then return end
  -- teeth snapping shut: a short woody knock
  sounds.clack = synth(0.12, function(t)
    local v = math.sin(2 * math.pi * 520 * t) + 0.5 * math.sin(2 * math.pi * 1300 * t)
    return v * math.exp(-t * 50) * 0.4
  end)
  -- springy boing for jumps
  sounds.boing = synth(0.45, function(t)
    local f = 180 + 500 * t + 60 * math.sin(t * 40)
    return math.sin(2 * math.pi * f * t) * math.exp(-t * 5) * 0.35
  end)
  -- a little dance tune: five bouncy notes
  local notes = { 0, 4, 7, 4, 12 }
  sounds.tune = synth(1.25, function(t)
    local n = math.min(#notes, math.floor(t / 0.25) + 1)
    local nt = t - (n - 1) * 0.25
    local f = 392 * 2 ^ (notes[n] / 12)
    return math.sin(2 * math.pi * f * t) * math.exp(-nt * 8) * 0.3
  end)
end

local function play(name)
  local s = sounds[name]:clone()
  s:play()
end

---------------------------------------------------------------- setup
function Croc.new(x, ground, scale)
  loadSounds()
  local self = setmetatable({}, Croc)
  self.x, self.ground, self.scale = x, ground, scale or 1
  self.time = 0
  self.hungry = 0
  self.eyes = { x = 0, y = 0 }
  self.lookTarget = { x = 0, y = 0 }
  self.wanderTimer = 2
  self.blinkTimer = 2
  self.blinkAge = 1
  self.pose = {}
  return self
end

function Croc:start(name, duration)
  self.action = { name = name, t = 0, duration = duration, fired = false }
end

function Croc:chomp() self:start("chomp", 0.55) end
function Croc:jump() self:start("jump", 0.75); play("boing") end
function Croc:wave() self:start("wave", 1.3) end
function Croc:dance() self:start("dance", 1.4); play("tune") end
function Croc:giggle() self:start("giggle", 1.0) end
function Croc:rawr() self:start("rawr", 0.9) end
function Croc:chew() self:start("chew", 1.2) end

function Croc:look(dx, dy)
  self:start("look", 0.8)
  self.action.dx, self.action.dy = dx, dy
  self.lookTarget.x, self.lookTarget.y = dx, dy
  self.wanderTimer = 1.5
end

-- where the mouth is on screen, for things that fly into it
function Croc:mouthPosition()
  local p = self.pose
  return self.x, self.ground + ((p.y or 0) - 185 * (p.sy or 1)) * self.scale
end

---------------------------------------------------------------- animation
local function bump(u) return math.sin(math.max(0, math.min(1, u)) * math.pi) end

function Croc:update(dt)
  self.time = self.time + dt
  local t = self.time

  -- idle: breathing, arms hanging slightly out, a hungry mouth if wanted
  local breathe = math.sin(t * 2.2) * 0.015
  local p = {
    y = 0, sx = 1 - breathe / 2, sy = 1 + breathe, lean = 0,
    mouth = self.hungry * (0.35 + math.sin(t * 3) * 0.05),
    armL = 0.18 + math.sin(t * 2.2) * 0.03, armR = -0.18 - math.sin(t * 2.2) * 0.03,
    happy = 0, wide = 0,
  }

  -- eyes wander now and then, and always drift smoothly
  self.wanderTimer = self.wanderTimer - dt
  if self.wanderTimer <= 0 then
    self.wanderTimer = love.math.random(15, 40) / 10
    if love.math.random() < 0.5 then
      self.lookTarget.x, self.lookTarget.y = 0, 0
    else
      self.lookTarget.x = love.math.random(-10, 10) / 10
      self.lookTarget.y = love.math.random(-5, 5) / 10
    end
  end
  local k = math.min(1, dt * 8)
  self.eyes.x = self.eyes.x + (self.lookTarget.x - self.eyes.x) * k
  self.eyes.y = self.eyes.y + (self.lookTarget.y - self.eyes.y) * k

  -- blink every few seconds
  self.blinkTimer = self.blinkTimer - dt
  self.blinkAge = self.blinkAge + dt
  if self.blinkTimer <= 0 then
    self.blinkTimer = love.math.random(20, 50) / 10
    self.blinkAge = 0
  end
  p.blink = self.blinkAge < 0.16 and bump(self.blinkAge / 0.16) or 0

  local a = self.action
  if a then
    a.t = a.t + dt
    local u = a.t
    if a.name == "chomp" then
      if u < 0.15 then p.mouth = u / 0.15
      elseif u < 0.25 then p.mouth = 1
      elseif u < 0.3 then p.mouth = 1 - (u - 0.25) / 0.05
      else
        p.mouth = 0
        if not a.fired then a.fired = true; play("clack") end
        local b = bump((u - 0.3) / 0.25)
        p.sy, p.sx = 1 - 0.07 * b, 1 + 0.05 * b
      end
      p.wide = 1

    elseif a.name == "jump" then
      if u < 0.15 then
        local s = u / 0.15
        p.sy, p.sx = 1 - 0.15 * s, 1 + 0.1 * s
      elseif u < 0.6 then
        local v = (u - 0.15) / 0.45
        p.y = -75 * 4 * v * (1 - v)
        p.sy, p.sx = 1.08, 0.95
        p.armL, p.armR = 1.3, -1.3
        p.mouth = 0.35
        p.happy = 1
      else
        local b = bump((u - 0.6) / 0.15)
        p.sy, p.sx = 1 - 0.12 * b, 1 + 0.08 * b
      end

    elseif a.name == "wave" then
      local e = math.min(1, u / 0.15) * math.min(1, (a.duration - u) / 0.15)
      p.armR = p.armR + (-2.6 + math.sin(u * 14) * 0.35 - p.armR) * e
      p.happy, p.mouth = e, 0.25 * e
      p.lean = -0.05 * e

    elseif a.name == "dance" then
      local e = math.min(1, (a.duration - u) / 0.2)
      local beat = u * 9
      p.lean = math.sin(beat) * 0.15 * e
      p.y = -math.abs(math.sin(beat)) * 22 * e
      p.armL = 0.18 + (1.6 + math.sin(beat) * 0.5) * e
      p.armR = -0.18 + (-1.6 + math.sin(beat) * 0.5) * e
      p.happy, p.mouth = e, 0.3 * e

    elseif a.name == "giggle" then
      local e = 1 - u / a.duration
      local j = math.sin(u * 32) * 0.05 * e
      p.sx, p.sy = 1 + j, 1 - j
      p.armL, p.armR = -0.35 * e + 0.18, 0.35 * e - 0.18
      p.happy, p.mouth = 1, 0.45 * e
      p.lean = math.sin(u * 16) * 0.04 * e

    elseif a.name == "rawr" then
      local open = math.min(1, u / 0.12) * math.min(1, (a.duration - u) / 0.2)
      p.mouth = open
      p.armL, p.armR = 0.18 + 2.0 * open, -0.18 - 2.0 * open
      p.sy, p.sx = 1 + 0.06 * open, 1 - 0.03 * open
      p.lean = math.sin(u * 40) * 0.015 * open
      p.wide = 1

    elseif a.name == "chew" then
      local e = math.min(1, (a.duration - u) / 0.2)
      p.mouth = math.abs(math.sin(u * 11)) * 0.3 * e
      p.happy = 1
      p.sy = 1 + math.sin(u * 22) * 0.02 * e

    elseif a.name == "look" then
      local e = bump(math.min(1, u / a.duration)) -- lean over and back
      p.lean = a.dx * 0.14 * e
      if a.dy < 0 then p.y = -30 * e; p.sy = 1 + 0.05 * e end
      if a.dy > 0 then p.sy, p.sx = 1 - 0.1 * e, 1 + 0.07 * e end
      if a.dx ~= 0 then
        local side = a.dx > 0 and "armR" or "armL"
        p[side] = p[side] + a.dx * -0.9 * e -- reach that way
      end
    end

    if a.t >= a.duration then self.action = nil end
  end

  self.pose = p
end

---------------------------------------------------------------- drawing
local function color(c, alpha) love.graphics.setColor(c[1], c[2], c[3], alpha or 1) end

local function arm(x, angle)
  love.graphics.push()
  love.graphics.translate(x, -168)
  love.graphics.rotate(angle)
  color(DARK)
  love.graphics.ellipse("fill", 0, 44, 22, 48)
  color(GREEN)
  love.graphics.ellipse("fill", 0, 42, 18, 44)
  love.graphics.pop()
end

local function eye(x, p, look)
  -- the green bump the eye sits in
  color(GREEN)
  love.graphics.circle("fill", x, -326, 42)
  local open = 1 - p.blink
  if p.happy > 0.5 then
    -- happy squint: an upside-down U
    color(INK)
    love.graphics.setLineWidth(7)
    love.graphics.arc("line", "open", x, -318, 20, math.pi * 1.1, math.pi * 1.9)
    return
  end
  local ry = (36 + p.wide * 4) * math.max(0.08, open)
  color({ 1, 1, 1 })
  love.graphics.ellipse("fill", x, -330, 31 + p.wide * 3, ry)
  if open > 0.3 then
    local px, py = x + look.x * 11, -330 + look.y * 12
    color(INK)
    love.graphics.circle("fill", px, py, 15)
    color({ 1, 1, 1 })
    love.graphics.circle("fill", px - 5, py - 6, 5)
  end
end

-- the croc's room: soft striped wall and a wooden floor from floorY down
function Croc.drawRoom(floorY)
  local W, H = love.graphics.getDimensions()
  love.graphics.clear(0.99, 0.87, 0.89)
  love.graphics.setColor(1, 0.93, 0.94)
  for x = 0, W, 40 do love.graphics.rectangle("fill", x, 0, 20, floorY) end
  love.graphics.setColor(0.95, 0.80, 0.55)
  love.graphics.rectangle("fill", 0, floorY, W, H - floorY)
  love.graphics.setColor(0.88, 0.70, 0.45)
  for y = floorY + 18, H, 26 do love.graphics.rectangle("fill", 0, y, W, 3) end
end

function Croc:draw()
  local p, s = self.pose, self.scale
  if not p.sx then return end

  -- shadow shrinks as he jumps
  local lift = math.max(0, -p.y) / 200
  love.graphics.setColor(0, 0, 0, 0.12 * (1 - lift))
  love.graphics.ellipse("fill", self.x, self.ground, 100 * s * (1 - lift * 0.5), 16 * s)

  love.graphics.push()
  love.graphics.translate(self.x, self.ground + p.y * s)
  love.graphics.rotate(p.lean)
  love.graphics.scale(s * p.sx, s * p.sy)

  -- crest on top of the head
  color(CREST)
  love.graphics.rectangle("fill", -13, -372, 26, 34, 10, 10)

  -- body and belly
  color(GREEN)
  love.graphics.ellipse("fill", 0, -122, 92, 112)
  color(BELLY)
  love.graphics.ellipse("fill", 0, -108, 60, 86)
  color(SCALES)
  for row = 0, 5 do
    local y = -168 + row * 22
    for col = -2, 2 do
      local x = col * 24 + (row % 2) * 12
      if (x / 52) ^ 2 + ((y + 108) / 76) ^ 2 < 1 then
        love.graphics.polygon("fill", x - 6, y - 4, x + 6, y - 4, x, y + 6)
      end
    end
  end

  -- feet
  color(DARK)
  love.graphics.ellipse("fill", -44, -12, 38, 19)
  love.graphics.ellipse("fill", 44, -12, 38, 19)

  -- head
  color(GREEN)
  love.graphics.ellipse("fill", 0, -262, 102, 78)

  -- mouth: the chin drops as it opens, showing the inside and bottom teeth
  local open = p.mouth
  local chinY = -176 + open * 62
  color(GREEN)
  love.graphics.ellipse("fill", 0, chinY, 78, 20)
  local topY, bottomY = -188, chinY - 12
  if bottomY > topY + 2 then
    color(MOUTH)
    love.graphics.ellipse("fill", 0, (topY + bottomY) / 2, 70, (bottomY - topY) / 2 + 4)
    color(TONGUE)
    love.graphics.ellipse("fill", 0, bottomY - 4, 34, math.min(14, (bottomY - topY) / 3))
    color({ 1, 1, 1 })
    for i = -2, 2 do
      local x = i * 24
      love.graphics.polygon("fill", x - 7, bottomY + 2, x + 7, bottomY + 2, x, bottomY - 11)
    end
  end

  -- snout, nostrils, and the top teeth that always peek out
  color(SNOUT)
  love.graphics.ellipse("fill", 0, -230, 98, 48)
  color(DARK)
  love.graphics.ellipse("fill", -26, -250, 6, 8)
  love.graphics.ellipse("fill", 26, -250, 6, 8)
  color({ 1, 1, 1 })
  for i = -3, 3 do
    local x = i * 20
    love.graphics.polygon("fill", x - 8, -188, x + 8, -188, x, -171)
  end

  -- eyes on top
  eye(-44, p, self.eyes)
  eye(44, p, self.eyes)

  -- arms in front
  arm(-80, p.armL)
  arm(80, p.armR)

  love.graphics.pop()
end

return Croc
