-- Подбираемая монетка: крутится и плавает вверх-вниз вокруг своей стартовой
-- высоты. Стартовая высота запоминается в OnStart через таблицу состояний —
-- скрипт один, сущностей с ним десятки (стресс ScriptEngine).
local baseY = {}
local phase = {}

function OnStart(entity)
    baseY[entity.Id] = entity.Transform.Position.y
    phase[entity.Id] = entity.Id * 0.7 -- рассинхронизировать колебания соседей
    -- Демонстрация системы твинов: пружинистое появление монетки — масштаб
    -- от нуля к полному с перелётом (Ease.BackOut). Тикает движком твинов.
    local full = entity.Transform.Scale
    entity.Transform.Scale = Vec3(0.01, 0.01, 0.01)
    TweenScale(entity, full, 0.6, Ease.BackOut)
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
