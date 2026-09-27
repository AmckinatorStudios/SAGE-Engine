-- Не тест: скрипт, который тесты ставят на объект (AddComponent("Script")).
local C = {}
C.public = { step = field.integer(1) }
function C:Start()
    self.count = 0
    self.started = true
    Events.on("lt_counter_tick", function() self.count = self.count + self.step end)
end
function C:add(n) self.count = self.count + (n or 1); return self.count end
return C
