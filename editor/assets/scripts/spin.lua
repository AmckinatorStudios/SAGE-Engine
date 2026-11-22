-- Пример скрипта для Play-режима редактора: плавное вращение объекта.
-- Назначь его объекту в Inspector (секция Script), нажми Play — куб
-- закрутится; Stop вернёт сцену в исходное состояние.

---@export_range 0 360
---@tooltip "Градусов в секунду"
speed = 90.0

function start()
    debug.log("spin.lua attached to: " .. object.name)
end

function update(delta)
    object.transform.rotation.y = object.transform.rotation.y + delta * speed
end
