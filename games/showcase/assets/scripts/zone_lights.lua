-- ---------------------------------------------------------------------------
-- zone_lights.lua — участок «Свет и тени».
--
-- КОЛОННАДА, А НЕ КРАСИВЫЙ ПРЕДМЕТ. Свет проверяется тенями, а тень нужно на
-- что-то отбросить и обо что-то сломать. Ряд колонн даёт и то и другое: тени
-- ложатся на пол длинными полосами (видно направление и мягкость края) и
-- ломаются о соседние колонны (видно, что тень честно считает перекрытие, а не
-- рисует пятно под объектом).
--
-- ТРИ ЦВЕТНЫХ ИСТОЧНИКА ЛЕТАЮТ по кругу. Движение здесь не украшение: у
-- статичного света ошибку в затухании или в переходе между каскадами тени
-- заметить нельзя — она всегда в одном и том же месте и выглядит как часть
-- сцены. Движущийся источник протаскивает её через весь участок.
--
-- ПРОЖЕКТОР сверху — отдельный вид источника со своим конусом и своей тенью.
-- Он же самый требовательный: у конуса видно и мягкость края, и то, как свет
-- сходит на нет к границе.
--
-- СВЕТЯЩИЕСЯ ПОЛОСЫ — это НЕ источники света. Материал со свечением ярче
-- единицы попадает за порог bloom и даёт ореол, но пол под ним не освещает.
-- Стоят они здесь ровно затем, чтобы разницу было видно рядом: полоса светится
-- сама, а круг света на полу — от лампы.
-- ---------------------------------------------------------------------------
local Mat = require "mat"

local Z = {}

Z.name = "Свет и тени"
Z.about = "Колоннада, три бегущих источника и прожектор. E — сменить схему"
Z.center = {x = -34.0, z = 0.0}

local root
local orbs = {}          -- {объект, радиус, скорость, фаза, высота}
local spot
local strips = {}
local time = 0.0
local scheme = 1
local ambientBefore       -- общий свет до входа на участок: его возвращаем на выходе

local COLUMNS = 7
local SCHEMES = {
    {name = "тёплая", colors = {Vec3(1.0, 0.55, 0.25), Vec3(1.0, 0.78, 0.42), Vec3(0.95, 0.35, 0.20)},
     spot = Vec3(1.0, 0.92, 0.78), ambient = 0.30},
    {name = "холодная", colors = {Vec3(0.30, 0.62, 1.0), Vec3(0.45, 0.85, 0.95), Vec3(0.60, 0.45, 1.0)},
     spot = Vec3(0.82, 0.90, 1.0), ambient = 0.26},
    {name = "контраст", colors = {Vec3(1.0, 0.20, 0.30), Vec3(0.20, 1.0, 0.45), Vec3(0.25, 0.45, 1.0)},
     spot = Vec3(1.0, 1.0, 1.0), ambient = 0.16},
    {name = "ночь", colors = {Vec3(0.35, 0.45, 0.95), Vec3(0.25, 0.30, 0.70), Vec3(0.55, 0.35, 0.85)},
     spot = Vec3(0.70, 0.80, 1.0), ambient = 0.10},
}

local function column(x, z, height)
    local o = SpawnObject("Column")
    SetMeshCylinder(o)
    o.Transform.Position = Vec3(x, height * 0.5, z)
    o.Transform.Scale = Vec3(0.7, height, 0.7)
    Mat.Concrete(o, Vec3(0.92, 0.92, 0.94))
    o:SetParent(root)
    return o
end

function Z.Build()
    root = SpawnObject("Zone Lights")
    SetMeshNone(root)
    local cx, cz = Z.center.x, Z.center.z

    -- Два ряда колонн разной высоты: одинаковые дали бы одинаковые тени, и
    -- проверять по ним было бы нечего.
    for i = 0, COLUMNS - 1 do
        local t = (i - (COLUMNS - 1) * 0.5) * 2.6
        column(cx + t, cz - 3.5, 4.2 + (i % 3) * 0.8)
        column(cx + t, cz + 3.5, 3.4 + ((i + 1) % 3) * 0.9)
    end

    -- Перекрытие над колоннадой: балка, которая режет свет прожектора на
    -- полосы. Без неё конус лежит на полу ровным пятном, и тень от него
    -- проверить нечем.
    local beam = SpawnObject("Light Beam")
    SetMeshCube(beam)
    beam.Transform.Position = Vec3(cx, 5.2, cz)
    beam.Transform.Scale = Vec3(COLUMNS * 2.6, 0.3, 1.0)
    Mat.Concrete(beam, Vec3(0.34, 0.35, 0.38))
    beam:SetParent(root)

    -- Бегущие источники. Сферы при них ВИДИМЫЕ: невидимый источник света
    -- выглядит как блуждающее пятно неизвестно от чего.
    for i = 1, 3 do
        local o = SpawnObject("Orb " .. i)
        SetMeshSphere(o)
        o.Transform.Scale = Vec3(0.35, 0.35, 0.35)
        o.Color = SCHEMES[1].colors[i]
        Mat.Glow(o, SCHEMES[1].colors[i], 3.0)
        local L = o:AddLight()
        L.Kind = LightType.Point
        L.Color = SCHEMES[1].colors[i]
        L.Range = 18.0
        L.Intensity = 4.0
        o:SetParent(root)
        orbs[#orbs + 1] = {obj = o, radius = 6.0 + i * 1.6, speed = 0.55 - i * 0.11,
                           phase = i * 2.1, height = 1.2 + i * 0.7}
    end

    -- Прожектор сверху вниз, слегка наклонённый: строго вертикальный конус
    -- прячет собственную тень под источником.
    spot = SpawnObject("Spot")
    SetMeshCone(spot)
    spot.Transform.Position = Vec3(cx, 8.5, cz + 7.0)
    spot.Transform.Rotation = Vec3(-160.0, 0.0, 0.0)
    spot.Transform.Scale = Vec3(0.9, 0.9, 0.9)
    spot.Color = Vec3(0.15, 0.16, 0.18)
    Mat.Apply(spot, 0.8, 0.3)
    local sl = spot:AddLight()
    sl.Kind = LightType.Spot
    sl.Color = SCHEMES[1].spot
    sl.Range = 30.0
    sl.InnerConeDeg = 16.0
    sl.OuterConeDeg = 30.0
    sl.Intensity = 6.0
    spot:SetParent(root)

    -- Светящиеся полосы вдоль пола.
    for i = 0, 5 do
        local s = SpawnObject("Strip " .. i)
        SetMeshCube(s)
        s.Transform.Position = Vec3(cx - 7.0 + i * 2.8, 0.06, cz + 9.5)
        s.Transform.Scale = Vec3(2.0, 0.08, 0.35)
        -- Сила свечения умеренная: за двойкой цвет уходит в белый, и синяя
        -- полоса перестаёт быть синей — остаётся белая с синим ореолом.
        Mat.Glow(s, Vec3(0.35, 0.85, 1.0), 1.5)
        s.Color = Vec3(0.35, 0.85, 1.0)
        s:SetParent(root)
        strips[#strips + 1] = s
    end
end

function Z.Update(dt)
    time = time + dt
    local cx, cz = Z.center.x, Z.center.z
    for _, o in ipairs(orbs) do
        if o.obj:Valid() then
            local a = time * o.speed * math.pi * 2.0 + o.phase
            o.obj.Transform.Position = Vec3(cx + math.cos(a) * o.radius,
                                            o.height + math.sin(a * 2.0) * 0.6,
                                            cz + math.sin(a) * o.radius)
        end
    end
    -- Прожектор поводит конусом. Медленно: быстрый прожектор превращает участок
    -- в дискотеку, а смотреть надо на край тени.
    if spot and spot:Valid() then
        spot.Transform.Rotation.z = math.sin(time * 0.35) * 14.0
    end
end

function Z.Use()
    scheme = (scheme % #SCHEMES) + 1
    local s = SCHEMES[scheme]
    for i, o in ipairs(orbs) do
        if o.obj:Valid() then
            o.obj:GetLight().Color = s.colors[i]
            o.obj.Color = s.colors[i]
            Mat.Glow(o.obj, s.colors[i], 3.0)
        end
    end
    if spot and spot:Valid() then spot:GetLight().Color = s.spot end
    -- Фон меняется вместе со схемой: цветные источники на ярком общем свете
    -- почти не видны — они добавляют к тому, что уже освещено.
    sage.light.Get().AmbientStrength = s.ambient
    return "Схема света: " .. s.name
end

-- Общий свет — настройка ВСЕЙ сцены, а участок здесь один из восьми. Поэтому
-- он приглушается на входе и возвращается на выходе: иначе схема «ночь»,
-- выбранная здесь, осталась бы висеть над всей платформой, и следующий
-- участок выглядел бы сломанным.
function Z.Enter()
    ambientBefore = sage.light.Get().AmbientStrength
    sage.light.Get().AmbientStrength = SCHEMES[scheme].ambient
end

function Z.Leave()
    if ambientBefore then sage.light.Get().AmbientStrength = ambientBefore end
end

function Z.Reset()
    scheme = 0
    return Z.Use()
end

-- Колонны — твердь: сквозь них ходить нельзя, иначе участок про перекрытие
-- света оказывается участком, где перекрытий нет.
function Z.Solid(minX, minY, minZ, maxX, maxY, maxZ)
    local cx, cz = Z.center.x, Z.center.z
    if minY > 4.2 then return false end
    for i = 0, COLUMNS - 1 do
        local x = cx + (i - (COLUMNS - 1) * 0.5) * 2.6
        for _, z in ipairs({cz - 3.5, cz + 3.5}) do
            if maxX > x - 0.35 and minX < x + 0.35
               and maxZ > z - 0.35 and minZ < z + 0.35 then
                return true
            end
        end
    end
    return false
end

return Z
