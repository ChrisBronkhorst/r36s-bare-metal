function love.conf(t)
  t.identity = "counting-beads"
  t.version = "11.4"
  t.window.title = "Counting Beads"
  t.window.width = 640   -- R36S screen
  t.window.height = 480
  t.window.vsync = 1
  t.modules.physics = false
  t.modules.video = false
end
