-- ---------------------------------------------------------------------------
-- world.lua — точка входа демо. Висит на сущности World в main.sage.
--
-- Обязанностей четыре: собрать мир из модулей, объявить раскладку, раз в кадр
-- прокрутить цикл в понятном порядке и держать переключатели рендера, которыми
-- демо и показывает движок.
--
-- Ни строчки C++: платформа, восемь участков, ходьба от первого лица,
-- процедурные текстуры, физика, частицы, свет, анимация, объёмный свет,
-- отражения и экран — всё это скрипты поверх обычного ECS движка. Ровно то,
-- что должно получаться у игры, написанной снаружи.
--
-- ПОРЯДОК В КАДРЕ. Сначала ввод, потом игрок (он двигается), потом участки (им
-- нужна уже НОВАЯ позиция игрока — иначе всё, что тянется за ним, отстаёт на
-- кадр), потом экран.
-- ---------------------------------------------------------------------------
local F      = require "floor"
local P      = require "player"
local Zones  = require "zones"
local Hub    = require "hub"
local HUD    = require "hud"

local started = false
local zones = {}
local debugIndex = 1        -- 1 — обычный вид (см. sage.render.DebugViews)
local debugViews = {}
local volumetrics = true
local lensFlare = true
local autopilot = nil
local statusTimer = 0.0

-- --- Раскладка --------------------------------------------------------------
local function bindControls()
    BindAction("Move Forward", {"W", "UP"})
    BindAction("Move Back",    {"S", "DOWN"})
    BindAction("Move Left",    {"A", "LEFT"})
    BindAction("Move Right",   {"D", "RIGHT"})
    BindAction("Jump",         "SPACE")
    BindAction("Run",          "LEFT_SHIFT")
    BindAction("Use",          "E")
    BindAction("Reset",        "R")
    BindAction("Lamp",         "L")
    BindAction("Debug View",   "V")
    BindAction("Volumetrics",  "B")
    BindAction("Lens Flare",   "N")
    BindAction("Help",         "F1")
end

local MOUSE_SENS = 0.11

local function readInput()
    local input = {
        moveF = 0.0, moveR = 0.0, jump = false, run = false,
        lookX = 0.0, lookY = 0.0,
        use = false, reset = false, lamp = false,
        debugView = false, volumetrics = false, flare = false, help = false,
    }
    if IsActionDown("Move Forward") then input.moveF = input.moveF + 1.0 end
    if IsActionDown("Move Back")    then input.moveF = input.moveF - 1.0 end
    if IsActionDown("Move Right")   then input.moveR = input.moveR + 1.0 end
    if IsActionDown("Move Left")    then input.moveR = input.moveR - 1.0 end
    input.jump = IsActionDown("Jump")
    input.run = IsActionDown("Run")
    input.use = WasActionPressed("Use")
    input.reset = WasActionPressed("Reset")
    input.lamp = WasActionPressed("Lamp")
    input.debugView = WasActionPressed("Debug View")
    input.volumetrics = WasActionPressed("Volumetrics")
    input.flare = WasActionPressed("Lens Flare")
    input.help = WasActionPressed("Help")

    if IsMouseCaptured() then
        local d = GetMouseDelta()
        input.lookX = d.x * MOUSE_SENS
        input.lookY = d.y * MOUSE_SENS
    end
    return input
end

-- --- Переключатели рендера --------------------------------------------------
--
-- Строка состояния собирается ЗДЕСЬ, а не в hud.lua: экран показывает то, что
-- ему дали, а знает о режимах рендера тот, кто их переключает.
local function modeLine()
    local parts = {}
    local view = debugViews[debugIndex]
    if view and view.id ~= "none" then
        parts[#parts + 1] = "разбор кадра: " .. view.name .. " — " .. view.hint
    end
    if not volumetrics then parts[#parts + 1] = "объём выключен" end
    if not lensFlare then parts[#parts + 1] = "блик выключен" end
    return table.concat(parts, "   |   ")
end

local function cycleDebugView()
    debugIndex = (debugIndex % #debugViews) + 1
    local view = debugViews[debugIndex]
    sage.render.SetDebugView(view.id)
    HUD.SetModes(modeLine())
    return view.id == "none" and "Обычный вид" or ("Разбор кадра: " .. view.name)
end

local function toggleVolumetrics()
    volumetrics = not volumetrics
    sage.volumetric.Set({enabled = volumetrics})
    HUD.SetModes(modeLine())
    return volumetrics and "Объёмный свет включён" or "Объёмный свет выключен"
end

local function toggleFlare()
    lensFlare = not lensFlare
    sage.lensflare.Set({enabled = lensFlare})
    HUD.SetModes(modeLine())
    return lensFlare and "Блик в объективе включён" or "Блик в объективе выключен"
end

-- --- Старт ------------------------------------------------------------------
function OnStart(entity)
    log("SHOWCASE: сборка витрины движка")

    bindControls()
    -- Меню паузы у плеера встроенное и здесь уместно: своего меню у витрины
    -- нет и не нужно, а выйти из неё чем-то надо.
    sage.game.SetPauseMenu(true)

    local tiles = F.Build()
    zones = Zones.Load()
    local built = Zones.Build()
    Hub.Build(zones)

    -- Твердь мира для контроллера персонажа: пол, центр и участки. Один
    -- предикат на всё — игрок не должен знать, из чего сложен мир.
    P.Init({
        solid = function(x0, y0, z0, x1, y1, z1)
            if F.Solid(x0, y0, z0, x1, y1, z1) then return true end
            if Hub.Solid(x0, y0, z0, x1, y1, z1) then return true end
            return Zones.Solid(x0, y0, z0, x1, y1, z1)
        end,
    })

    HUD.Build(zones)

    debugViews = sage.render.DebugViews()
    sage.render.SetDebugView("none")
    sage.volumetric.Set({
        enabled = true, shafts = true, clouds = true,
        density = 0.028, intensity = 0.8, coverage = 0.42,
        maxDistance = 120.0, heightFalloff = 0.05,
        cloudBottom = 120.0, cloudTop = 320.0,
    })
    sage.lensflare.Set({enabled = true, intensity = 0.8})
    -- Отражения: куб окружения на всю сцену. Плоское отражение включает
    -- участок материалов — оно одно на сцену, и владелец у него должен быть
    -- один (см. zone_material).
    sage.reflect.SetEnabled(true)

    -- Откуда и куда смотреть на старте (--at=x,y,z --look=yaw,pitch). Нужно
    -- ровно для снимков и проверок: поставить камеру в нужную точку витрины
    -- иначе можно только дойдя туда мышью, а headless-запуск мышью не
    -- управляют.
    local atArg = LaunchArg("at")
    if atArg then
        local ax, ay, az = atArg:match("^([^,]+),([^,]+),(.+)$")
        if ax then P.Teleport(tonumber(ax) or 0.0, tonumber(ay) or 1.2, tonumber(az) or 0.0) end
    end
    local lookArg = LaunchArg("look")
    if lookArg then
        local ly, lp = lookArg:match("^([^,]+),(.+)$")
        P.SetLook(tonumber(ly or lookArg) or 0.0, tonumber(lp) or 0.0)
    end
    -- Экран прячется на время снимков сцены: прицел и подписи поверх кадра
    -- мешают смотреть на то, ради чего снимок делается.
    if LaunchFlag("nohud") then HUD.SetVisible(false) end

    SetMouseCaptured(true)
    HUD.SetModes(modeLine())
    HUD.Message("F1 — справка. Дойди до участка — он расскажет о себе сам", 6.0)

    if LaunchFlag("autopilot") then
        autopilot = require "autopilot"
        autopilot.Start(zones)
    end

    started = true
    log(("SHOWCASE: платформа собрана, участков %d"):format(built))
end

function OnUpdate(entity, dt)
    if not started then return end

    local input = readInput()
    if autopilot then autopilot.Drive(dt, input, P) end

    -- Справка забирает мышь: под открытой карточкой обзор крутиться не должен.
    if input.help then
        HUD.SetHelpVisible(not HUD.HelpVisible())
        SetMouseCaptured(not HUD.HelpVisible())
    end
    if HUD.HelpVisible() then
        input.moveF, input.moveR = 0.0, 0.0
        input.lookX, input.lookY = 0.0, 0.0
        input.jump = false
    end

    P.Update(dt, input)

    if input.lamp then
        HUD.Message(P.ToggleLamp() and "Фонарь включён" or "Фонарь выключен", 1.6)
    end
    if input.debugView then HUD.Message(cycleDebugView(), 2.4) end
    if input.volumetrics then HUD.Message(toggleVolumetrics(), 2.0) end
    if input.flare then HUD.Message(toggleFlare(), 2.0) end

    local px, py, pz = P.pos.x, P.pos.y, P.pos.z
    local ctx = {playerPos = {x = px, y = py, z = pz}, dt = dt}

    Hub.Update(dt)
    if Zones.Update(dt, ctx) then
        local z = Zones.Current()
        HUD.SetZone(z and z.name or "", z and z.about or "")
        HUD.SetHint(z and z.Use and "E — действие,  R — собрать заново" or "")
    end

    local zone = Zones.Current()
    if input.use and zone and zone.Use then
        local msg = zone.Use()
        if msg and msg ~= "" then HUD.Message(msg, 2.6) end
    end
    if input.reset and zone and zone.Reset then
        local msg = zone.Reset()
        if msg and msg ~= "" then HUD.Message(msg, 2.6) end
    end

    HUD.Update(dt)

    -- Раз в десять секунд — строка в журнал. Она нужна не человеку за экраном,
    -- а тому, кто читает лог автопрогона: по ней видно, что демо живо и где
    -- находится игрок.
    statusTimer = statusTimer + dt
    if statusTimer >= 10.0 then
        statusTimer = 0.0
        log(("SHOWCASE: игрок (%.1f, %.1f, %.1f), участок: %s"):format(
            px, py, pz, zone and zone.name or "переход"))
    end
end
