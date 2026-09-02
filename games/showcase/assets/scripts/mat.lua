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

-- ---------------------------------------------------------------------------
-- Поверхность С УЗОРОМ: цвет и рельеф считаются процедурно (см.
-- docs/procedural_textures.md) и кладутся на один материал.
--
-- ЗАЧЕМ ЭТО ЗДЕСЬ, А НЕ В КАЖДОМ УЧАСТКЕ. Описание «доски: полосы поперёк,
-- рельеф по швам, повтор один к одному» — это шесть строк, и повторённые в
-- пяти участках они разъедутся: у ящика в одном месте будет пять досок, в
-- другом четыре, и одинаковые с виду вещи окажутся из разного дерева.
--
-- ВАЖНО ПРО ЦЕНУ. Текстурированный объект рисуется ОТДЕЛЬНЫМ вызовом — он не
-- попадает в инстансную группу (у инстансного пути нет слота под карты).
-- Поэтому узор дают тому, что стоит поштучно и на что смотрят вблизи: ящикам,
-- колоннам, подиумам. Тысяче одинаковых мелочей его давать нельзя.
--
--   opts: pattern, tiles/tilesX/tilesY, colorA, colorB, lineWidth, offset,
--         frequency, grain, seed, metallic, roughness, relief (сила рельефа),
--         uv/uvX/uvY (повтор по развёртке)
-- ---------------------------------------------------------------------------
function M.Textured(key, opts)
    local name = "showcase/tex/" .. key
    local m = sage.render.NewMaterial(name)
    local tilesX = opts.tilesX or opts.tiles or 4
    local tilesY = opts.tilesY or opts.tiles or 4

    m.Metallic = opts.metallic or 0.0
    m.Roughness = opts.roughness or 0.6
    m.TexturePath = sage.texture.Generate(name .. "_a", {
        pattern = opts.pattern or "noise",
        width = opts.width or 512,
        tilesX = tilesX, tilesY = tilesY,
        colorA = opts.colorA, colorB = opts.colorB,
        lineWidth = opts.lineWidth, offset = opts.offset,
        frequency = opts.frequency, grain = opts.grain or 0.06,
        seed = opts.seed or 1,
    })
    -- Рельеф необязателен: у крашеного металла его нет, и лишняя карта на нём
    -- дала бы рябь на ровной поверхности.
    if (opts.relief or 0) > 0 then
        m.NormalMapPath = sage.texture.Generate(name .. "_n", {
            pattern = opts.pattern or "noise",
            width = opts.width or 512,
            tilesX = tilesX, tilesY = tilesY,
            lineWidth = opts.lineWidth, offset = opts.offset,
            frequency = opts.frequency,
            normal = true, strength = opts.relief, seed = opts.seed or 1,
        })
    end
    m.Render.UVScaleX = opts.uvX or opts.uv or 1.0
    m.Render.UVScaleY = opts.uvY or opts.uv or 1.0
    sage.render.ResolveMaterialTextures(m)
    return name
end

-- Готовые поверхности демо. Отдельными именами, а не таблицей настроек в
-- каждом участке: «бетон» и «доски» должны выглядеть одинаково везде, где они
-- встречаются, иначе витрина рассыпается на разные по стилю куски.
function M.Concrete(obj, tint)
    local name = M.Textured("concrete", {
        -- Разброс тона узкий. Широкий даёт не бетон, а пятна грязи: шум с
        -- контрастом в полтона читается как «поверхность неровно освещена»,
        -- то есть как ошибка света, а не как материал.
        pattern = "noise", frequency = 6, octaves = 4, grain = 0.06,
        colorA = Vec3(0.74, 0.74, 0.76), colorB = Vec3(0.88, 0.88, 0.90),
        roughness = 0.75, relief = 0.5, seed = 5,
    })
    sage.render.SetMaterial(obj, name)
    if tint then obj.Color = tint end
    return name
end

-- Доски: полосы поперёк и глубокие швы между ними.
function M.Planks(obj, tint)
    local name = M.Textured("planks", {
        pattern = "grid", tilesX = 1, tilesY = 4, lineWidth = 0.08,
        colorA = Vec3(0.86, 0.72, 0.50), colorB = Vec3(0.35, 0.24, 0.14),
        grain = 0.16, roughness = 0.72, relief = 2.0, seed = 9,
    })
    sage.render.SetMaterial(obj, name)
    if tint then obj.Color = tint end
    return name
end

-- Шлифованный металл: цвет ровный, работает шероховатость.
function M.Brushed(obj, tint)
    local name = M.Textured("brushed", {
        -- У металла работает не цвет, а блик, поэтому разброс тона почти
        -- нулевой: заметный шум на металле выглядит не шлифовкой, а рябью.
        pattern = "noise", frequency = 16, octaves = 3, grain = 0.0,
        colorA = Vec3(0.86, 0.87, 0.89), colorB = Vec3(0.94, 0.95, 0.97),
        metallic = 0.9, roughness = 0.35, relief = 0.15, seed = 13,
    })
    sage.render.SetMaterial(obj, name)
    if tint then obj.Color = tint end
    return name
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
