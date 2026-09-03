-- ---------------------------------------------------------------------------
-- hub.lua — центр платформы: место, откуда видно, куда идти.
--
-- ЗАЧЕМ ОН НУЖЕН. Игрок появляется в центре пустой стометровой площадки, а
-- участки стоят по краям, за пределами внимания. Без центра первое, что делает
-- человек, — идёт наугад; половина уходит не туда и решает, что демо пустое.
--
-- Поэтому здесь стоят ДОРОЖКИ — по одной к каждому участку, тонированные в его
-- цвет. Дорожка не объясняет ничего словами и работает на любом языке: она
-- просто ведёт. А обелиск в центре даёт точку отсчёта, к которой возвращаются.
--
-- Дорожка — та же плоскость с процедурной текстурой, что и пол: полосы поперёк
-- хода. Полосы, а не ровная заливка: по ним видно, что дорожка КУДА-ТО ведёт,
-- и с какой скоростью ты по ней идёшь.
-- ---------------------------------------------------------------------------
local Mat = require "mat"

local H = {}

local root
local obelisk
local time = 0.0

-- Цвета дорожек. По одному на участок, в порядке списка участков: цвет — это
-- всё, что отличает одну дорожку от другой, поэтому они взяты заметно разными,
-- а не оттенками одного.
local PATH_COLORS = {
    Vec3(0.90, 0.45, 0.30),   -- физика
    Vec3(0.85, 0.75, 0.35),   -- материалы
    Vec3(0.95, 0.55, 0.25),   -- частицы
    Vec3(0.45, 0.75, 0.95),   -- свет
    Vec3(0.55, 0.85, 0.55),   -- анимация
    Vec3(0.60, 0.60, 0.95),   -- небо
    Vec3(0.90, 0.60, 0.85),   -- логика
    Vec3(0.50, 0.85, 0.85),   -- текстуры
}

local function pathMaterial(i, color)
    local key = "showcase/path_" .. i
    local m = sage.render.NewMaterial(key)
    m.Metallic = 0.0
    m.Roughness = 0.45
    m.Albedo = color
    m.TexturePath = sage.texture.Generate(key .. "_a", {
        pattern = "grid", width = 256, tilesX = 1, tilesY = 4,
        lineWidth = 0.25,
        colorA = Vec3(1.0, 1.0, 1.0), colorB = Vec3(0.45, 0.45, 0.48),
    })
    m.Render.UVScaleX = 1.0
    m.Render.UVScaleY = 6.0
    sage.render.ResolveMaterialTextures(m)
    return key
end

function H.Build(zones)
    root = SpawnObject("Hub")
    SetMeshNone(root)

    -- Площадка центра: круг из цилиндра. Круг, а не квадрат, — у круга нет
    -- «лица», и он не спорит с восемью направлениями, которые от него отходят.
    local pad = SpawnObject("Hub Pad")
    SetMeshCylinder(pad)
    pad.Transform.Position = Vec3(0, 0.06, 0)
    pad.Transform.Scale = Vec3(9.0, 0.12, 9.0)
    Mat.Concrete(pad, Vec3(0.34, 0.35, 0.40))
    pad:SetParent(root)

    -- Обелиск: высокий, тонкий, светящийся сверху. Видно из любой точки
    -- платформы — по нему находят дорогу назад.
    obelisk = SpawnObject("Obelisk")
    SetMeshCube(obelisk)
    obelisk.Transform.Position = Vec3(0, 3.0, 0)
    obelisk.Transform.Scale = Vec3(0.8, 6.0, 0.8)
    Mat.Brushed(obelisk, Vec3(0.30, 0.32, 0.38))
    obelisk:SetParent(root)

    local crown = SpawnObject("Obelisk Crown")
    SetMeshSphere(crown)
    crown.Transform.Position = Vec3(0, 6.4, 0)
    crown.Transform.Scale = Vec3(0.9, 0.9, 0.9)
    crown.Color = Vec3(0.55, 0.85, 1.0)
    Mat.Glow(crown, Vec3(0.55, 0.85, 1.0), 4.0)
    crown:SetParent(root)

    local L = crown:AddLight()
    L.Kind = LightType.Point
    L.Color = Vec3(0.55, 0.85, 1.0)
    L.Range = 26.0
    L.Intensity = 4.0

    -- Дорожки к участкам.
    for i, z in ipairs(zones or {}) do
        local dx, dz = z.center.x, z.center.z
        local len = math.sqrt(dx * dx + dz * dz)
        if len > 1.0 then
            local o = SpawnObject("Path " .. i)
            SetMeshPlane(o)
            -- Дорожка идёт от края площадки до края участка, а не до его
            -- центра: доведённая до центра, она проходит СКВОЗЬ то, что там
            -- стоит, и упирается в стопку ящиков.
            local from, to = 8.0, len - 11.0
            local mid = (from + to) * 0.5
            o.Transform.Position = Vec3(dx / len * mid, 0.02, dz / len * mid)
            o.Transform.Rotation = Vec3(0, math.deg(math.atan(dx, dz)), 0)
            o.Transform.Scale = Vec3(2.2, 1.0, to - from)
            o.Color = PATH_COLORS[((i - 1) % #PATH_COLORS) + 1]
            sage.render.SetMaterial(o, pathMaterial(i, PATH_COLORS[((i - 1) % #PATH_COLORS) + 1]))
            o:SetParent(root)

            -- Столбик у начала дорожки: он подсвечен в тот же цвет и виден
            -- ночью, когда самой дорожки под ногами почти не разобрать.
            local mark = SpawnObject("Path Mark " .. i)
            SetMeshCylinder(mark)
            mark.Transform.Position = Vec3(dx / len * 8.5, 0.7, dz / len * 8.5)
            mark.Transform.Scale = Vec3(0.22, 1.4, 0.22)
            mark.Color = PATH_COLORS[((i - 1) % #PATH_COLORS) + 1]
            Mat.Glow(mark, PATH_COLORS[((i - 1) % #PATH_COLORS) + 1], 2.4)
            mark:SetParent(root)
        end
    end
end

function H.Update(dt)
    time = time + dt
    -- Обелиск медленно поворачивается. Одно движущееся тело в центре пустой
    -- площадки — единственное, что отличает «демо работает» от «демо
    -- зависло», когда игрок стоит на месте.
    if obelisk and obelisk:Valid() then
        obelisk.Transform.Rotation.y = (time * 8.0) % 360.0
    end
end

-- Обелиск — твердь: в него упираются, а не проходят насквозь.
function H.Solid(minX, minY, minZ, maxX, maxY, maxZ)
    return minY < 6.0 and maxX > -0.4 and minX < 0.4 and maxZ > -0.4 and minZ < 0.4
end

return H
