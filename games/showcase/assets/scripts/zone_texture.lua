-- ---------------------------------------------------------------------------
-- zone_texture.lua — участок «Процедурные текстуры».
--
-- Здесь показано то, из чего сделан пол под ногами: картинки, которых нет на
-- диске. Каждая посчитана формулой при запуске (см. render/TextureGen.h), и
-- это не экономия ради экономии — это три разных выигрыша сразу:
--
--   РАЗМЕР. Шахматка 1024x1024 в файле — сотни килобайт; здесь она весит
--   одну строчку описания и считается за миллисекунды.
--
--   ПРАВКА. Размер клетки, цвет, толщина шва — числа в скрипте. Чтобы сделать
--   клетку крупнее, не нужно ни художника, ни редактора изображений.
--
--   РЕЛЬЕФ ИЗ ТОГО ЖЕ ОПИСАНИЯ. Карта нормалей считается по той же формуле,
--   что и цвет, — значит, шов на рельефе ВСЕГДА там же, где шов на картинке.
--   Пара «цвет + нормаль», нарисованная руками, расходится на первой же правке.
--
-- СТЕНД ИЗ ПАР. Каждый узор показан дважды: слева плита с цветом, справа — та
-- же плита с рельефом по тому же узору. Разницу между «нарисовано» и «выпукло»
-- на одной плите увидеть нельзя, на паре — сразу. Плиты пары стоят ВПЛОТНУЮ и
-- в один ряд: разнесённые по глубине, они закрывали бы друг друга, а
-- разнесённые далеко по ширине — были бы освещены заметно по-разному, и
-- разницу рельефа не отличить от разницы света.
--
-- E пересчитывает весь стенд с новым зерном и другим числом клеток: главное
-- свойство процедурной текстуры — что она БЫВАЕТ ДРУГОЙ, и показывать его надо
-- сменой на глазах, а не словами.
-- ---------------------------------------------------------------------------
local Mat = require "mat"

local Z = {}

Z.name = "Процедурные текстуры"
Z.about = "Узоры, посчитанные формулой, и рельеф из них же. E — пересчитать"
Z.center = {x = -26.0, z = -26.0}

local root
local panels = {}          -- {объект, имя материала, узор, роль}
local seed = 11
local density = 1.0

-- Шесть узоров: по одному на каждый вид, который умеет генератор. Порядок —
-- от самого «технического» к самому «природному».
local PATTERNS = {
    {id = "checker",  name = "шахматка", a = Vec3(0.86, 0.86, 0.88), b = Vec3(0.24, 0.25, 0.28), tiles = 6},
    {id = "grid",     name = "сетка",    a = Vec3(0.16, 0.18, 0.22), b = Vec3(0.55, 0.85, 1.00), tiles = 8},
    {id = "bricks",   name = "кладка",   a = Vec3(0.58, 0.30, 0.24), b = Vec3(0.16, 0.13, 0.12), tiles = 6},
    {id = "dots",     name = "заклёпки", a = Vec3(0.30, 0.32, 0.36), b = Vec3(0.80, 0.78, 0.70), tiles = 8},
    {id = "gradient", name = "градиент", a = Vec3(0.10, 0.20, 0.45), b = Vec3(0.95, 0.60, 0.30), tiles = 1},
    {id = "noise",    name = "шум",      a = Vec3(0.20, 0.22, 0.20), b = Vec3(0.75, 0.72, 0.62), tiles = 1},
}

local PANEL_W, PANEL_H = 1.5, 3.0
local PAIR_GAP = 0.12        -- зазор внутри пары
local PAIR_PITCH = 3.4       -- расстояние между парами

-- Одна плита стенда. Роль "color" — узор цветом, "relief" — тот же узор
-- рельефом поверх ровной заливки: так видно ИМЕННО вклад карты нормалей,
-- не смешанный с цветом.
local function panelMaterial(p, role)
    local key = string.format("showcase/tex_%s_%s", p.id, role)
    local m = sage.render.NewMaterial(key)
    m.Metallic = 0.0
    m.Roughness = 0.55
    local tiles = math.max(1, math.floor(p.tiles * density + 0.5))

    if role == "color" then
        m.TexturePath = sage.texture.Generate(key .. "_a", {
            pattern = p.id, width = 512, tilesX = tiles, tilesY = tiles,
            colorA = p.a, colorB = p.b, lineWidth = 0.07,
            frequency = 6, octaves = 4, grain = 0.04, seed = seed,
        })
        m.NormalMapPath = ""
    else
        -- Ровный светлый цвет и весь рисунок — в рельефе.
        m.TexturePath = ""
        m.Albedo = Vec3(0.72, 0.72, 0.74)
        m.NormalMapPath = sage.texture.Generate(key .. "_n", {
            pattern = p.id, width = 512, tilesX = tiles, tilesY = tiles,
            lineWidth = 0.07, frequency = 6, octaves = 4,
            normal = true, strength = 2.4, seed = seed,
        })
    end
    sage.render.ResolveMaterialTextures(m)
    return key
end

-- Середина i-й пары. Одна формула на постановку и на твердь: разъехавшись,
-- они дали бы невидимую стену рядом с плитой.
function Z.PairX(i)
    return Z.center.x - (#PATTERNS - 1) * PAIR_PITCH * 0.5 + (i - 1) * PAIR_PITCH
end

function Z.Build()
    root = SpawnObject("Zone Texture")
    SetMeshNone(root)
    local cx, cz = Z.center.x, Z.center.z

    local base = SpawnObject("Texture Base")
    SetMeshCube(base)
    base.Transform.Position = Vec3(cx, 0.15, cz)
    base.Transform.Scale = Vec3(#PATTERNS * PAIR_PITCH + 2.0, 0.3, 5.0)
    Mat.Concrete(base, Vec3(0.26, 0.27, 0.31))
    base:SetParent(root)

    for i, p in ipairs(PATTERNS) do
        local x = Z.PairX(i)
        for j, role in ipairs({"color", "relief"}) do
            local o = SpawnObject("Panel " .. p.name .. " " .. role)
            SetMeshCube(o)
            o.Transform.Position = Vec3(x + (j - 1.5) * (PANEL_W + PAIR_GAP),
                                        0.3 + PANEL_H * 0.5, cz)
            o.Transform.Scale = Vec3(PANEL_W, PANEL_H, 0.18)
            sage.render.SetMaterial(o, panelMaterial(p, role))
            o:SetParent(root)
            panels[#panels + 1] = {obj = o, pattern = p, role = role}
        end

        -- Разделитель между парами: без него двенадцать плит читаются одной
        -- стеной, и какая с какой в паре — непонятно.
        local post = SpawnObject("Panel Post " .. i)
        SetMeshCube(post)
        post.Transform.Position = Vec3(x + PAIR_PITCH * 0.5, 1.9, cz)
        post.Transform.Scale = Vec3(0.16, 3.8, 0.5)
        post.Color = Vec3(0.3, 0.31, 0.35)
        Mat.Apply(post, 0.7, 0.4)
        post:SetParent(root)
    end
end

-- E: другое зерно и другая плотность узора. Пересчитываются ТЕ ЖЕ материалы по
-- тем же именам — объекты не трогаются вовсе, меняется только картинка внутри.
function Z.Use()
    seed = seed + 7
    density = density * 1.5
    if density > 3.1 then density = 0.5 end
    for _, p in ipairs(panels) do
        panelMaterial(p.pattern, p.role)
    end
    return string.format("Стенд пересчитан: плотность x%.1f, зерно %d", density, seed)
end

function Z.Reset()
    seed = 11
    density = 1.0
    for _, p in ipairs(panels) do panelMaterial(p.pattern, p.role) end
    return "Стенд возвращён к исходному виду"
end

-- Плиты — твердь: между ними ходят, и проваливаться сквозь стенд не должно.
function Z.Solid(minX, minY, minZ, maxX, maxY, maxZ)
    local cz = Z.center.z
    if minY > 0.3 + PANEL_H then return false end
    if maxZ <= cz - 0.15 or minZ >= cz + 0.15 then return false end
    local halfPair = PANEL_W + PAIR_GAP * 0.5
    for i = 1, #PATTERNS do
        local x = Z.PairX(i)
        if maxX > x - halfPair and minX < x + halfPair then return true end
    end
    return false
end

return Z
