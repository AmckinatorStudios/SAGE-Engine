-- =============================================================================
--  06_mesh_collider_dropper.lua — раз в несколько секунд роняет сверху
--  очередную фигуру со столкновением ПО НАСТОЯЩЕМУ МЕШУ (Convex Hull), а не
--  по грубому боксу.
--
--  ЧТО ПРОВЕРЯЕТ. Новые формы коллайдера ConvexHull/Mesh: геометрия строится
--  из меша объекта (сварка вершин, оболочка Jolt), падает, крутится и ложится
--  на пол так же аккуратно, как обычный Box/Sphere/Capsule — без проваливания.
--
--  ЧТО НУЖНО, ЧТОБЫ ЗАРАБОТАЛО:
--   1. Повесьте на ЛЮБОЙ объект в сцене (он только отмечает точку, откуда
--      падают фигуры, — сам ничего не рисует и физики не имеет).
--   2. Под ним должен быть пол — объект с RigidBody (Static) + Collider.
--      Полезно сделать этот пол коллайдером ИМЕННО ПО МЕШУ: возьмите примитив
--      Plane (Object -> Plane), растяните его (Transform -> Scale), повесьте
--      RigidBody (Static) и Collider с формой "Mesh" — коллайдер построится
--      прямо по этой плоскости, без единого размера в самом коллайдере
--      (см. docs/physics.md, "Коллайдер по мешу").
--
--  ПОДСКАЗКА. Чтобы проверить по-настоящему ВОГНУТУЮ форму (не то, что итак
--  выпукло, как кубик или капсула), поставьте на объект со сложной моделью
--  свой .obj/.glb через SetMeshModel(obj, "assets/models/имя.obj") и
--  Collider.Shape = ColliderShape.Mesh — но ТОЛЬКО для Static/Kinematic:
--  у падающего (Dynamic) тела форма "Mesh" сама превращается в оболочку
--  (Convex Hull), потому что у сетки треугольников нет ни объёма, ни массы.
-- =============================================================================

spawn_interval = 1.5
max_alive = 12
spawn_height = 6.0

local timer = 0.0
local count = 0
local alive = {}
local shapes = {"cube", "sphere", "cylinder", "cone", "capsule"}

function OnStart(entity)
    Debug.Log("[06_mesh_collider_dropper] " .. entity.Name .. ": раз в " .. spawn_interval ..
              "с роняет фигуру с коллайдером Convex Hull, построенным по её мешу.")
end

local function spawn(kind, pos)
    count = count + 1
    local obj = SpawnObject("МешКоллайдер_" .. count)
    obj.Transform.Position = pos
    -- Случайный наклон при падении — чтобы фигура садилась не только плашмя,
    -- а училась ложиться на грань/бок, как в тестах падения.
    obj.Transform.Rotation = Vec3((30 * count) % 360, (17 * count) % 360, (53 * count) % 360)

    if kind == "cube" then SetMeshCube(obj)
    elseif kind == "sphere" then SetMeshSphere(obj)
    elseif kind == "cylinder" then SetMeshCylinder(obj)
    elseif kind == "cone" then SetMeshCone(obj)
    else SetMeshCapsule(obj) end

    local col = obj:AddCollider()
    col.Shape = ColliderShape.ConvexHull   -- геометрия — из меша, назначенного выше

    local rb = obj:AddRigidBody()
    rb.Type = BodyType.Dynamic
    rb.Mass = 0.6 + (count % 5) * 0.4
    rb.Restitution = 0.1

    table.insert(alive, obj)
    if #alive > max_alive then
        local victim = table.remove(alive, 1)
        if victim:Valid() then victim:Destroy() end
    end
end

function OnUpdate(entity, dt)
    timer = timer + dt
    if timer >= spawn_interval then
        timer = 0.0
        local p = entity.Transform.Position
        local kind = shapes[(count % #shapes) + 1]
        spawn(kind, Vec3(p.x + (count % 3) - 1.0, p.y + spawn_height, p.z + (count % 4) - 1.5))
    end
end
