-- ---------------------------------------------------------------------------
-- zone_material.lua — участок «Материалы»: сетка PBR и зеркальный пол.
--
-- СЕТКА СФЕР — стандартный способ показать физически корректное освещение, и
-- он стандартный не случайно: по строке видно, как металл переходит в
-- диэлектрик, по столбцу — как зеркало переходит в матовое. Всё остальное
-- (один красивый предмет, набор разных вещей) показывает результат, но не
-- ПРАВИЛО, по которому он получен.
--
-- Ряды — металличность 0..1, столбцы — шероховатость 0.05..1.
--
-- ЗЕРКАЛЬНЫЙ ПОЛ рядом с сеткой не для красоты: он показывает вторую половину
-- отражений — плоский проход, снимающий сцену зеркально. Карта окружения умеет
-- отражать небо и общую обстановку, но не умеет отражать конкретный предмет с
-- нужного места; плоскость умеет. Стоя между ними, разницу видно в упор.
-- ---------------------------------------------------------------------------
local Mat = require("mat")

local Z = {}

Z.name = "Материалы и отражения"
Z.about = "Сетка металл/шероховатость и зеркальная плита. E — сменить цвет"
Z.center = {x = 34.0, z = 0.0}

local COLS, ROWS = 7, 5
local STEP = 2.1

local root
local spheres = {}
local palette = {
    Vec3(0.94, 0.78, 0.36),   -- золото
    Vec3(0.86, 0.88, 0.92),   -- сталь
    Vec3(0.72, 0.40, 0.32),   -- медь
    Vec3(0.34, 0.52, 0.78),   -- синий лак
    Vec3(0.40, 0.66, 0.44),   -- зелёный лак
}
local tint = 1

function Z.Build()
    root = SpawnObject("Zone Material")
    SetMeshNone(root)
    local cx, cz = Z.center.x, Z.center.z

    -- Подиум: тёмная матовая плита. Она нужна, чтобы сетка не тонула в
    -- клетчатом полу — на клетке любой блик читается как продолжение узора.
    local base = SpawnObject("Material Base")
    SetMeshCube(base)
    base.Transform.Position = Vec3(cx, 0.1, cz)
    base.Transform.Scale = Vec3(COLS * STEP + 2.0, 0.2, ROWS * STEP + 2.0)
    base.Color = Vec3(0.12, 0.12, 0.14)
    Mat.Apply(base, 0.0, 0.85)
    base:SetParent(root)

    for r = 0, ROWS - 1 do
        for c = 0, COLS - 1 do
            local o = SpawnObject("PBR Sphere")
            SetMeshSphere(o)
            o.Transform.Position = Vec3(cx + (c - (COLS - 1) * 0.5) * STEP,
                                        1.15,
                                        cz + (r - (ROWS - 1) * 0.5) * STEP)
            o.Transform.Scale = Vec3(0.9, 0.9, 0.9)
            o.Color = palette[1]
            -- Не от нуля по шероховатости: идеальное зеркало без окружения
            -- выглядит чёрным шаром, и первый столбец читался бы как
            -- «материал сломан».
            Mat.Apply(o, r / (ROWS - 1), 0.05 + (c / (COLS - 1)) * 0.95)
            o:SetParent(root)
            spheres[#spheres + 1] = o
        end
    end

    -- Зеркальная плита. Плоское отражение в движке одно на сцену (второе
    -- потребовало бы второго прохода геометрии), поэтому плоскость ставится
    -- здесь — в единственном месте, где она по делу.
    local mirror = SpawnObject("Mirror")
    SetMeshCube(mirror)
    mirror.Transform.Position = Vec3(cx, 0.22, cz + ROWS * STEP * 0.5 + 5.0)
    mirror.Transform.Scale = Vec3(12.0, 0.12, 7.0)
    mirror.Color = Vec3(0.55, 0.60, 0.66)
    -- Доля отражения плоскости: единица даёт идеальное зеркало, которое на
    -- прототипном уровне выглядит дырой в полу. Чуть меньше — полированный
    -- камень, и в нём видно и отражение, и сам материал.
    Mat.Mirror(mirror, 1.0, 0.04, 0.85)
    mirror:SetParent(root)

    sage.reflect.SetEnabled(true)
    sage.reflect.SetPlanar(Vec3(0, 1, 0), Vec3(cx, 0.28, cz + ROWS * STEP * 0.5 + 5.0))
    sage.reflect.SetPlanarScale(0.6)
end

function Z.Use()
    tint = (tint % #palette) + 1
    for _, o in ipairs(spheres) do o.Color = palette[tint] end
    return "Цвет сетки сменён"
end

function Z.Solid() return false end

return Z
