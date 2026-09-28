-- =============================================================================
--  07_stress_crowd_dropper.lua — один раз, при старте Play, роняет целую
--  толпу тел разной формы и массы одновременно.
--
--  ЧТО ПРОВЕРЯЕТ. Одновременное взаимодействие МНОГИХ тел сразу: ни одно не
--  должно провалиться сквозь пол или друг друга, и рано или поздно (через
--  несколько секунд) вся куча обязана улечься и уснуть — понаблюдайте за
--  консолью редактора или просто за сценой.
--
--  ЧТО НУЖНО, ЧТОБЫ ЗАРАБОТАЛО: повесьте на любой объект — он только
--  отмечает точку, над которой появится куча. Под ним должен быть
--  достаточно большой пол: объект с RigidBody (Static) + Collider.
-- =============================================================================

count = 40
spread = 3.0        -- разброс по X/Z от точки объекта
drop_height = 8.0

function OnStart(entity)
    Debug.Log("[07_stress_crowd_dropper] " .. entity.Name .. ": спавню " .. count ..
              " тел сразу. Ждите, пока всё не уляжется — ни одно не должно провалиться в пол " ..
              "или зависнуть над ним.")

    local base = entity.Transform.Position
    for i = 1, count do
        local obj = SpawnObject("Толпа_" .. i)
        local x = base.x + (math.random() - 0.5) * spread * 2.0
        local z = base.z + (math.random() - 0.5) * spread * 2.0
        local y = base.y + drop_height + i * 0.15
        obj.Transform.Position = Vec3(x, y, z)

        local col = obj:AddCollider()
        local pick = i % 3
        if pick == 0 then
            SetMeshCube(obj)
            col.Shape = ColliderShape.Box
            col.HalfExtents = Vec3(0.25, 0.25, 0.25)
        elseif pick == 1 then
            SetMeshSphere(obj)
            col.Shape = ColliderShape.Sphere
            col.Radius = 0.25
        else
            SetMeshCapsule(obj)
            col.Shape = ColliderShape.Capsule
            col.Radius = 0.2
            col.HalfHeight = 0.25
        end

        local rb = obj:AddRigidBody()
        rb.Type = BodyType.Dynamic
        -- Разные массы нарочно: перепад масс в стопке/куче — самое частое
        -- место, где физика «расползается».
        rb.Mass = 0.3 + (i % 7)
        rb.Restitution = 0.05
    end
end
