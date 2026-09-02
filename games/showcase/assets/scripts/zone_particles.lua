-- ---------------------------------------------------------------------------
-- zone_particles.lua — участок «Частицы и эффекты».
--
-- Частицы показывают не «летающие точки», а ТРИ РАЗНЫХ РЕЖИМА системы, и
-- каждый из них решает свою задачу:
--
--   ПОСТОЯННАЯ СТРУЯ (костёр, дым). Живёт сама, задаётся один раз и стоит
--   ровно столько, сколько частиц в кадре. Так делают огонь, пар, факелы.
--
--   ЗАЛП (взрыв, осколки). Разовая пачка в мировой точке. Так отвечают на
--   событие: удар, попадание, разбитый ящик.
--
--   ЭМИТТЕР НА СУЩНОСТИ. Струя, ПРИВЯЗАННАЯ к объекту: он движется — она с
--   ним. Это отдельный механизм, потому что догонять объект скриптом каждый
--   кадр значит отставать на кадр, и на быстром движении хвост рвётся.
--
-- Здесь есть все три: костёр и дым — струи, «фонтан» на карусели — эмиттер на
-- движущейся сущности, а E — залп.
--
-- БИЛБОРДЫ рядом не для полноты списка: это второй способ нарисовать то, чего
-- нет в геометрии, и у него другая цена. Частица живёт и умирает, билборд
-- стоит; метки над участком дешевле держать билбордами, чем вечной струёй.
-- ---------------------------------------------------------------------------
local Mat = require "mat"

local Z = {}

Z.name = "Частицы и эффекты"
Z.about = "Костёр, дым, фонтан на карусели и билборды. E — залп"
Z.center = {x = 0.0, z = -34.0}

local root
local carousel, nozzle
local fireLamp
local time = 0.0
local marks = {}

-- Огонь: короткая жизнь, малая скорость, тёплый цвет, гаснущий в тёмно-красный.
local function fireConfig()
    local c = ParticlePresets.StoveEmbers()
    c.EmissionRate = 90.0
    c.SpeedMin, c.SpeedMax = 0.8, 2.2
    c.LifetimeMin, c.LifetimeMax = 0.5, 1.1
    c.StartSizeMin, c.StartSizeMax = 0.10, 0.20
    c.EndSizeMin, c.EndSizeMax = 0.02, 0.05
    c.StartColor = Vec4(1.0, 0.72, 0.28, 1.0)
    c.EndColor = Vec4(0.55, 0.12, 0.05, 0.0)
    c.Gravity = 1.4          -- ускорение по Y вверх: горячее поднимается
    return c
end

-- Дым: жизнь долгая, размер растёт, цвет уходит в прозрачный серый. Он же
-- проверяет сортировку полупрозрачного прохода: клубы обязаны накладываться
-- друг на друга, а не мигать по очереди.
local function smokeConfig()
    local c = ParticlePresets.Smoke()
    c.EmissionRate = 22.0
    c.LifetimeMin, c.LifetimeMax = 2.2, 3.8
    c.StartSizeMin, c.StartSizeMax = 0.25, 0.45
    c.EndSizeMin, c.EndSizeMax = 1.4, 2.1
    -- Дым СВЕТЛЫЙ и полупрозрачный. Тёмный столб на светлом небе читается не
    -- как дым, а как дыра в кадре: на просвет настоящий дым почти всегда
    -- светлее фона, а не темнее.
    c.StartColor = Vec4(0.62, 0.61, 0.60, 0.30)
    c.EndColor = Vec4(0.78, 0.78, 0.80, 0.0)
    -- Вверх, но слабее огня: дым легче воздуха, а не выстреливает из костра.
    c.Gravity = 0.9
    -- Снос вбок задаётся разбросом направления: гравитация в движке — одно
    -- число по Y, и «ветра» в ней нет. Вертикальный столб дыма выглядит мёртвым.
    c.DirectionMin = Vec3(0.1, 0.8, -0.25)
    c.DirectionMax = Vec3(0.6, 1.2, 0.25)
    return c
end

local function waterConfig()
    local c = ParticlePresets.WaterSplash()
    c.EmissionRate = 140.0
    c.SpeedMin, c.SpeedMax = 3.5, 5.0
    c.LifetimeMin, c.LifetimeMax = 0.9, 1.5
    c.DirectionMin = Vec3(-0.25, 1.0, -0.25)
    c.DirectionMax = Vec3(0.25, 1.0, 0.25)
    c.StartColor = Vec4(0.62, 0.86, 1.0, 0.95)
    c.EndColor = Vec4(0.30, 0.58, 0.86, 0.0)
    return c
end

function Z.Build()
    root = SpawnObject("Zone Particles")
    SetMeshNone(root)
    local cx, cz = Z.center.x, Z.center.z

    -- Кострище: кольцо камней и тёмное пятно под ним. Без него огонь висит
    -- над клеткой пола и читается как ошибка, а не как костёр.
    local pit = SpawnObject("Fire Pit")
    SetMeshCylinder(pit)
    pit.Transform.Position = Vec3(cx - 6.0, 0.08, cz)
    pit.Transform.Scale = Vec3(2.4, 0.16, 2.4)
    Mat.Concrete(pit, Vec3(0.28, 0.26, 0.24))
    pit:SetParent(root)

    for i = 0, 7 do
        local a = i / 8.0 * math.pi * 2.0
        local s = SpawnObject("Fire Stone")
        SetMeshSphere(s)
        s.Transform.Position = Vec3(cx - 6.0 + math.cos(a) * 1.25, 0.16,
                                    cz + math.sin(a) * 1.25)
        s.Transform.Scale = Vec3(0.42, 0.30, 0.42)
        s.Color = Vec3(0.30, 0.30, 0.32)
        Mat.Apply(s, 0.0, 0.85)
        s:SetParent(root)
    end

    sage.fx.CreateStream("demo_fire", fireConfig(), Vec3(cx - 6.0, 0.25, cz))
    sage.fx.SetStreamActive("demo_fire", true)
    sage.fx.CreateStream("demo_smoke", smokeConfig(), Vec3(cx - 6.0, 1.1, cz))
    sage.fx.SetStreamActive("demo_smoke", true)

    -- Свет костра. Частицы САМИ НЕ СВЕТЯТ — они рисуются как полупрозрачные
    -- спрайты и на освещение сцены не влияют вовсе. Огонь без источника света
    -- рядом выглядит наклейкой; с ним — освещает камни и пол вокруг.
    local lamp = SpawnObject("Fire Light")
    SetMeshNone(lamp)
    lamp.Transform.Position = Vec3(cx - 6.0, 1.0, cz)
    local fl = lamp:AddLight()
    fl.Kind = LightType.Point
    fl.Color = Vec3(1.0, 0.62, 0.28)
    fl.Range = 14.0
    fl.Intensity = 3.0
    lamp:SetParent(root)
    fireLamp = lamp

    -- Карусель с фонтаном: эмиттер, привязанный к движущейся сущности.
    carousel = SpawnObject("Carousel")
    SetMeshCylinder(carousel)
    carousel.Transform.Position = Vec3(cx + 5.0, 0.5, cz)
    carousel.Transform.Scale = Vec3(0.5, 1.0, 0.5)
    carousel.Color = Vec3(0.20, 0.22, 0.26)
    Mat.Apply(carousel, 0.7, 0.35)
    carousel:SetParent(root)

    nozzle = SpawnObject("Fountain")
    SetMeshSphere(nozzle)
    nozzle.Transform.Position = Vec3(3.2, 0.6, 0.0)   -- локально: вынос от оси
    nozzle.Transform.Scale = Vec3(0.3, 0.3, 0.3)
    nozzle.Color = Vec3(0.45, 0.70, 0.92)
    Mat.Apply(nozzle, 0.9, 0.15)
    nozzle:SetParent(carousel)

    local em = nozzle:AddEmitter()
    em.Config = waterConfig()
    em.Active = true
    em.Continuous = true

    -- Билборды: метки, которые всегда повёрнуты к камере. Три штуки на разной
    -- высоте — по ним видно, что поворот честный, а не «повернули один раз».
    -- Размер маленький и намеренно: билборд без текстуры — это сплошной
    -- квадрат, и на полметра он читается как метка, а на два — как забытый в
    -- воздухе куб.
    local labels = {
        {Vec3(cx - 6.0, 2.6, cz), Vec4(1.0, 0.66, 0.28, 0.85)},   -- над костром
        {Vec3(cx + 5.0, 2.8, cz), Vec4(0.45, 0.78, 1.0, 0.85)},   -- над каруселью
        {Vec3(cx, 2.4, cz + 4.0), Vec4(0.95, 0.85, 0.45, 0.85)},  -- над местом залпа
    }
    for _, l in ipairs(labels) do
        local id = sage.fx.AddBillboard(l[1], Vec2(0.4, 0.4))
        sage.fx.SetBillboardTint(id, l[2])
        marks[#marks + 1] = id
    end
end

function Z.Update(dt)
    time = time + dt
    if carousel and carousel:Valid() then
        carousel.Transform.Rotation.y = (time * 42.0) % 360.0
    end
    -- Мерцание костра. Две синусоиды с несовпадающими периодами: одна читается
    -- как ровное «дыхание» механизма, а не как живой огонь.
    if fireLamp and fireLamp:Valid() then
        fireLamp:GetLight().Intensity =
            2.6 + math.sin(time * 6.1) * 0.35 + math.sin(time * 11.3) * 0.22
    end
end

-- Залп. Три пачки подряд с разбросом — одна пачка в одной точке выглядит
-- хлопушкой, а не взрывом.
function Z.Use()
    local cx, cz = Z.center.x, Z.center.z
    local c = ParticlePresets.BlockBreak()
    c.SpeedMin, c.SpeedMax = 3.0, 9.0
    c.LifetimeMin, c.LifetimeMax = 0.6, 1.6
    c.StartColor = Vec4(1.0, 0.85, 0.45, 1.0)
    c.EndColor = Vec4(0.7, 0.25, 0.1, 0.0)
    sage.fx.Emit(c, Vec3(cx, 1.4, cz + 4.0), 60)

    local s = smokeConfig()
    s.StartColor = Vec4(0.5, 0.45, 0.4, 0.7)
    sage.fx.Emit(s, Vec3(cx, 1.6, cz + 4.0), 18)

    -- Вспышка: короткий яркий свет. Без неё взрыв не освещает ничего вокруг, и
    -- вся сцена остаётся такой же, как была, — глаз это замечает сразу.
    local flash = SpawnObject("Blast Flash")
    SetMeshNone(flash)
    flash.Transform.Position = Vec3(cx, 1.6, cz + 4.0)
    local L = flash:AddLight()
    L.Kind = LightType.Point
    L.Color = Vec3(1.0, 0.78, 0.42)
    L.Range = 22.0
    L.Intensity = 9.0
    -- Гаснет по таймеру движка и убирает себя сам: вспышка, которую надо
    -- гасить снаружи, однажды останется гореть.
    --
    -- Компонент в таймере берётся ЗАНОВО, а не через захваченную ссылку L:
    -- хранилище компонентов переезжает при добавлении новых сущностей, и
    -- ссылка, пойманная кадр назад, к моменту срабатывания смотрит в никуда.
    sage.time.Schedule(0.10, function()
        if flash:Valid() then flash:GetLight().Intensity = 4.0 end
    end)
    sage.time.Schedule(0.28, function() if flash:Valid() then sage.scene.Destroy(flash) end end)
    return "Залп"
end

function Z.Solid() return false end

return Z
