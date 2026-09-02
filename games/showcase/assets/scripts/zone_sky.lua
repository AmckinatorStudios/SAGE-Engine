-- ---------------------------------------------------------------------------
-- zone_sky.lua — участок «Небо, объём и солнце».
--
-- Объёмный свет виден не сам по себе, а НА ПЕРЕКРЫТИИ: луч в чистом воздухе —
-- это ровная дымка, и отличить её от тумана нельзя. Поэтому здесь стоит
-- решётчатая арка: солнце режется её рёбрами на полосы, и «объёмный свет»
-- превращается из слова в то, что видно.
--
-- ВРЕМЯ СУТОК ПЕРЕКЛЮЧАЕТСЯ ЗДЕСЬ, а не идёт само. Демо смотрят пять минут, и
-- ждать заката в них никто не будет; к тому же именно РЕЗКОЕ сравнение полудня
-- и заката показывает, что меняется не только яркость: цвет неба, длина теней,
-- цвет тумана и блик в объективе — всё разное.
--
-- БЛИК В ОБЪЕКТИВЕ включается вместе с низким солнцем и гаснет в полдень. Это
-- не лень, а правило: блик в зените — грязь на весь кадр, и включённым его
-- держат ровно там, где он бывает у настоящей камеры.
-- ---------------------------------------------------------------------------
local Mat = require "mat"

local Z = {}

Z.name = "Небо, объём и солнце"
Z.about = "Объёмный свет, облака и блик в объективе. E — сменить время суток"
Z.center = {x = -26.0, z = 26.0}

local root
local time = 0.0
-- Стартуем НЕ с полудня. Солнце в зените — худший свет для показа: тени
-- короткие и прячутся под предметами, формы плоские, цвета одинаковые. Час до
-- заката даёт длинные тени поперёк платформы и тёплый свет, на котором видно и
-- рельеф пола, и объём предметов.
local phase = 2

-- Время суток по точкам, а не непрерывно. Каждая точка — законченная картинка,
-- и каждая проверяет своё: полдень — жёсткие короткие тени, закат — длинные
-- тени и блик, ночь — что сцена не превращается в чёрный прямоугольник.
local PHASES = {
    {
        name = "полдень",
        dir = Vec3(-0.30, -0.92, -0.25), sun = Vec3(1.0, 0.97, 0.92), power = 3.0,
        sky = {top = Vec3(0.18, 0.38, 0.78), horizon = Vec3(0.70, 0.80, 0.92)},
        -- Туман начинается ЗА платформой (её половина — 53 метра): дымка на
        -- самой площадке съедает контраст клетки, ради которой она сделана.
        fog = {Vec3(0.66, 0.74, 0.85), 110.0, 340.0},
        ambient = 0.22, flare = 0.0,
        vol = {density = 0.012, intensity = 0.45, coverage = 0.30},
    },
    {
        name = "день на исходе",
        dir = Vec3(-0.55, -0.52, -0.44), sun = Vec3(1.0, 0.90, 0.74), power = 2.7,
        sky = {top = Vec3(0.22, 0.40, 0.74), horizon = Vec3(0.86, 0.82, 0.74)},
        fog = {Vec3(0.78, 0.76, 0.72), 100.0, 320.0},
        ambient = 0.20, flare = 0.7,
        vol = {density = 0.016, intensity = 0.75, coverage = 0.36},
    },
    {
        name = "закат",
        dir = Vec3(-0.86, -0.14, -0.30), sun = Vec3(1.0, 0.62, 0.34), power = 2.4,
        sky = {top = Vec3(0.16, 0.24, 0.52), horizon = Vec3(0.98, 0.55, 0.30)},
        fog = {Vec3(0.86, 0.58, 0.42), 70.0, 260.0},
        ambient = 0.17, flare = 1.25,
        vol = {density = 0.030, intensity = 1.20, coverage = 0.45},
    },
    {
        name = "ночь",
        dir = Vec3(-0.35, -0.80, 0.42), sun = Vec3(0.42, 0.52, 0.86), power = 0.35,
        sky = {top = Vec3(0.02, 0.03, 0.09), horizon = Vec3(0.08, 0.11, 0.22)},
        fog = {Vec3(0.07, 0.09, 0.16), 60.0, 240.0},
        ambient = 0.09, flare = 0.0,
        vol = {density = 0.018, intensity = 0.35, coverage = 0.55},
    },
}

local function applyPhase(p)
    -- Солнце пишется ЧЕРЕЗ sage.light.SetSun, а не в GetLighting().Sun: если в
    -- сцене есть сущность-источник направленного света, она перекрывает
    -- солнце настроек, и правка настроек не доходит до кадра вовсе.
    sage.light.SetSun(p.dir, p.sun, p.power)

    local L = sage.light.Get()
    L.AmbientStrength = p.ambient
    L.Skybox.TopColor = p.sky.top
    L.Skybox.HorizonColor = p.sky.horizon
    L.Skybox.Celestials = true
    L.Skybox.StarIntensity = (p.name == "ночь") and 1.0 or 0.0
    L.Fog.Color = p.fog[1]
    L.Fog.Start = p.fog[2]
    L.Fog.End = p.fog[3]

    sage.volumetric.Set({
        enabled = true, shafts = true, clouds = true,
        density = p.vol.density, intensity = p.vol.intensity,
        coverage = p.vol.coverage,
        -- Дальность марша ограничена: объём, считающий дымку на всю сотню
        -- метров, складывается с туманом сцены и съедает горизонт.
        maxDistance = 120.0, heightFalloff = 0.05,
        cloudBottom = 120.0, cloudTop = 320.0,
    })

    sage.lensflare.Set({enabled = p.flare > 0.0, intensity = p.flare})
end

function Z.Build()
    root = SpawnObject("Zone Sky")
    SetMeshNone(root)
    local cx, cz = Z.center.x, Z.center.z

    -- Решётчатая арка: два столба и рёбра поверху. Рёбра редкие и толстые —
    -- частая решётка даёт кашу вместо полос.
    local H, W = 9.0, 12.0
    for _, sx in ipairs({-1, 1}) do
        local leg = SpawnObject("Arch Leg")
        SetMeshCube(leg)
        leg.Transform.Position = Vec3(cx + sx * W * 0.5, H * 0.5, cz)
        leg.Transform.Scale = Vec3(0.8, H, 0.8)
        leg.Color = Vec3(0.22, 0.23, 0.26)
        Mat.Apply(leg, 0.35, 0.55)
        leg:SetParent(root)
    end
    for i = 0, 8 do
        local rib = SpawnObject("Arch Rib " .. i)
        SetMeshCube(rib)
        rib.Transform.Position = Vec3(cx - W * 0.5 + (i + 0.5) * (W / 9.0), H - 0.5, cz)
        rib.Transform.Scale = Vec3(0.5, 1.0, 3.4)
        rib.Color = Vec3(0.26, 0.27, 0.30)
        Mat.Apply(rib, 0.35, 0.55)
        rib:SetParent(root)
    end

    -- Кольцо низких плит вокруг: на них ложатся полосы света от рёбер. Без
    -- «экрана» полосы видно только на полу, то есть под ногами, куда никто не
    -- смотрит.
    for i = 0, 5 do
        local a = math.pi * (0.15 + i * 0.14)
        local o = SpawnObject("Sky Slab " .. i)
        SetMeshCube(o)
        o.Transform.Position = Vec3(cx + math.cos(a) * 9.0, 1.4, cz + math.sin(a) * 9.0)
        o.Transform.Rotation = Vec3(0, -math.deg(a), 0)
        o.Transform.Scale = Vec3(3.0, 2.8, 0.3)
        o.Color = Vec3(0.80, 0.80, 0.82)
        Mat.Apply(o, 0.0, 0.55)
        o:SetParent(root)
    end

    applyPhase(PHASES[phase])
end

function Z.Update(dt) time = time + dt end

function Z.Use()
    phase = (phase % #PHASES) + 1
    applyPhase(PHASES[phase])
    return "Время суток: " .. PHASES[phase].name
end

function Z.Reset()
    phase = 2
    applyPhase(PHASES[phase])
    return "Время суток: " .. PHASES[phase].name
end

function Z.Solid(minX, minY, minZ, maxX, maxY, maxZ)
    local cx, cz = Z.center.x, Z.center.z
    if minY > 9.0 then return false end
    for _, sx in ipairs({-1, 1}) do
        local x = cx + sx * 6.0
        if maxX > x - 0.4 and minX < x + 0.4 and maxZ > cz - 0.4 and minZ < cz + 0.4 then
            return true
        end
    end
    return false
end

return Z
