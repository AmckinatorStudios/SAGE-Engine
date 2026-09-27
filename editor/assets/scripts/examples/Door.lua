-- Дверь: открывается по событию "door_open" (его шлёт Trigger.lua или
-- Events.lua) и плавно уезжает вверх на open_height.

open_height = 3.0
open_speed = 2.0
is_open = false

local closed_y = 0

function Start()
    closed_y = self.transform.position.y
end

-- Своё действие двери: его можно позвать и снаружи —
-- Scene.find("Door"):call("Open").
function Open()
    if is_open then return end
    is_open = true
    Debug.log(self.name .. " opened")
    Events.emit("door_opened", self.name)
end

-- Events.emit("door_open") зовёт эту функцию сам: On + имя события.
function OnDoorOpen(data)
    Open()
end

function Update(dt)
    local target = is_open and closed_y + open_height or closed_y
    local y = self.transform.position.y
    local step = open_speed * dt
    if math.abs(target - y) <= step then
        self.transform.position.y = target
    elseif y < target then
        self.transform.position.y = y + step
    else
        self.transform.position.y = y - step
    end
end
