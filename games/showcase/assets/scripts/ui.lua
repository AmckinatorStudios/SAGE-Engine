-- ---------------------------------------------------------------------------
-- ui.lua — кирпичики экрана демо.
--
-- Стиль плоский: заливка, волосяная рамка, цвет. Ни теней, ни градиентов.
-- Причина та же, что и в «Лодке»: интерфейс лежит поверх ЖИВОЙ картинки —
-- клетчатый пол, блики, частицы, — и мягкая тень под панелью на такой подложке
-- читается как грязь. Здесь к этому добавляется своё: демо смотрят на чужом
-- экране и часто в проекторе, где полутон теряется первым.
--
-- Всё, что видно на экране, обязано отвечать на вопрос «что я могу сделать
-- прямо сейчас». Числа ради чисел — счётчики, координаты, версии — на экран не
-- выводятся: они не помогают ни ходить, ни нажимать.
-- ---------------------------------------------------------------------------
local U = {}

U.INK     = {0.05, 0.06, 0.08}   -- подложка
U.SURFACE = {0.10, 0.12, 0.15}
U.LINE    = {0.30, 0.34, 0.40}
U.ACCENT  = {0.45, 0.78, 0.98}   -- холодный акцент: демо про рендер, не про закат
U.TEXT    = {0.96, 0.96, 0.95}
U.MUTED   = {0.62, 0.67, 0.73}
U.GOOD    = {0.55, 0.84, 0.58}

U.ROUND = 2.0

function U.C(rgb, a) return Vec4(rgb[1], rgb[2], rgb[3], a or 1.0) end

-- Корень экрана: холст во весь кадр. Порядок (order) разводит слои — худ под
-- справкой, справка под меню.
function U.Screen(name, order)
    local obj = SpawnObject(name)
    SetMeshNone(obj)
    local e = obj:AddUI()
    e.Anchor = UIAnchor.TopLeft
    e.Offset = Vec2(0, 0)
    e.Stretch = UIStretch.Both
    e.Color = Vec4(0, 0, 0, 0)
    sage.ui.SetCanvas(obj, {order = order or 0})
    return obj, e
end

local function base(parent, name, anchor, x, y, w, h)
    local obj = SpawnObject(name)
    SetMeshNone(obj)
    local e = obj:AddUI()
    e.Anchor = anchor or UIAnchor.TopLeft
    e.Offset = Vec2(x, y)
    e.Size = Vec2(w, h)
    if parent then obj:SetParent(parent) end
    return obj, e
end

function U.Panel(parent, name, anchor, x, y, w, h)
    local obj, e = base(parent, name, anchor, x, y, w, h)
    e.Type = UIKind.Panel
    e.Color = Vec4(0, 0, 0, 0)
    e.Rounding = U.ROUND
    e.TextCentered = false
    e.TextColor = U.C(U.TEXT)
    return obj, e
end

function U.Card(parent, name, anchor, x, y, w, h)
    local obj, e = U.Panel(parent, name, anchor, x, y, w, h)
    e.Color = U.C(U.INK, 0.88)
    e.BorderThickness = 1.0
    e.BorderColor = U.C(U.LINE, 0.8)
    return obj, e
end

function U.Label(parent, name, anchor, x, y, w, h, text, scale)
    local obj, e = base(parent, name, anchor, x, y, w, h)
    e.Type = UIKind.Label
    e.Text = text or ""
    e.TextScale = scale or 1.2
    e.TextCentered = false
    e.TextColor = U.C(U.TEXT)
    return obj, e
end

function U.Icon(parent, name, anchor, x, y, size, icon, color)
    local obj, e = base(parent, name, anchor, x, y, size, size)
    e.Type = UIKind.Icon
    e.Icon = icon
    e.IconColor = color or U.C(U.TEXT)
    return obj, e
end

-- Полоса: показывает долю. В демо ею меряется не здоровье, а то, что
-- действительно меняется на глазах, — заряд фонаря, прогресс замера.
function U.Bar(parent, name, anchor, x, y, w, h, color)
    local obj, e = base(parent, name, anchor, x, y, w, h)
    e.Type = UIKind.Bar
    e.Color = U.C(U.INK, 0.85)
    e.BarFillColor = color or U.C(U.ACCENT)
    e.Rounding = 1.0
    e.MinValue = 0.0
    e.MaxValue = 1.0
    e.Value = 1.0
    return obj, e
end

-- Строка «клавиша — что делает». Из таких строк собрана вся справка, и делать
-- их вручную значило бы каждый раз заново подбирать ширину колонки под клавишу.
function U.KeyRow(parent, name, anchor, x, y, w, key, what)
    local rowObj = SpawnObject(name)
    SetMeshNone(rowObj)
    local re = rowObj:AddUI()
    re.Anchor = anchor
    re.Offset = Vec2(x, y)
    re.Size = Vec2(w, 20)
    re.Color = Vec4(0, 0, 0, 0)
    if parent then rowObj:SetParent(parent) end

    local _, capE = U.Panel(rowObj, name .. " Key", UIAnchor.CenterLeft, 24, 0, 46, 18)
    capE.Color = U.C(U.SURFACE, 0.95)
    capE.BorderThickness = 1.0
    capE.BorderColor = U.C(U.LINE, 0.7)
    capE.Text = key
    capE.TextCentered = true
    capE.TextScale = 1.05
    capE.TextColor = U.C(U.ACCENT)

    local _, txtE = U.Label(rowObj, name .. " Text", UIAnchor.CenterLeft, 78, 0, w - 84, 18,
                            what, 1.1)
    txtE.TextColor = U.C(U.TEXT)
    return rowObj
end

return U
