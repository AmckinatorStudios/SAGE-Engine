-- Пример скрипта движка SAGE Engine.
-- Вешается на любой GameObject через ScriptEngine::AttachScript().
-- entity — это тот самый GameObject, Transform/Color доступны на чтение и запись.

-- Вызывается один раз, сразу после загрузки скрипта (необязательно определять)
function OnStart(entity)
    log("Скрипт запущен для объекта: " .. entity.Name)
end

-- Вызывается каждый кадр
function OnUpdate(entity, dt)
    entity.Transform.Rotation.y = entity.Transform.Rotation.y + dt * 45.0
end
