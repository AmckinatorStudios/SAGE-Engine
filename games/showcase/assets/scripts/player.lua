-- ---------------------------------------------------------------------------
-- player.lua — ходьба от первого лица по контроллеру персонажа ДВИЖКА.
--
-- Здесь намеренно нет своей физики персонажа. Контроллер (physics/CharacterMotor)
-- умеет всё, что нужно: скользить вдоль стен, всходить по ступеням, не
-- проваливаться на большой скорости и отличать «стою на земле» от «падаю». Своя
-- реализация повторила бы это заново, хуже и с другими ошибками, — а заодно
-- перестала бы быть проверкой движка, ради которой демо и сделано.
--
-- ТВЕРДЬ ЗАДАЁТ ДЕМО, А НЕ ДВИЖОК. Контроллер спрашивает у игры один вопрос:
-- «свободна ли эта коробка». Пол здесь клетчатая платформа, препятствия —
-- список ящиков и стен зон; никакого физического тела под ними нет и не нужно.
-- Тот же приём, что и в «Лодке», где твердь — воксельная палуба.
-- ---------------------------------------------------------------------------
local P = {}

local HEIGHT = 1.8
local HALF_W = 0.35
local EYE = 1.62

local WALK = 5.0
local RUN = 9.0
local ACCEL = 42.0          -- как быстро набираем скорость на земле
local AIR_ACCEL = 6.0       -- в воздухе управление есть, но слабое
local JUMP = 6.2
local GRAVITY = -22.0       -- заметно больше земной: с ней прыжок «плавает»
local MAX_PITCH = 88.0

P.pos = {x = 0.0, y = 1.0, z = -6.0}
P.vel = {x = 0.0, y = 0.0, z = 0.0}
P.yaw, P.pitch = 0.0, -4.0
P.onGround = false
P.running = false

local body, cam
local solid                 -- функция тверди, задаёт мир (см. Init)
local bob = 0.0
local bobPhase = 0.0

local function rad(d) return d * math.pi / 180.0 end

function P.Forward()
    local cp = math.cos(rad(P.pitch))
    return -cp * math.sin(rad(P.yaw)), math.sin(rad(P.pitch)), -cp * math.cos(rad(P.yaw))
end
function P.ForwardFlat() return -math.sin(rad(P.yaw)), 0.0, -math.cos(rad(P.yaw)) end
function P.RightFlat() return math.cos(rad(P.yaw)), 0.0, -math.sin(rad(P.yaw)) end
function P.EyePos() return P.pos.x, P.pos.y + EYE, P.pos.z end
function P.Camera() return cam end
function P.Body() return body end

function P.Init(deps)
    solid = deps.solid
    body = FindObject("Player")
    cam = FindObject("Player Camera")
    if body == nil or cam == nil then
        error("player.lua: в сцене нет 'Player' и/или 'Player Camera'")
    end
    cam.Transform.Position = Vec3(0.0, EYE, 0.0)

    sage.physics.SetCharacterShape(body, {
        radius = HALF_W, height = HEIGHT,
        -- Полметра шага: на бордюр и на низкую ступень всходят пешком, на ящик
        -- в метр — прыжком. Больше нельзя: с шагом в человеческий рост игрок
        -- «зашагивает» на стену любой высоты, и препятствий в демо не остаётся.
        step = 0.5, mass = 78.0,
    })
    sage.physics.SetCharacterWorld(body, function(x0, y0, z0, x1, y1, z1)
        return solid(x0, y0, z0, x1, y1, z1)
    end)

    -- Фонарь в руке: дочерний камере, поэтому иерархия доворачивает его за
    -- взглядом сама — скрипту не нужно догонять камеру каждый кадр.
    P.lamp = SpawnObject("Player Lamp")
    SetMeshNone(P.lamp)
    P.lamp:SetParent(cam)
    P.lamp.Transform.Position = Vec3(0.22, -0.18, 0.0)
    local L = P.lamp:AddLight()
    L.Kind = LightType.Spot
    L.Color = Vec3(1.0, 0.93, 0.80)
    L.Range = 34.0
    L.InnerConeDeg = 14.0
    L.OuterConeDeg = 30.0
    L.Intensity = 0.0
    P.lampOn = false

    P.Apply()
end

function P.ToggleLamp()
    P.lampOn = not P.lampOn
    P.lamp:GetLight().Intensity = P.lampOn and 3.2 or 0.0
    return P.lampOn
end

function P.SetLook(yaw, pitch)
    P.yaw = yaw % 360.0
    P.pitch = math.max(-MAX_PITCH, math.min(MAX_PITCH, pitch))
end

function P.Teleport(x, y, z)
    P.pos.x, P.pos.y, P.pos.z = x, y, z
    P.vel.x, P.vel.y, P.vel.z = 0.0, 0.0, 0.0
    P.Apply()
end

function P.Apply()
    local t = body.Transform
    t.Position = Vec3(P.pos.x, P.pos.y + bob, P.pos.z)
    t.Rotation.y = P.yaw
    cam.Transform.Rotation.x = P.pitch
end

function P.Update(dt, input)
    -- Взгляд. Мышь опрашивается движком и приходит уже в пикселях за кадр.
    P.yaw = (P.yaw - input.lookX) % 360.0
    P.pitch = math.max(-MAX_PITCH, math.min(MAX_PITCH, P.pitch - input.lookY))

    -- Желаемая горизонтальная скорость.
    local fx, _, fz = P.ForwardFlat()
    local rx, _, rz = P.RightFlat()
    local wx = fx * input.moveF + rx * input.moveR
    local wz = fz * input.moveF + rz * input.moveR
    local len = math.sqrt(wx * wx + wz * wz)
    if len > 1e-4 then wx, wz = wx / len, wz / len else wx, wz = 0.0, 0.0 end

    P.running = input.run and len > 0.01
    local speed = P.running and RUN or WALK
    local accel = P.onGround and ACCEL or AIR_ACCEL
    local v = P.vel
    v.x = v.x + (wx * speed - v.x) * math.min(1.0, accel * dt)
    v.z = v.z + (wz * speed - v.z) * math.min(1.0, accel * dt)

    v.y = v.y + GRAVITY * dt
    if input.jump and P.onGround then v.y = JUMP end

    sage.physics.SetCharacterPosition(body, Vec3(P.pos.x, P.pos.y, P.pos.z))
    sage.physics.MoveCharacter(body, Vec3(v.x, v.y, v.z), dt)
    local st = sage.physics.CharacterState(body)
    P.pos.x, P.pos.y, P.pos.z = st.position.x, st.position.y, st.position.z
    v.x, v.y, v.z = st.velocity.x, st.velocity.y, st.velocity.z
    P.onGround = st.grounded

    -- Упал с платформы — возвращаем в центр. Смерти в демо нет: это витрина,
    -- а не игра, и наказывать за шаг мимо края нечем и незачем.
    if P.pos.y < -12.0 then P.Teleport(0.0, 1.2, -6.0) end

    -- Покачивание на ходу. Амплитуда крошечная: на большой её видно как
    -- «камера дрожит», а нужна она ровно затем, чтобы шаг ощущался шагом.
    local moving = P.onGround and (math.abs(v.x) + math.abs(v.z)) > 0.6
    if moving then
        bobPhase = bobPhase + dt * (P.running and 13.0 or 9.0)
        bob = bob + (math.sin(bobPhase) * 0.035 - bob) * math.min(1.0, dt * 10.0)
    else
        bob = bob + (0.0 - bob) * math.min(1.0, dt * 8.0)
    end

    P.Apply()
end

return P
