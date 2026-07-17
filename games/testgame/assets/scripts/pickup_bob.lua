-- Подбираемая монетка: крутится и плавает вверх-вниз вокруг своей стартовой
-- высоты. Стартовая высота запоминается в OnStart через таблицу состояний —
-- скрипт один, сущностей с ним десятки (стресс ScriptEngine).
local baseY = {}
local phase = {}

function OnStart(entity)
    baseY[entity.Id] = entity.Transform.Position.y
    phase[entity.Id] = entity.Id * 0.7 -- рассинхронизировать колебания соседей
end

local t = 0
function OnUpdate(entity, dt)
    t = t + dt
    entity.Transform.Rotation.y = entity.Transform.Rotation.y + dt * 180.0
    local id = entity.Id
    if baseY[id] then
        entity.Transform.Position.y = baseY[id] + 0.18 * math.sin(t * 2.2 + phase[id])
    end
end
