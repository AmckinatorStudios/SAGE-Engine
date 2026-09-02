-- ---------------------------------------------------------------------------
-- mat.lua — библиотека поверхностей демо.
--
-- ПОЧЕМУ НЕ «МАТЕРИАЛ НА КАЖДЫЙ ОБЪЕКТ». Металличность и шероховатость — это
-- свойства ПОВЕРХНОСТИ, а поверхность в движке одна на всех, кто ей покрашен.
-- Дай каждому объекту свой материал — и две с половиной тысячи плиток пола
-- станут двумя с половиной тысячами инстансных групп, то есть отрисовка
-- вернётся к «один вызов на объект», ради ухода от которого батчинг и написан.
--
-- Поэтому здесь материалы заводятся ПО ЗНАЧЕНИЯМ: запрос (metallic, roughness)
-- округляется до сотых и отдаёт один и тот же материал всем, кто просил то же
-- самое. Сетка из тридцати пяти шаров получает тридцать пять материалов —
-- столько разных поверхностей в ней и есть, — а тысячи плиток пола две.
--
-- ЦВЕТ СЮДА НЕ ВХОДИТ намеренно: albedo материала остаётся белым, а тон задаёт
-- Color объекта (движок их перемножает — см. EffectiveColor). Цвет едет
-- per-instance атрибутом и группу не разбивает, поэтому красные и синие ящики
-- с одной поверхностью рисуются одним вызовом.
-- ---------------------------------------------------------------------------
local M = {}

local function key(metallic, roughness)
    return string.format("showcase/s_%02d_%02d",
                         math.floor(metallic * 100 + 0.5),
                         math.floor(roughness * 100 + 0.5))
end

-- Поверхность по паре чисел. Возвращает ИМЯ материала — его ждёт SetMaterial.
function M.Surface(metallic, roughness)
    local name = key(metallic, roughness)
    local m = sage.render.NewMaterial(name)
    m.Metallic = metallic
    m.Roughness = roughness
    return name
end

-- То же и сразу назначить объекту.
function M.Apply(obj, metallic, roughness)
    sage.render.SetMaterial(obj, M.Surface(metallic, roughness))
end

-- Зеркальная поверхность, участвующая в ПЛОСКОМ отражении сцены. Отдельно от
-- Surface, потому что это свойство не поверхности, а её отношения к проходу
-- отражения: гладкий металлический шар тоже гладкий, но плоскости не
-- принадлежит, и отражение плоскости на нём было бы неправдой.
function M.Mirror(obj, metallic, roughness, reflectivity)
    local name = string.format("showcase/mirror_%02d",
                               math.floor(reflectivity * 100 + 0.5))
    local m = sage.render.NewMaterial(name)
    m.Metallic = metallic
    m.Roughness = roughness
    m.Render.PlanarReflectivity = reflectivity
    sage.render.SetMaterial(obj, name)
    return name
end

-- Светящаяся поверхность: свечение задаётся здесь, а не полями рендерера,
-- чтобы у ламп одного вида был один материал и одна группа отрисовки.
function M.Glow(obj, color, strength)
    local name = string.format("showcase/glow_%02d_%02d_%02d_%02d",
                               math.floor(color.x * 99), math.floor(color.y * 99),
                               math.floor(color.z * 99), math.floor(strength * 10))
    local m = sage.render.NewMaterial(name)
    m.Roughness = 0.6
    m.Emissive = color
    m.EmissiveStrength = strength
    sage.render.SetMaterial(obj, name)
    return name
end

return M
