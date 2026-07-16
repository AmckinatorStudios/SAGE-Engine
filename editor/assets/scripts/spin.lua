-- Пример скрипта для Play-режима редактора: плавное вращение сущности.
-- Назначь его сущности в Inspector (секция Script), нажми Play — куб
-- закрутится; Stop вернёт сцену в исходное состояние.

function OnStart(entity)
    log("spin.lua attached to: " .. entity.Name)
end

function OnUpdate(entity, dt)
    entity.Transform.Rotation.y = entity.Transform.Rotation.y + dt * 90.0
end
