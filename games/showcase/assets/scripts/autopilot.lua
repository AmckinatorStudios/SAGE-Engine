-- ---------------------------------------------------------------------------
-- autopilot.lua — витрина, которая показывает себя сама.
--
-- ЗАЧЕМ. Демо проверяется только одним способом — пройти его целиком: дойти до
-- каждого участка, нажать на нём всё, что нажимается, и посмотреть, что ничего
-- не сломалось. Руками это десять минут и делается ровно один раз перед
-- показом; здесь то же самое делает автопилот, и делает при каждом прогоне.
--
-- Он же нужен для снимков: чтобы снять участок, надо на нём оказаться, а
-- headless-запуск мышью не управляют.
--
-- Автопилот НЕ телепортируется: он ходит теми же клавишами, что и человек.
-- Иначе он проверял бы только то, что участки строятся, — а самое хрупкое в
-- демо это ходьба: контроллер персонажа, ступени, твердь участков.
-- ---------------------------------------------------------------------------
local A = {}

local order = {}
local step = 1
local phase = "walk"      -- walk -> act -> reset -> next
local timer = 0.0
local visited = 0
local failures = 0
local done = false
local stuckTimer = 0.0   -- сколько идём без заметного продвижения
local lastDist = math.huge
local sideTimer = 0.0    -- сколько ещё идти боком, обходя препятствие
local sideDir = 1.0

local ARRIVE = 9.0        -- считаем, что дошли, на этом расстоянии до центра
local ACT_WAIT = 1.6      -- сколько смотреть на результат E
local WALK_LIMIT = 45.0   -- дольше идти к одному участку незачем: что-то не так

function A.Start(zones)
    order = zones or {}
    step = 1
    phase = "walk"
    timer = 0.0
    visited = 0
    failures = 0
    done = false
    stuckTimer, lastDist, sideTimer, sideDir = 0.0, math.huge, 0.0, 1.0
    log("SHOWCASE: автопилот пошёл по участкам, всего " .. #order)
end

local function target()
    local z = order[step]
    return z and z.center or nil
end

-- Куда повернуться, чтобы идти на точку. Угол считается ТОТ ЖЕ, что и в
-- player.ForwardFlat: если формулы разойдутся, автопилот пойдёт боком.
local function yawTo(px, pz, tx, tz)
    return math.deg(math.atan(-(tx - px), -(tz - pz)))
end

function A.Drive(dt, input, P)
    if done or #order == 0 then return end
    timer = timer + dt

    local t = target()
    if t == nil then return end
    local dx, dz = t.x - P.pos.x, t.z - P.pos.z
    local dist = math.sqrt(dx * dx + dz * dz)

    if phase == "walk" then
        -- Смотрим на цель и идём вперёд. Взгляд ставится напрямую, а не
        -- «доворачивается»: доворот — это ещё один регулятор, который надо
        -- настраивать, а проверяем мы не его.
        P.SetLook(yawTo(P.pos.x, P.pos.z, t.x, t.z), -6.0)
        input.moveF = 1.0
        input.run = dist > 18.0

        -- ОБХОД ПРЕПЯТСТВИЯ. Путь от центра к участку проходит мимо обелиска,
        -- и, упершись в него ровно лбом, скользить не по чему: боковой
        -- составляющей у движения нет, и автопилот честно толкается в столб
        -- бесконечно. Человек в этой ситуации делает шаг вбок — здесь то же
        -- самое: не продвинулись на треть метра за полторы секунды — идём
        -- боком, пока не обойдём.
        if sideTimer > 0.0 then
            sideTimer = sideTimer - dt
            input.moveR = sideDir
        else
            stuckTimer = stuckTimer + dt
            if stuckTimer >= 1.5 then
                stuckTimer = 0.0
                if lastDist - dist < 0.3 then
                    sideTimer = 1.2
                    sideDir = -sideDir
                end
                lastDist = dist
            end
        end
        if dist <= ARRIVE then
            phase = "act"
            timer = 0.0
            stuckTimer, lastDist, sideTimer = 0.0, math.huge, 0.0
            visited = visited + 1
            log(("SHOWCASE: участок '%s' — дошёл"):format(order[step].name))
        elseif timer > WALK_LIMIT then
            failures = failures + 1
            log(("SHOWCASE: до участка '%s' дойти не удалось за %.0f с"):format(
                order[step].name, WALK_LIMIT))
            phase = "act"
            timer = 0.0
        end
        return
    end

    input.moveF = 0.0
    if phase == "act" then
        if timer >= 0.4 and timer < 0.4 + dt then input.use = true end
        if timer >= ACT_WAIT then phase = "reset"; timer = 0.0 end
        return
    end

    if phase == "reset" then
        if timer >= 0.3 and timer < 0.3 + dt then input.reset = true end
        if timer >= 1.2 then
            step = step + 1
            timer = 0.0
            if step > #order then
                done = true
                -- Итог одной строкой: её и ищет проверка в CI.
                if failures == 0 and visited == #order then
                    log(("SHOWCASE: ROUTINE OK — обойдено участков %d"):format(visited))
                else
                    log(("SHOWCASE: ROUTINE FAIL — дошёл до %d из %d"):format(visited, #order))
                end
                -- Выход по флагу, а не всегда: тот же автопилот гоняют и на
                -- глазах — чтобы посмотреть обход целиком, — и закрывать окно
                -- под человеком в этом случае незачем.
                if LaunchFlag("quit") then sage.game.Quit() end
            else
                phase = "walk"
                stuckTimer, lastDist, sideTimer = 0.0, math.huge, 0.0
            end
        end
    end
end

function A.Done() return done end

return A
