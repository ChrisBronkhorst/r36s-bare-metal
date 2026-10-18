-- Croc: play with our crocodile friend. Every button makes him do something:
--   A chomp · B jump · X wave "hi!" · Y dance
--   D-pad / stick: look and lean that way
--   L1 R1 L2 R2: tickle, he giggles
--   Start / Select: a friendly "rawr!"

local C = require("lib.common")
local Croc = require("lib.croc")

local FLOOR = 400
local croc

local DIRECTIONS = {
  up = { 0, -1 }, down = { 0, 1 }, left = { -1, 0 }, right = { 1, 0 },
}

function love.load()
  C.load()
  croc = Croc.new(C.CX, 450, 0.9)
end

function love.keypressed(key)
  local b = C.button(key)
  if not b then return end
  if b == "a" then
    croc:chomp()
  elseif b == "b" then
    croc:jump()
  elseif b == "x" then
    croc:wave()
    C.say({ "croc", "hi" })
  elseif b == "y" then
    croc:dance()
  elseif DIRECTIONS[b] then
    croc:look(DIRECTIONS[b][1], DIRECTIONS[b][2])
    C.tock(1 + DIRECTIONS[b][1] * 0.15 - DIRECTIONS[b][2] * 0.2)
  elseif b == "l1" or b == "r1" or b == "l2" or b == "r2" then
    croc:giggle()
    C.say({ "croc", "giggle" })
  elseif b == "start" or b == "select" then
    croc:rawr()
    C.say({ "croc", "rawr" })
  end
end

function love.update(dt)
  C.update(dt)
  croc:update(dt)
end

function love.draw()
  Croc.drawRoom(FLOOR)
  croc:draw()
  C.drawQuit()
end
