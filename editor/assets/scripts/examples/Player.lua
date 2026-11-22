-- Игрок: WASD двигает объект, пробел — прыжок, столкновения — в консоль.
-- Переменные с ---@export видны в инспекторе (Script → speed, jump_force).
--
-- «Вперёд» берётся у самого объекта (transform.forward() — туда, куда смотрит
-- камера), а не осью мира: повёрнутый игрок обязан идти туда, куда повёрнут.
-- Клавиши здесь для краткости; в игре ходят по действиям InputMap
-- (input.is_action_down("Move")), их можно переназначить.

---@export_range 0 20
---@tooltip "Скорость ходьбы, м/с"
speed = 5.0

---@export_range 0 30
jump_force = 8.0

-- Удары приходят событием «collision»: event.data — тот, с кем столкнулись.
events.connect(object, "collision", function(event)
    debug.log("Hit " .. event.data.name)
end)

function start()
    debug.log("Player started: " .. object.name)
end

function update(delta)
    local ahead = 0
    local side = 0

    if input.is_key_down("W") then ahead = ahead + 1 end
    if input.is_key_down("S") then ahead = ahead - 1 end
    if input.is_key_down("D") then side = side + 1 end
    if input.is_key_down("A") then side = side - 1 end

    local direction = object.transform.forward() * ahead + object.transform.right() * side
    direction.y = 0

    object.transform.position = object.transform.position + direction * speed * delta

    if input.is_key_pressed("Space") then
        debug.log("Jump")
        object.transform.position.y = object.transform.position.y + jump_force * 0.1
    end
end
