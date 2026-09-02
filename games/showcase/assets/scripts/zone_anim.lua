-- ---------------------------------------------------------------------------
-- zone_anim.lua — участок «Анимация и движение».
--
-- Здесь три разных способа что-то шевелить, и они НЕ взаимозаменяемы:
--
--   СКЕЛЕТНАЯ АНИМАЦИЯ. Модель со скином и клипами: вершины гнутся вслед за
--   костями на GPU. Так двигают всё живое. Проверяется тем, что клипы
--   ПЕРЕКЛЮЧАЮТСЯ с переходом — резкая смена позы видна сразу и означает, что
--   смешивание не работает.
--
--   ОБРАТНАЯ КИНЕМАТИКА. Кость тянут за конец, а поворот суставов считает
--   движок. Тут это «фигура тянется за тобой»: цель IK — сам игрок, и по тому,
--   как модель доворачивается, видно и предел угла, и плавность веса.
--
--   ИНТЕРПОЛЯЦИЯ (tween). Никакого скелета: значение едет из A в B по кривой.
--   Так двигают двери, лифты, интерфейс. Пять плит с разными кривыми стоят
--   рядом специально — по одной кривой не видно, что кривая вообще есть.
--
-- ПОЧЕМУ ПЛИТЫ, А НЕ ПОДПИСИ. Названия кривых («BackOut», «ElasticOut»)
-- человеку ничего не говорят, а пять плит, стартующих одновременно и приходящих
-- по-разному, объясняют разницу за один прогон.
-- ---------------------------------------------------------------------------
local Mat = require "mat"

local Z = {}

Z.name = "Анимация и движение"
Z.about = "Скелет, обратная кинематика и кривые интерполяции. E — сменить клип"
Z.center = {x = 26.0, z = 26.0}

local MODEL = "assets/models/figure.glb"

local root
local figure                 -- модель со скином
local aimGoal = -1
local clip = 0
local clipCount = 0
local ikOn = true
local plates = {}
local platesUp = false
local time = 0.0

local EASES = {
    {Ease.Linear,     "линейно"},
    {Ease.QuadInOut,  "квадратично"},
    {Ease.ExpoOut,    "экспонентой"},
    {Ease.BackOut,    "с перелётом"},
    {Ease.BounceOut,  "с отскоком"},
}

function Z.Build()
    root = SpawnObject("Zone Anim")
    SetMeshNone(root)
    local cx, cz = Z.center.x, Z.center.z

    -- Подиум: тёмный, чтобы светлая модель на нём читалась.
    local base = SpawnObject("Anim Base")
    SetMeshCube(base)
    base.Transform.Position = Vec3(cx, 0.15, cz)
    base.Transform.Scale = Vec3(16.0, 0.3, 12.0)
    Mat.Concrete(base, Vec3(0.30, 0.31, 0.35))
    base:SetParent(root)

    -- Модель со скином. Она же проверяет загрузчик glTF: скин, кости, клипы и
    -- встроенная текстура приходят одним файлом.
    figure = SpawnObject("Figure")
    figure.Transform.Position = Vec3(cx - 4.0, 0.3, cz)
    figure.Transform.Scale = Vec3(1.6, 1.6, 1.6)
    sage.anim.Add(figure, MODEL)
    figure:SetParent(root)

    -- Вторая фигура — БЕЗ анимации, в позе покоя. Рядом с движущейся она
    -- отвечает на вопрос «а точно ли гнётся скелет, а не крутится вся модель».
    local still = SpawnObject("Figure Still")
    still.Transform.Position = Vec3(cx - 7.0, 0.3, cz + 2.5)
    still.Transform.Scale = Vec3(1.6, 1.6, 1.6)
    sage.anim.Add(still, MODEL)
    sage.anim.SetPlaying(still, false)
    still:SetParent(root)

    -- Плиты интерполяции.
    for i, e in ipairs(EASES) do
        local o = SpawnObject("Ease " .. i)
        SetMeshCube(o)
        local x = cx + 1.0 + (i - 1) * 1.8
        o.Transform.Position = Vec3(x, 0.5, cz - 3.0)
        o.Transform.Scale = Vec3(1.2, 0.4, 1.2)
        o.Color = Vec3(0.35 + i * 0.1, 0.55, 0.85 - i * 0.08)
        Mat.Apply(o, 0.15, 0.4)
        o:SetParent(root)
        plates[#plates + 1] = {obj = o, ease = e[1], name = e[2], x = x, z = cz - 3.0}
    end

    -- Столбики-указатели у плит: без них ряд читается как декорация.
    for i = 1, #EASES do
        local p = SpawnObject("Ease Post " .. i)
        SetMeshCube(p)
        p.Transform.Position = Vec3(cx + 1.0 + (i - 1) * 1.8, 0.2, cz - 4.6)
        p.Transform.Scale = Vec3(0.12, 0.4, 0.12)
        p.Color = Vec3(0.5, 0.52, 0.56)
        Mat.Apply(p, 0.6, 0.4)
        p:SetParent(root)
    end
end

-- Готовность модели проверяется НЕ в Build: скелет и клипы грузятся движком на
-- первом кадре после того, как компонент появился (см. AnimatedModelComponent.
-- Ready), и спрашивать число клипов сразу — значит всегда получать ноль.
local function ensureRig()
    if clipCount > 0 or figure == nil or not figure:Valid() then return end
    clipCount = sage.anim.Count(figure)
    if clipCount <= 0 then return end
    sage.anim.Play(figure, 0, 0.25)
    sage.anim.SetLoop(figure, true)
    -- Цель IK на последней кости цепочки: за неё модель и тянется.
    local names = sage.anim.JointNames(figure)
    if #names > 0 then
        aimGoal = sage.ik.AddGoal(figure, names[#names], 3)
        sage.ik.SetWeight(figure, aimGoal, 0.85)
        sage.ik.SetEnabled(figure, true)
    end
end

function Z.Update(dt, ctx)
    time = time + dt
    ensureRig()

    -- IK тянется за игроком. Цель берётся на уровне груди, а не глаз: иначе
    -- вблизи фигура задирает голову в потолок.
    if ikOn and aimGoal >= 0 and ctx and ctx.playerPos then
        sage.ik.SetTarget(figure, aimGoal,
                          Vec3(ctx.playerPos.x, ctx.playerPos.y + 1.1, ctx.playerPos.z))
    end
end

-- E переключает клип. R (Reset) гоняет плиты.
function Z.Use()
    ensureRig()
    if clipCount <= 0 then return "Модель ещё грузится" end
    clip = (clip + 1) % clipCount
    -- Переход в четверть секунды: с нулём поза скачет, с секундой движение
    -- выглядит вязким.
    sage.anim.Play(figure, clip, 0.25)
    local names = sage.anim.Names(figure)
    return "Клип: " .. (names[clip + 1] or tostring(clip + 1))
end

function Z.Reset()
    platesUp = not platesUp
    local dy = platesUp and 2.6 or 0.5
    for _, p in ipairs(plates) do
        sage.tween.Move(p.obj, Vec3(p.x, dy, p.z), 1.4, p.ease)
    end
    return platesUp and "Плиты пошли вверх — кривые видно по приходу"
                     or "Плиты возвращаются"
end

function Z.Solid() return false end

return Z
