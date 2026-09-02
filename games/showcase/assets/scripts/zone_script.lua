-- ---------------------------------------------------------------------------
-- zone_script.lua — участок «Логика: события, таймеры, сохранение».
--
-- Это единственный участок, где смотреть надо не на картинку, а на ПОРЯДОК
-- происходящего. Он показывает то, из чего собрана любая игра на движке и чего
-- не видно ни на одном скриншоте:
--
--   ТАЙМЕРЫ. Отложенный вызов и повтор. Лампы зажигаются не «через десять
--   кадров», а через ровные доли секунды — на любом FPS одинаково.
--
--   СОБЫТИЯ. Лампа не знает про колокол, колокол не знает про лампу: лампа
--   объявляет «шаг такой-то», а кто на это отзовётся — дело подписчиков. Так
--   в игре и разводят системы, которые иначе пришлось бы сшивать вызовами
--   каждой в каждую.
--
--   СОХРАНЕНИЕ. Число запусков переживает выход из демо. Мелочь, которая
--   проверяет весь путь: запись в пользовательский каталог, версию формата и
--   чтение обратно.
--
-- ПОЧЕМУ ЛАМПЫ, А НЕ НАДПИСЬ «событие получено». Порядок и промежутки видно
-- глазами: если таймер врёт, лампы зажгутся неровно, и это заметно без часов.
-- ---------------------------------------------------------------------------
local Mat = require "mat"

local Z = {}

Z.name = "Логика: события и таймеры"
Z.about = "Отложенные вызовы, шина событий и сохранение. E — запустить цепочку"
Z.center = {x = 26.0, z = -26.0}

local SLOT = "showcase"
local SAVE_VERSION = 1
local LAMPS = 6
local STEP = 0.28            -- секунд между лампами

local root
local lamps = {}
local bell, gear
local running = false
local runs = 0
local time = 0.0
local bellHit = 0.0
local subscribed = false

local OFF = Vec3(0.18, 0.19, 0.22)
local ON = Vec3(1.0, 0.78, 0.35)

local function setLamp(i, on)
    local l = lamps[i]
    if l == nil or not l.obj:Valid() then return end
    l.obj.Color = on and ON or OFF
    if on then
        Mat.Glow(l.obj, ON, 3.4)
    else
        Mat.Apply(l.obj, 0.1, 0.5)
    end
    l.obj:GetLight().Intensity = on and 3.0 or 0.0
end

function Z.Build()
    root = SpawnObject("Zone Script")
    SetMeshNone(root)
    local cx, cz = Z.center.x, Z.center.z

    local base = SpawnObject("Script Base")
    SetMeshCube(base)
    base.Transform.Position = Vec3(cx, 0.15, cz)
    base.Transform.Scale = Vec3(14.0, 0.3, 10.0)
    base.Color = Vec3(0.14, 0.15, 0.18)
    Mat.Apply(base, 0.0, 0.75)
    base:SetParent(root)

    -- Ряд ламп: шкала времени, разложенная в пространстве.
    for i = 1, LAMPS do
        local o = SpawnObject("Lamp " .. i)
        SetMeshSphere(o)
        o.Transform.Position = Vec3(cx - (LAMPS - 1) * 0.75 + (i - 1) * 1.5, 1.4, cz - 2.0)
        o.Transform.Scale = Vec3(0.5, 0.5, 0.5)
        o.Color = OFF
        Mat.Apply(o, 0.1, 0.5)
        local L = o:AddLight()
        L.Kind = LightType.Point
        L.Color = ON
        L.Range = 8.0
        L.Intensity = 0.0
        o:SetParent(root)

        local post = SpawnObject("Lamp Post " .. i)
        SetMeshCylinder(post)
        post.Transform.Position = Vec3(o.Transform.Position.x, 0.75, cz - 2.0)
        post.Transform.Scale = Vec3(0.12, 1.3, 0.12)
        post.Color = Vec3(0.3, 0.31, 0.34)
        Mat.Apply(post, 0.7, 0.4)
        post:SetParent(root)

        lamps[i] = {obj = o}
    end

    -- Колокол: подписчик события. Он ничего не знает про лампы — только про
    -- то, что «шаг случился».
    bell = SpawnObject("Bell")
    SetMeshCone(bell)
    bell.Transform.Position = Vec3(cx + 5.5, 1.6, cz + 1.5)
    bell.Transform.Rotation = Vec3(180.0, 0.0, 0.0)
    bell.Transform.Scale = Vec3(1.0, 1.2, 1.0)
    bell.Color = Vec3(0.80, 0.62, 0.28)
    Mat.Apply(bell, 0.95, 0.25)
    bell:SetParent(root)

    -- Шестерня: второй подписчик того же события. Два подписчика, а не один,
    -- специально: одного можно принять за прямой вызов.
    gear = SpawnObject("Gear")
    SetMeshCylinder(gear)
    gear.Transform.Position = Vec3(cx - 5.5, 1.4, cz + 1.5)
    gear.Transform.Rotation = Vec3(90.0, 0.0, 0.0)
    gear.Transform.Scale = Vec3(1.6, 0.25, 1.6)
    gear.Color = Vec3(0.55, 0.58, 0.62)
    Mat.Apply(gear, 0.85, 0.3)
    gear:SetParent(root)

    if not subscribed then
        subscribed = true
        sage.events.On("showcase.step", function(e)
            bellHit = 0.35
            if gear and gear:Valid() then
                -- Шестерня доворачивается на шаг: поворот НАКОПИТЕЛЬНЫЙ, по
                -- нему видно, сколько событий пришло.
                sage.tween.Rotate(gear, Vec3(90.0, gear.Transform.Rotation.y + 30.0, 0.0),
                                  0.22, Ease.QuadOut)
            end
        end)
        sage.events.On("showcase.done", function()
            running = false
        end)
    end

    -- Сколько раз цепочку запускали за всё время. Читается один раз при сборке
    -- участка: сохранение — это состояние, а не источник данных на каждый кадр.
    local saved = sage.save.Read(SLOT)
    runs = (saved and saved.runs) or 0
end

function Z.Update(dt)
    time = time + dt
    -- Колокол качается после удара и затухает. Затухание, а не мгновенный
    -- возврат: иначе на серии событий он просто дрожит.
    if bellHit > 0.0 then bellHit = math.max(0.0, bellHit - dt) end
    if bell and bell:Valid() then
        local swing = bellHit > 0.0 and math.sin(time * 26.0) * bellHit * 22.0 or 0.0
        bell.Transform.Rotation = Vec3(180.0, 0.0, swing)
    end
end

function Z.Use()
    if running then return "Цепочка уже идёт" end
    running = true
    for i = 1, LAMPS do setLamp(i, false) end

    -- Отложенные вызовы вместо счётчика в Update. Разница не в удобстве:
    -- счётчик кадра пришлось бы сравнивать с накопленным временем в каждом
    -- кадре и он сбивался бы на просадке, а таймер движка срабатывает по
    -- ВРЕМЕНИ и не зависит от того, сколько кадров успело пройти.
    for i = 1, LAMPS do
        sage.time.Schedule(i * STEP, function()
            setLamp(i, true)
            -- Событие с полезной нагрузкой: подписчики получают номер шага.
            sage.events.Emit("showcase.step", {step = i})
        end)
    end

    sage.time.Schedule((LAMPS + 1) * STEP, function()
        for i = 1, LAMPS do setLamp(i, false) end
        sage.events.Emit("showcase.done", {})
    end)

    runs = runs + 1
    sage.save.Write(SLOT, {runs = runs}, SAVE_VERSION)
    return string.format("Цепочка пошла. Запусков всего: %d (сохранено)", runs)
end

function Z.Reset()
    for i = 1, LAMPS do setLamp(i, false) end
    running = false
    sage.save.Delete(SLOT)
    runs = 0
    return "Счётчик запусков стёрт"
end

function Z.Solid() return false end

return Z
