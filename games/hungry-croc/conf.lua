function love.conf(t)
  t.identity = "hungry-croc"
  t.version = "11.4"
  t.window.title = "Hungry Croc"
  t.window.width = 640   -- R36S screen
  t.window.height = 480
  t.window.vsync = 1
  t.modules.physics = false
  t.modules.video = false
end
