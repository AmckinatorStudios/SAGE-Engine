-- ---------------------------------------------------------------------------
-- hud.lua — то, что видно поверх демо.
--
-- Экран демо решает ровно одну задачу: человек, впервые надевший управление,
-- должен в любой момент знать, ГДЕ он и ЧТО тут можно нажать. Всё остальное —
-- лишнее, потому что смотреть пришли на сцену, а не на интерфейс.
--
-- Отсюда состав: прицел, название участка с одной строкой пояснения, строка
-- действия под прицелом, счётчик кадра и справка по F1. Числа, которые ничего
-- не меняют (координаты, число объектов, версия), на экран не выводятся.
--
-- СЧЁТЧИК КАДРА ЗДЕСЬ ПО ДЕЛУ, а не для красоты: это демо ДВИЖКА, и первый
-- вопрос к нему — «сколько это стоит». Показаны миллисекунды, а не только
-- кадры в секунду: 60 и 58 fps выглядят одинаково, 16.7 и 17.2 мс — нет.
-- ---------------------------------------------------------------------------
local U = require "ui"

local H = {}

local root, help
local els = {}
local messageTimer = 0.0
local frameMs = 16.0        -- сглаженная длительность кадра
local titleFade = 0.0

local function keep(name, obj) els[name] = obj end
local function ui(name)
    local o = els[name]
    if o == nil or not o:Valid() then return nil end
    return o:GetUI()
end

-- Клавиши демо. Один список: он же показывается в справке, он же служит
-- напоминанием при чтении world.lua, где эти действия объявлены.
local KEYS = {
    {"WASD", "идти"},
    {"SHIFT", "бежать"},
    {"SPACE", "прыжок"},
    {"E", "действие участка"},
    {"R", "собрать участок заново"},
    {"L", "фонарь"},
    {"V", "разбор кадра (нормали, тени, каскады…)"},
    {"B", "объёмный свет и облака"},
    {"N", "блик в объективе"},
    {"F1", "эта справка"},
    {"ESC", "меню"},
}

function H.Build(zones)
    root = U.Screen("HUD", 0)

    -- Прицел: четыре штриха вокруг пустого центра. Точка в середине закрывала
    -- бы ровно то, на что смотришь.
    for i, m in ipairs({{0, -8, 2, 6}, {0, 8, 2, 6}, {-8, 0, 6, 2}, {8, 0, 6, 2}}) do
        local _, e = U.Panel(root, "Aim " .. i, UIAnchor.Center, m[1], m[2], m[3], m[4])
        e.Color = Vec4(1, 1, 1, 0.55)
        e.Rounding = 0.0
    end

    -- Название участка. Без карточки под ним: крупная надпись на своём фоне
    -- читается и так, а панель во весь верх экрана закрыла бы небо, ради
    -- которого половина участков и сделана.
    local titleObj, title = U.Label(root, "Zone Title", UIAnchor.TopCenter, 0, 26, 620, 26,
                                    "", 1.9)
    title.TextCentered = true
    title.TextColor = U.C(U.TEXT, 0.0)
    keep("Zone Title", titleObj)

    local aboutObj, about = U.Label(root, "Zone About", UIAnchor.TopCenter, 0, 54, 620, 20,
                                    "", 1.15)
    about.TextCentered = true
    about.TextColor = U.C(U.MUTED, 0.0)
    keep("Zone About", aboutObj)

    -- Строка действия — под прицелом, там же, где взгляд. Пустая, если на
    -- участке нажимать нечего: подсказка «нечего делать» хуже, чем её
    -- отсутствие.
    local hintObj, hint = U.Label(root, "Hint", UIAnchor.Center, 0, 44, 560, 20, "", 1.2)
    hint.TextCentered = true
    hint.TextColor = U.C(U.ACCENT, 0.9)
    keep("Hint", hintObj)

    -- Сообщение о том, что только что произошло. Гаснет само.
    local msgObj, msg = U.Label(root, "Message", UIAnchor.BottomCenter, 0, 96, 620, 20, "", 1.25)
    msg.TextCentered = true
    msg.TextColor = U.C(U.TEXT, 0.0)
    keep("Message", msgObj)

    -- Счётчик кадра.
    -- Смещение у правого якоря отсчитывается ВЛЕВО от края (см. UIAnchor.h),
    -- поэтому оно положительное; текст в своей коробке выравнивается по
    -- центру, иначе он ушёл бы к её левому краю, то есть на середину экрана.
    local perfObj, perf = U.Label(root, "Perf", UIAnchor.TopRight, 20, 22, 200, 18, "", 1.1)
    perf.TextCentered = true
    perf.TextColor = U.C(U.MUTED)
    keep("Perf", perfObj)

    -- Состояние переключателей рендера — внизу слева, мелко. Нужно ровно
    -- затем, чтобы после нажатия V человек видел, ЧТО именно он включил:
    -- «нормали» и «шероховатость» на глаз путаются.
    local modeObj, mode = U.Label(root, "Modes", UIAnchor.BottomLeft, 20, 20, 560, 18, "", 1.1)
    mode.TextColor = U.C(U.MUTED)
    keep("Modes", modeObj)

    -- Подсказка про справку. Одна строка, и она себя окупает: без неё F1
    -- никто не нажимает.
    local tipObj, tip = U.Label(root, "F1 Tip", UIAnchor.BottomRight, 20, 20, 240, 18,
                                "F1 — справка", 1.1)
    tip.TextCentered = true
    tip.TextColor = U.C(U.MUTED, 0.8)
    keep("F1 Tip", tipObj)

    H.BuildHelp(zones)
    return root
end

-- Справка. Отдельный холст поверх худа: спрятать её — одно поле на корне.
function H.BuildHelp(zones)
    help = U.Screen("Help", 10)
    local _, dim = U.Panel(help, "Help Dim", UIAnchor.TopLeft, 0, 0, 10, 10)
    dim.Stretch = UIStretch.Both
    dim.Color = Vec4(0.02, 0.03, 0.04, 0.72)
    dim.Rounding = 0.0

    local cardW, cardH = 760, 460
    local card = U.Card(help, "Help Card", UIAnchor.Center, 0, 0, cardW, cardH)

    local _, h1 = U.Label(card, "Help Title", UIAnchor.TopLeft, 28, 24, 460, 26,
                          "SAGE Engine — витрина возможностей", 1.7)
    h1.TextColor = U.C(U.TEXT)
    local _, h2 = U.Label(card, "Help Sub", UIAnchor.TopLeft, 28, 54, 620, 20,
                          "Платформа с участками. Дойди до участка — он расскажет о себе сам.",
                          1.15)
    h2.TextColor = U.C(U.MUTED)

    -- Левая колонка — клавиши, правая — участки. Две колонки, а не один
    -- список: клавиш ровно столько, чтобы уместиться в высоту карточки, и
    -- смешанный список пришлось бы листать.
    local _, kc = U.Label(card, "Help Keys Cap", UIAnchor.TopLeft, 28, 96, 300, 18,
                          "УПРАВЛЕНИЕ", 1.1)
    kc.TextColor = U.C(U.ACCENT)
    for i, k in ipairs(KEYS) do
        U.KeyRow(card, "Help Key " .. i, UIAnchor.TopLeft, 4, 120 + (i - 1) * 24, 340, k[1], k[2])
    end

    local _, zc = U.Label(card, "Help Zones Cap", UIAnchor.TopLeft, 400, 96, 320, 18,
                          "УЧАСТКИ", 1.1)
    zc.TextColor = U.C(U.ACCENT)
    for i, z in ipairs(zones or {}) do
        local y = 120 + (i - 1) * 40
        local _, ze = U.Label(card, "Help Zone " .. i, UIAnchor.TopLeft, 400, y, 336, 18,
                              z.name, 1.2)
        ze.TextColor = U.C(U.TEXT)
        local _, za = U.Label(card, "Help Zone About " .. i, UIAnchor.TopLeft, 400, y + 17, 336, 16,
                              z.about or "", 1.0)
        za.TextColor = U.C(U.MUTED)
        za.WrapText = true
    end

    local _, close = U.Label(card, "Help Close", UIAnchor.BottomCenter, 0, 18, 400, 18,
                             "F1 — закрыть", 1.1)
    close.TextCentered = true
    close.TextColor = U.C(U.MUTED)

    help:GetUI().Visible = false
    keep("Help", help)
    return help
end

function H.SetHelpVisible(visible)
    local e = ui("Help")
    if e then e.Visible = visible end
end

function H.HelpVisible()
    local e = ui("Help")
    return e ~= nil and e.Visible
end

function H.SetVisible(visible)
    if root and root:Valid() then root:GetUI().Visible = visible end
end

-- Текущий участок: имя и пояснение проявляются при входе и гаснут при выходе.
-- Плавно, а не мгновенно: резкая смена крупной надписи в центре верха читается
-- как ошибка, особенно если пройти по границе двух участков.
function H.SetZone(name, about)
    els.zoneName = name
    els.zoneAbout = about
end

function H.SetHint(text)
    local e = ui("Hint")
    if e then e.Text = text or "" end
end

function H.SetModes(text)
    local e = ui("Modes")
    if e then e.Text = text or "" end
end

function H.Message(text, seconds)
    local e = ui("Message")
    if e == nil then return end
    e.Text = text or ""
    messageTimer = seconds or 2.4
end

function H.Update(dt)
    -- Сглаживание кадра: без него счётчик прыгает так, что прочитать его
    -- нельзя, а редкий тяжёлый кадр всё равно виден по скачку.
    frameMs = frameMs + ((dt * 1000.0) - frameMs) * math.min(1.0, dt * 4.0)
    local perf = ui("Perf")
    if perf then
        perf.Text = string.format("%.1f мс   %d fps", frameMs, math.floor(1000.0 / math.max(frameMs, 0.01) + 0.5))
    end

    local want = (els.zoneName ~= nil and els.zoneName ~= "") and 1.0 or 0.0
    titleFade = titleFade + (want - titleFade) * math.min(1.0, dt * 6.0)
    local t, a = ui("Zone Title"), ui("Zone About")
    if t then
        if els.zoneName and els.zoneName ~= "" then t.Text = els.zoneName end
        t.TextColor = U.C(U.TEXT, titleFade)
    end
    if a then
        if els.zoneAbout then a.Text = els.zoneAbout end
        a.TextColor = U.C(U.MUTED, titleFade * 0.9)
    end

    local m = ui("Message")
    if m then
        if messageTimer > 0.0 then
            messageTimer = messageTimer - dt
            -- Последние полсекунды — затухание: сообщение, исчезающее мгновенно,
            -- выглядит как сбой отрисовки.
            m.TextColor = U.C(U.TEXT, math.min(1.0, messageTimer * 2.0))
        else
            m.TextColor = U.C(U.TEXT, 0.0)
        end
    end
end

return H
