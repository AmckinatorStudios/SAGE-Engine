-- ---------------------------------------------------------------------------
-- zone_physics.lua — участок «Физика».
--
-- Что здесь показано и почему именно это:
--
--   СТОПКА ЯЩИКОВ. Самая честная проверка решателя: стопка либо стоит, либо
--   медленно расползается, и второе видно сразу без всяких приборов. Стоит она
--   на тёплом старте и накопленных импульсах — то есть на том, что решателю
--   труднее всего.
--
--   ПИРАМИДА. Стопка проверяет вертикаль, пирамида — трение: без него верхние
--   ряды съезжают вниз по наклону контактов.
--
--   МАЯТНИК НА ШАРНИРЕ. Соединение, а не свободное тело: проверяется, что
--   ограничение держит длину и не накачивает энергию. Маятник, который со
--   временем раскачивается сам, — классический признак ошибки знака в смещении.
--
--   КУКЛА (ragdoll). Тринадцать тел и дюжина ограничений разом — то, что ломает
--   решатель, если он неустойчив при разных массах.
--
-- E роняет на стопку тяжёлый шар. Не «сбросить всё», а именно один шар:
-- интересно не то, что предметы падают, а как они разлетаются от удара.
-- ---------------------------------------------------------------------------
local Mat = require("mat")

local Z = {}

Z.name = "Физика"
Z.about = "Стопки, трение, шарнир и кукла. E — уронить шар"
Z.center = {x = 0.0, z = 34.0}

local root
local boxes = {}          -- ящики стопки и пирамиды: их сбрасывает R
local starts = {}
local ball
local ballStart = Vec3(0, 14, 0)
local time = 0.0

local function body(o, mass, half, restitution)
    local rb = o:AddRigidBody()
    rb.Type = BodyType.Dynamic
    rb.Mass = mass
    rb.Friction = 0.62
    rb.Restitution = restitution or 0.03
    local c = o:AddCollider()
    c.Shape = ColliderShape.Box
    c.HalfExtents = half
    return rb
end

local function crate(x, y, z, size, tint)
    local o = SpawnObject("Crate")
    SetMeshCube(o)
    o.Transform.Position = Vec3(x, y, z)
    o.Transform.Scale = Vec3(size, size, size)
    o.Color = tint
    Mat.Apply(o, 0.0, 0.72)
    body(o, size * size * size * 24.0, Vec3(size * 0.5, size * 0.5, size * 0.5))
    o:SetParent(root)
    boxes[#boxes + 1] = o
    starts[#starts + 1] = {x, y, z}
    return o
end

-- Пол участка — СТАТИЧЕСКОЕ тело. Клетчатая платформа физике не известна: по
-- ней ходит контроллер персонажа (свой запрос тверди), а телам нужна опора в
-- том же мире, в котором они считаются.
local function ground(cx, cz)
    local o = SpawnObject("Physics Ground")
    SetMeshNone(o)
    o.Transform.Position = Vec3(cx, -0.5, cz)
    local rb = o:AddRigidBody()
    rb.Type = BodyType.Static
    rb.Friction = 0.8
    local c = o:AddCollider()
    c.Shape = ColliderShape.Box
    c.HalfExtents = Vec3(14.0, 0.5, 14.0)
    o:SetParent(root)
end

function Z.Build()
    root = SpawnObject("Zone Physics")
    SetMeshNone(root)
    local cx, cz = Z.center.x, Z.center.z
    ground(cx, cz)

    -- Стопка из восьми: высокая настолько, чтобы неустойчивость была видна.
    for i = 0, 7 do
        crate(cx - 4.0, 0.55 + i * 1.02, cz, 1.0,
              Vec3(0.72, 0.44 + i * 0.02, 0.24))
    end

    -- Пирамида: пять в основании, вверх по одному.
    for row = 0, 4 do
        for i = 0, 4 - row do
            crate(cx + 2.0 + (i + row * 0.5) * 0.92 - 1.8, 0.46 + row * 0.9, cz + 3.5, 0.88,
                  Vec3(0.34, 0.48, 0.66))
        end
    end

    -- Маятник: неподвижный столб и груз на шарнире.
    local post = SpawnObject("Pendulum Post")
    SetMeshCube(post)
    post.Transform.Position = Vec3(cx + 6.5, 3.0, cz - 3.0)
    post.Transform.Scale = Vec3(0.25, 6.0, 0.25)
    post.Color = Vec3(0.22, 0.23, 0.26)
    Mat.Apply(post, 0.8, 0.35)
    local prb = post:AddRigidBody()
    prb.Type = BodyType.Static
    local pc = post:AddCollider()
    pc.Shape = ColliderShape.Box
    pc.HalfExtents = Vec3(0.125, 3.0, 0.125)
    post:SetParent(root)

    local bobObj = SpawnObject("Pendulum Bob")
    SetMeshSphere(bobObj)
    bobObj.Transform.Position = Vec3(cx + 9.2, 4.6, cz - 3.0)
    bobObj.Transform.Scale = Vec3(0.9, 0.9, 0.9)
    bobObj.Color = Vec3(0.85, 0.78, 0.35)
    Mat.Apply(bobObj, 0.95, 0.22)
    local brb = bobObj:AddRigidBody()
    brb.Type = BodyType.Dynamic
    brb.Mass = 40.0
    brb.Friction = 0.4
    local bc = bobObj:AddCollider()
    bc.Shape = ColliderShape.Sphere
    bc.Radius = 0.45
    local j = bobObj:AddJoint()
    j.Type = JointType.Distance
    j.TargetId = post.Id
    j.Anchor = Vec3(cx + 6.5, 5.8, cz - 3.0)
    j.MinDistance = 2.6
    j.MaxDistance = 2.7
    bobObj:SetParent(root)

    -- Шар, который роняют по E. Лежит он В СТОРОНЕ, а точка сброса — над
    -- стопкой: поставленный сразу над ней, он падает на старте демо и
    -- разносит стопку раньше, чем на неё кто-нибудь посмотрит.
    ballStart = Vec3(cx - 4.0, 13.0, cz)
    ball = SpawnObject("Drop Ball")
    SetMeshSphere(ball)
    ball.Transform.Position = Vec3(cx + 9.0, 0.9, cz + 8.0)
    ball.Transform.Scale = Vec3(1.6, 1.6, 1.6)
    ball.Color = Vec3(0.80, 0.24, 0.20)
    Mat.Apply(ball, 0.15, 0.42)
    local rb = ball:AddRigidBody()
    rb.Type = BodyType.Dynamic
    rb.Mass = 240.0
    rb.Friction = 0.5
    rb.Restitution = 0.18
    local c = ball:AddCollider()
    c.Shape = ColliderShape.Sphere
    c.Radius = 0.8
    ball:SetParent(root)

    -- Кукла: собирается движком одним вызовом (physics::BuildRagdoll).
    SpawnRagdoll(Vec3(cx - 9.0, 3.4, cz + 4.0), 1.0)
end

-- Препятствия зоны для ходьбы. Столб маятника — единственное, во что здесь
-- можно упереться; ящики и кукла ДВИЖУТСЯ, и делать их твердью для игрока
-- значило бы держать их список в двух системах сразу и рассинхронизировать.
function Z.Solid(minX, minY, minZ, maxX, maxY, maxZ)
    local cx, cz = Z.center.x, Z.center.z
    return maxX > cx + 6.25 and minX < cx + 6.75
       and maxZ > cz - 3.25 and minZ < cz - 2.75
       and minY < 6.0
end

function Z.Use()
    if ball == nil then return "" end
    ball.Transform.Position = ballStart
    sage.physics.SetVelocity(ball, Vec3(0, -2, 0))
    return "Шар сброшен"
end

-- R возвращает участок в исходное состояние: демо смотрят подряд несколько
-- человек, и второму должно достаться то же, что и первому.
function Z.Reset()
    for i, o in ipairs(boxes) do
        local s = starts[i]
        o.Transform.Position = Vec3(s[1], s[2], s[3])
        o.Transform.Rotation = Vec3(0, 0, 0)
        sage.physics.SetVelocity(o, Vec3(0, 0, 0))
    end
    if ball then
        local cx, cz = Z.center.x, Z.center.z
        ball.Transform.Position = Vec3(cx + 9.0, 0.9, cz + 8.0)
        sage.physics.SetVelocity(ball, Vec3(0, 0, 0))
    end
    return "Участок собран заново"
end

function Z.Update(dt) time = time + dt end

return Z
