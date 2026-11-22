-- Твин из скрипта: объект мягко «дышит», а по пробелу подпрыгивает —
-- вверх, пауза, вниз с отскоком.

---@export_range 1 3
---@tooltip "До какого масштаба дышать"
size = 1.15

---@export_range 0.1 3
---@tooltip "Полпериода, секунды"
period = 0.6

local jumping = false

function start()
    local breath = tween.to(object, "scale", size, period, Ease.InOutSine)
    breath.ping_pong()
end

function update(delta)
    if jumping or not input.is_key_pressed("Space") then return end
    jumping = true
    local position = object.transform.position
    local jump = tween.to(object, "position", position + vector3.up() * 2, 0.3, Ease.OutCubic)
    jump.wait(0.1)
    jump.then_to("position", vector3.new(position.x, position.y, position.z), 0.5, Ease.OutBounce)
    jump.on_complete(function() jumping = false end)
end
