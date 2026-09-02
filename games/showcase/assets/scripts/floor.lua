-- ---------------------------------------------------------------------------
-- floor.lua — платформа: клетчатый пол и бортик по краю.
--
-- ПОЧЕМУ ШАХМАТНЫЙ ПОЛ. У прототипного уровня клетка выполняет работу, которую
-- не делает никакая другая отделка: по ней ВИДНО МАСШТАБ. Клетка ровно два
-- метра, и, не глядя ни на какие цифры, понятно, какой высоты ящик, насколько
-- далеко участок и сколько шагов до него.
--
-- ПОЧЕМУ ТЕКСТУРА, А НЕ ТЫСЯЧИ ПЛИТОК-ОБЪЕКТОВ. Раньше пол здесь был сеткой
-- 53 x 53 из отдельных кубов — две с половиной тысячи сущностей. Инстансный
-- батчинг сводил их в пару вызовов отрисовки, так что кадр это переживал, но
-- платили за них все остальные системы: обход сцены, отсечение, иерархия,
-- сохранение, панель объектов в редакторе, где список из 2809 «Tile» невозможно
-- листать. Плата шла ЗА ВСЁ, а работал этот пол ровно как одна картинка.
--
-- Теперь пол — ОДНА плоскость с процедурной текстурой (см. render/TextureGen.h):
-- шахматка считается формулой при запуске, повтор задаётся материалом, и
-- клетка на дальнем краю платформы остаётся ровной благодаря мипмапам и
-- анизотропной фильтрации — чего сетка кубов не давала в принципе.
--
-- ТРИ КАРТЫ, А НЕ ОДНА. Цвет говорит, где светлая клетка, а где тёмная. Но
-- ровно закрашенный пол читается как бумага: у него нет ни шва между плитками,
-- ни разнотона. Поэтому к цвету идут карта нормалей (по той же клетке — швы
-- получают глубину) и карта шероховатости из шума (блик перестаёт быть
-- одинаковым на все сто метров). Все три считаются из одного описания, поэтому
-- разъехаться не могут.
-- ---------------------------------------------------------------------------
local Mat = require "mat"

local F = {}

F.TILE = 2.0          -- метра на клетку: по ней меряется всё остальное
F.HALF = 26           -- клеток от центра до края: платформа 104 x 104 м
F.TOP = 0.0           -- верх пола
local THICK = 0.35    -- толщина плиты под полом (нужна бортику и тени)

-- Два оттенка серого. Белый берём НЕ чистый: чисто белая клетка на солнце
-- уходит в пересвет, и тон-маппинг съедает на ней всю форму — клетка
-- превращается в белое поле.
local LIGHT = Vec3(0.82, 0.82, 0.84)
local DARK  = Vec3(0.17, 0.18, 0.21)

-- Клеток в самой текстуре. Не одна пара: чем больше клеток в картинке, тем
-- реже она повторяется на полу, а повтор — это то, что глаз замечает первым.
-- Четыре на четыре при повторе в 13 раз дают 52 клетки на платформу.
local TEX_CELLS = 4
local TEX_SIZE = 1024

local root
local slab

function F.Build()
    root = SpawnObject("Floor")
    SetMeshNone(root)

    -- --- Текстуры пола: цвет, рельеф, шероховатость ------------------------
    local albedo = sage.texture.Generate("showcase/floor_albedo", {
        pattern = "checker", width = TEX_SIZE,
        tilesX = TEX_CELLS, tilesY = TEX_CELLS,
        colorA = LIGHT, colorB = DARK,
        -- Зерно: слабый шум поверх заливки. Без него клетка выглядит
        -- напечатанной, а с ним — крашеным бетоном.
        grain = 0.05, seed = 7,
    })
    -- Рельеф по сетке швов: линии в тех же местах, где стыки клеток.
    local normal = sage.texture.Generate("showcase/floor_normal", {
        pattern = "grid", width = TEX_SIZE,
        tilesX = TEX_CELLS, tilesY = TEX_CELLS,
        lineWidth = 0.035, normal = true, strength = 1.6,
    })
    -- Разнотон шероховатости: шум множится на фактор материала, поэтому пол
    -- получается местами глаже, местами матовее — как настоящий крашеный пол,
    -- по которому ходят.
    local rough = sage.texture.Generate("showcase/floor_rough", {
        pattern = "noise", width = 512, frequency = 8, octaves = 4,
        colorA = Vec3(0.55, 0.55, 0.55), colorB = Vec3(1.0, 1.0, 1.0), seed = 21,
    })

    -- Материал пола намеренно ПОЛУГЛЯНЦЕВЫЙ. Матовый пол не отражает ничего, и
    -- половина работы освещения (небо, блики ламп, отражения) на нём просто не
    -- видна. Зеркальный, наоборот, превращает сцену в аттракцион.
    local m = sage.render.NewMaterial("showcase/floor")
    m.Metallic = 0.0
    m.Roughness = 0.55
    m.TexturePath = albedo
    m.NormalMapPath = normal
    m.RoughnessMapPath = rough
    -- Повтор считается ИЗ РАЗМЕРА платформы и размера клетки, а не подбирается
    -- числом: подобранное число разъедется с полом при первой же правке
    -- F.HALF, и клетка перестанет быть ровно двухметровой — то есть перестанет
    -- работать как мерка.
    local span = (F.HALF * 2 + 1) * F.TILE
    m.Render.UVScaleX = span / F.TILE / TEX_CELLS
    m.Render.UVScaleY = m.Render.UVScaleX
    sage.render.ResolveMaterialTextures(m)

    slab = SpawnObject("Floor Slab")
    SetMeshPlane(slab)
    slab.Transform.Position = Vec3(0, F.TOP, 0)
    slab.Transform.Scale = Vec3(span, 1.0, span)
    sage.render.SetMaterial(slab, "showcase/floor")
    slab:SetParent(root)

    -- Плита под полом. Плоскость бесконечно тонкая, и платформа с торца
    -- выглядела бы листом бумаги; тень от бортика тоже ложится на неё.
    local body = SpawnObject("Floor Body")
    SetMeshCube(body)
    body.Transform.Position = Vec3(0, F.TOP - THICK * 0.5 - 0.01, 0)
    body.Transform.Scale = Vec3(span, THICK, span)
    body.Color = Vec3(0.12, 0.13, 0.15)
    Mat.Apply(body, 0.0, 0.8)
    body:SetParent(root)

    F.Curb(span)
    return 1
end

-- Бортик по краю платформы: и ограждение, и рама кадра. Без него платформа
-- обрывается в небо, и сцена выглядит незаконченной с любой точки.
function F.Curb(span)
    local edge = (F.HALF + 0.5) * F.TILE

    -- Кладка на бортике: узкая полоса, и на ней рисунок читается вблизи, когда
    -- игрок подходит к краю. Швы — те же линии, что и на полу, поэтому бортик
    -- выглядит частью платформы, а не приставленным брусом.
    local wall = sage.texture.Generate("showcase/curb_albedo", {
        pattern = "bricks", width = 512, tilesX = 8, tilesY = 4,
        lineWidth = 0.06, offset = 0.5,
        colorA = Vec3(0.22, 0.23, 0.26), colorB = Vec3(0.10, 0.10, 0.12),
        grain = 0.12, seed = 3,
    })
    local wallN = sage.texture.Generate("showcase/curb_normal", {
        pattern = "bricks", width = 512, tilesX = 8, tilesY = 4,
        lineWidth = 0.06, offset = 0.5, normal = true, strength = 2.2,
    })
    local cm = sage.render.NewMaterial("showcase/curb")
    cm.Metallic = 0.1
    cm.Roughness = 0.65
    cm.TexturePath = wall
    cm.NormalMapPath = wallN
    cm.Render.UVScaleX = 26.0
    cm.Render.UVScaleY = 1.0
    sage.render.ResolveMaterialTextures(cm)

    local sides = {
        {Vec3(0, 0.35, edge),  Vec3(span, 0.7, 0.5)},
        {Vec3(0, 0.35, -edge), Vec3(span, 0.7, 0.5)},
        {Vec3(edge, 0.35, 0),  Vec3(0.5, 0.7, span)},
        {Vec3(-edge, 0.35, 0), Vec3(0.5, 0.7, span)},
    }
    for i, s in ipairs(sides) do
        local o = SpawnObject("Curb " .. i)
        SetMeshCube(o)
        o.Transform.Position = s[1]
        o.Transform.Scale = s[2]
        sage.render.SetMaterial(o, "showcase/curb")
        o:SetParent(root)
    end
end

-- Стоим ли мы на платформе. Пол — не физическое тело: игрок ходит по
-- контроллеру движка с простым запросом «твердо ли здесь» (см. player.lua).
-- С полом-плоскостью это стало ещё и единственным способом: у плоскости нет
-- толщины, и опираться на неё физически не на что.
function F.Solid(minX, minY, minZ, maxX, maxY, maxZ)
    local edge = (F.HALF + 0.5) * F.TILE
    -- Пол: всё, что ниже нуля и в пределах платформы.
    if minY < F.TOP and maxY > F.TOP - THICK
       and maxX > -edge and minX < edge and maxZ > -edge and minZ < edge then
        return true
    end
    -- Бортик: рамка шириной полметра по периметру.
    if maxY > 0.0 and minY < 0.7 then
        local inner = edge - 0.5
        local outer = edge + 0.5
        local outsideInner = maxX > inner or minX < -inner or maxZ > inner or minZ < -inner
        local insideOuter = minX < outer and maxX > -outer and minZ < outer and maxZ > -outer
        if outsideInner and insideOuter then return true end
    end
    return false
end

function F.Slab() return slab end

return F
