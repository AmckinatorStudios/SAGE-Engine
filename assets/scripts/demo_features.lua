-- Демонстрация расширенного Lua API SAGE Engine (спавн объектов, таймеры,
-- корутины, ввод, камера, частицы, билборды). Вешается как ОБЪЕКТНЫЙ скрипт
-- (AttachScript) или как УРОВНЕВЫЙ (RunScript) — работает в обоих случаях,
-- просто у уровневого не будет параметра entity в OnUpdate.
--
-- Требует ScriptEngine::BindScene() для SpawnObject/FindObject/DestroyObject,
-- ScriptEngine::BindInput() для IsActionDown/WasActionPressed,
-- ScriptEngine::BindCamera() для GetCamera(),
-- ScriptEngine::BindParticles() для EmitParticles/CreateParticleStream,
-- ScriptEngine::BindBillboards() для AddBillboard. The Boat привязывает все
-- пять в main.cpp, поэтому здесь используются все сразу; в игре, которая
-- привяжет не всё (например, только BindScene), секции ниже, которым нужна
-- непривязанная система, упадут с понятной ошибкой в лог — это ожидаемо,
-- сам движок не падает (см. ScriptEngine::UpdateAll/AttachScript).

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

    -- Частицы: разовый залп щепок (пресет ParticlePresets.BlockBreak) плюс
    -- непрерывная струя дыма (тот же пресет, что использует печка The Boat),
    -- которая гаснет и удаляется через 6 секунд — см. render/ParticlePresets.h
    EmitParticles(ParticlePresets.BlockBreak(), origin, 10)
    CreateParticleStream("demo_smoke", ParticlePresets.Smoke(), origin + Vec3.new(0, 1, 0))
    SetParticleStreamActive("demo_smoke", true)

    -- Звук: доступно после ScriptEngine::BindAudio() — скрипт может проиграть
    -- эффект в точке мира (3D, микшируется относительно камеры) и разово в 2D.
    -- Через 3 секунды повторяем всплеск как звуковую "отбивку" секции демо.
    PlaySound3D("assets/audio/splash.wav", origin, 0.8)
    Schedule(3.0, function()
        PlaySound("assets/audio/craft.wav", 0.7)
        log("Демо: звук проигран из Lua")
    end)
    Schedule(6.0, function()
        RemoveParticleStream("demo_smoke")
        log("Демо-дым потушен")
    end)

    -- Билборд: однотонный маркер без текстуры (Tint красит сам квад), мигает
    -- прозрачностью через повторяющийся таймер, убирается вместе с дымом
    local markerId = AddBillboard(origin + Vec3.new(0, 2.5, 0), Vec2.new(0.4, 0.4))
    local blinkOn = true
    local blinkTimer
    blinkTimer = Repeat(0.5, function()
        blinkOn = not blinkOn
        local alpha = blinkOn and 1.0 or 0.15
        SetBillboardTint(markerId, Vec4.new(1.0, 0.9, 0.2, alpha))
    end)
    Schedule(6.0, function()
        CancelTimer(blinkTimer)
        RemoveBillboard(markerId)
        log("Демо-маркер убран")
    end)

    -- Камера: временный "наезд" (уменьшение FOV) на 1.5 секунды —
    -- программный камера-эффект (прицеливание, катсцена) через корутину,
    -- без правки позиции, чтобы не сбивать игрока с толку в реальной партии
    StartCoroutine(function()
        local cam = GetCamera()
        local originalFov = cam.Fov
        log("Камера: FOV до наезда = " .. tostring(cam.Fov))
        cam.Fov = originalFov * 0.7
        wait(1.5)
        cam.Fov = originalFov
        log("Камера: FOV восстановлен")
    end)
end

-- Чтение именованных действий движка — тот же ввод, что использует C++-код
-- игры (см. game/GameActions.h), никакого дублирования раскладки клавиш
function OnUpdate(entity, dt)
    if WasActionPressed("Jump") then
        log("Скрипт увидел прыжок игрока!")
    end
end
