-- =============================================================================
--  05_kinematic_platform.lua — платформа-лифт, которая ездит туда-сюда и
--  толкает всё, что на неё поставили.
--
--  ЧТО ПРОВЕРЯЕТ. Кинематическое тело двигается через Transform, а физика
--  (Jolt) обязана толкать динамику БЕЗ дрожи на любой частоте кадров: цель
--  движения раскладывается по внутренним подшагам, а не проходится одним
--  скачком. Поставьте на платформу ящик и посмотрите, едет ли он вместе с
--  ней ровно, без подёргиваний и проваливания сквозь платформу.
--
--  ЧТО НУЖНО, ЧТОБЫ ЗАРАБОТАЛО:
--   1. Повесьте на плоский объект (например, растянутый куб-платформу) с
--      Collider (Box) и RigidBody.
--   2. В инспекторе RigidBody -> Body Type ОБЯЗАТЕЛЬНО поставьте Kinematic
--      (Static и Dynamic для платформы не подходят: Static не двигается
--      скриптом, Dynamic не ведётся Transform'ом напрямую).
--   3. Поставьте сверху обычный динамический ящик (Object -> Physics Cube,
--      RigidBody остаётся Dynamic) — он и есть «пассажир».
-- =============================================================================

travel = 4.0   -- метров в одну сторону от стартовой точки
speed = 1.2    -- условная скорость колебания

local startPos

function OnStart(entity)
    startPos = entity.Transform.Position
    local rb = entity:GetRigidBody()
    if not rb or rb.Type ~= BodyType.Kinematic then
        Debug.Log("[05_kinematic_platform] ВНИМАНИЕ: у " .. entity.Name ..
                  " RigidBody должен быть типа Kinematic (инспектор -> Rigid Body -> Body Type), " ..
                  "иначе платформа не будет толкать другие тела.")
    end
end

local t = 0.0

function OnUpdate(entity, dt)
    t = t + dt
    local offset = math.sin(t * speed) * travel
    entity.Transform.Position = Vec3(startPos.x, startPos.y, startPos.z + offset)
end
