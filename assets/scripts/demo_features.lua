-- Демонстрация расширенного Lua API SAGE Engine (спавн объектов, таймеры,
-- корутины, ввод). Вешается как ОБЪЕКТНЫЙ скрипт (AttachScript) или как
-- УРОВНЕВЫЙ (RunScript) — работает в обоих случаях, просто у уровневого
-- не будет параметра entity в OnUpdate.
--
-- Требует ScriptEngine::BindScene() для SpawnObject/FindObject/DestroyObject
-- и ScriptEngine::BindInput() для IsActionDown/WasActionPressed.

function OnStart(entity)
    log("demo_features.lua запущен")

    -- Vec3 поддерживает арифметику прямо в Lua — писать движение/направления
    -- не сложнее, чем в GLSL или C++
    local origin = Vec3.new(0, 10, 0)
    local offset = Vec3.new(2, 0, 0)
    local target = origin + offset
    log("Цель: " .. tostring(target) .. ", дистанция: " .. tostring(origin:Distance(target)))

    -- Спавн нескольких кубов волной — таймер на каждый, без блокирующих циклов
    for i = 1, 3 do
        local delay = i * 0.5
        Schedule(delay, function()
            local cube = SpawnObject("DemoCube_" .. i)
            SetMeshCube(cube)
            cube.Transform.Position = origin + Vec3.new(i * 1.5, 0, 0)
            cube.Color = Vec3.new(0.2 * i, 0.5, 1.0 - 0.2 * i)
            log("Заспавнен кубик #" .. i)
        end)
    end

    -- Корутина: последовательность действий во времени читается линейно,
    -- без ручного стейт-машины из таймеров
    StartCoroutine(function()
        wait(2.0)
        log("Прошло 2 секунды с начала демо")
        wait(1.0)
        log("Демо-последовательность завершена")
    end)

    -- Повторяющийся таймер с самоотменой по условию
    local pulseCount = 0
    local pulseTimer
    pulseTimer = Repeat(1.0, function()
        pulseCount = pulseCount + 1
        log("pulse " .. pulseCount)
        if pulseCount >= 5 then
            CancelTimer(pulseTimer)
            log("Пульс остановлен")
        end
    end)
end

-- Чтение именованных действий движка — тот же ввод, что использует C++-код
-- игры (см. game/GameActions.h), никакого дублирования раскладки клавиш
function OnUpdate(entity, dt)
    if WasActionPressed("Jump") then
        log("Скрипт увидел прыжок игрока!")
    end
end
