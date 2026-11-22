-- Дверь: открывается по событию "door_open" (его шлёт Trigger.lua или
-- Events.lua) и плавно уезжает вверх на open_height.

---@export_range 0 10
open_height = 3.0

---@export_range 0 10
open_speed = 2.0

is_open = false
local closed_y = 0

function start()
    closed_y = object.transform.position.y
end

-- Своё действие двери: его можно позвать и снаружи —
-- scene.find("Door").call("open").
function open()
    if is_open then return end
    is_open = true
    debug.log(object.name .. " opened")
    events.trigger("door_opened", object.name)
end

-- Любой скрипт может попросить открыть дверь: events.trigger("door_open").
events.connect("door_open", function(event)
    open()
end)

function update(delta)
    local target = is_open and closed_y + open_height or closed_y
    local y = object.transform.position.y
    local step = open_speed * delta
    if math.abs(target - y) <= step then
        object.transform.position.y = target
    elseif y < target then
        object.transform.position.y = y + step
    else
        object.transform.position.y = y - step
    end
end
