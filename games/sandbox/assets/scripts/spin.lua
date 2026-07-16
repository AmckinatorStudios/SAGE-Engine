-- Пример скрипта движка SAGE Engine — плавное вращение сущности.
-- Вешается на сущность через ScriptComponent (см. SandboxLayer::OnAttach)
-- или из редактора (Inspector -> Script). entity.Transform доступен на
-- чтение и запись.

function OnStart(entity)
    log("spin.lua attached to: " .. entity.Name)
end

function OnUpdate(entity, dt)
    entity.Transform.Rotation.y = entity.Transform.Rotation.y + dt * 45.0
end
