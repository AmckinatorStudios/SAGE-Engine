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

    -- Асинхронная загрузка модели: объект создаётся сразу, а его меш грузится
    -- в фоне (JobSystem: чтение+парсинг .obj вне главного потока) и появляется
    -- через несколько кадров — кадр при этом не фризит. См. движковые
    -- ResourceManager::GetModelAsync / SetMeshModelAsync.
    local sphere = SpawnObject("async_sphere")
    sphere.Transform.Position = origin + Vec3.new(0, 2, 0)
    sphere.Color = Vec3.new(0.4, 0.8, 1.0)
    SetMeshModelAsync(sphere, "assets/models/sphere.obj")
    log("Модель сферы поставлена в очередь на асинхронную загрузку")

    -- Tag и Lua — свободное расширение GameObject из скрипта (см. Scene.h):
    -- Tag фильтрует объекты в рендер-проходах по строке, Lua хранит любые
    -- свои данные объекта без правки заголовков движка. Таблица создаётся
    -- лениво при первом обращении к entity.Lua.
    sphere.Tag = "demo_sphere"
    sphere.Lua.spinSpeed = 30.0
    sphere.Lua.customLabel = "hello from lua table"
    log("sphere.Tag = " .. sphere.Tag .. ", sphere.Lua.spinSpeed = " .. tostring(sphere.Lua.spinSpeed)
        .. ", sphere.Lua.customLabel = " .. sphere.Lua.customLabel)

    -- Подтверждаем, что sphere.Lua переживает смену кадров (та же таблица,
    -- не пересоздаётся при каждом обращении) — читаем то же поле спустя время.
    Schedule(0.5, function()
        log("sphere.Lua пережила " .. "0.5с: spinSpeed всё ещё " .. tostring(sphere.Lua.spinSpeed)
            .. ", customLabel = " .. sphere.Lua.customLabel)
    end)

    -- Процедурный меш: движок не знает о вокселях/треугольниках игры — он
    -- просто грузит на GPU то, что Lua посчитала сама (см. SetMeshData).
    -- Здесь — плоский треугольник, но тем же способом Lua-воксельный мир
    -- строит меш чанка из посчитанных face-culling'ом вершин.
    local triObj = SpawnObject("demo_procedural_tri")
    triObj.Transform.Position = origin + Vec3.new(-3, 0, 0)
    triObj.Color = Vec3.new(1.0, 0.5, 0.2)
    local triVerts = {
        {0.0, 0.0, 0.0,  0, 1, 0,  0, 0},
        {1.0, 0.0, 0.0,  0, 1, 0,  1, 0},
        {0.5, 1.0, 0.0,  0, 1, 0,  0.5, 1},
    }
    SetMeshData(triObj, triVerts, {1, 2, 3})
    log("Процедурный треугольник построен через SetMeshData")
    -- Перестраиваем НА МЕСТЕ (тот же объект, другая форма) — подтверждает,
    -- что повторный SetMeshData не плодит новые GL-объекты, а зовёт Reload.
    Schedule(0.3, function()
        local biggerVerts = {
            {0.0, 0.0, 0.0,  0, 1, 0,  0, 0},
            {2.0, 0.0, 0.0,  0, 1, 0,  1, 0},
            {1.0, 2.0, 0.0,  0, 1, 0,  0.5, 1},
        }
        SetMeshData(triObj, biggerVerts, {1, 2, 3})
        log("Процедурный треугольник перестроен НА МЕСТЕ через SetMeshData")
    end)

    -- Освещение из Lua: движок не знает о дне/ночи — это целиком расчёт
    -- скрипта, который просто пишет в те же поля, что рендер читает каждый
    -- кадр (см. SetAmbient/SetSun/AddPointLight выше в ScriptEngine.cpp).
    SetAmbient(Vec3.new(0.5, 0.6, 0.8), Vec3.new(0.2, 0.18, 0.16), 0.35)
    SetSun(Vec3.new(-0.4, -1.0, -0.3), Vec3.new(1.0, 0.95, 0.85), 1.0)
    local lampId = AddPointLight(origin, Vec3.new(1.0, 0.7, 0.4), 1.0, 10.0)
    log("Точечный свет добавлен, id = " .. tostring(lampId))
    SetPointLight(lampId, origin + Vec3.new(0, 1, 0), Vec3.new(1.0, 0.7, 0.4), 1.5, 12.0)
    log("Точечный свет обновлён")

    -- Регистрация ввода из Lua — движок не знает заранее ни одного имени
    -- действия, игра объявляет свою раскладку сама (см. RegisterAction/
    -- BindAction). "DemoDance" — заведомо новое имя, не существовавшее в
    -- C++-раскладке The Boat, что и доказывает: работает без правки движка.
    RegisterAction("DemoDance")
    BindAction("DemoDance", "G")
    BindAction("DemoDance", "MOUSE_MIDDLE")
    log("Action 'DemoDance' зарегистрирован и привязан к G / MOUSE_MIDDLE, IsActionDown = "
        .. tostring(IsActionDown("DemoDance")))

    -- UI целиком из Lua: движок не знает ни про одну игру заранее — весь
    -- канвас (панель, текст, полоска, спрайт, кнопка, тумблер) собирается
    -- отсюда (см. CreateCanvas/Add*/SetWidget* в ScriptEngine.cpp).
    CreateCanvas("demo_ui")
    SetCanvasReferenceSize("demo_ui", 1280, 720)

    AddPanel("demo_ui", {
        Anchor = "TopLeft", Offset = Vec2.new(16, 100), Size = Vec2.new(220, 150),
        Color = Vec3.new(0.05, 0.06, 0.09), Alpha = 0.9, OutlineThickness = 2.0,
    })
    local demoLabelId = AddLabel("demo_ui", {
        Anchor = "TopLeft", Offset = Vec2.new(24, 108), Text = "Lua UI demo", Scale = 1.6,
    })
    local demoBarId = AddProgressBar("demo_ui", {
        Anchor = "TopLeft", Offset = Vec2.new(24, 130), Size = Vec2.new(180, 14),
        Label = "XP", FillColor = Vec3.new(0.3, 0.7, 0.9),
    })
    SetWidgetValue(demoBarId, 0.65)

    local demoClicks = 0
    local demoBtnId = AddButton("demo_ui", {
        Anchor = "TopLeft", Offset = Vec2.new(24, 160), Size = Vec2.new(120, 30),
        Label = "Click me",
        OnClick = function()
            demoClicks = demoClicks + 1
            SetWidgetText(demoLabelId, "Clicked " .. demoClicks .. "x")
            log("Lua UI button clicked, count = " .. demoClicks)
        end,
    })

    local demoToggleId = AddToggle("demo_ui", {
        Anchor = "TopLeft", Offset = Vec2.new(24, 200), Size = Vec2.new(44, 20),
        Label = "Enabled",
        OnChanged = function(value)
            log("Lua UI toggle changed to " .. tostring(value))
        end,
    })
    log("Lua UI canvas 'demo_ui' построен: label=" .. tostring(demoLabelId) .. " bar=" .. tostring(demoBarId)
        .. " button=" .. tostring(demoBtnId) .. " toggle=" .. tostring(demoToggleId))

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
