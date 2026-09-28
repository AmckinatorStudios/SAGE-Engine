-- =============================================================================
--  08_rope_and_cut.lua — подвешивает груз на трос (Distance joint) и по
--  нажатию клавиши трос "перерезает".
--
--  ЧТО ПРОВЕРЯЕТ. Соединения (joints) и то, что снятие соединения СРАЗУ
--  будит тело: раньше отрезанный груз мог повиснуть в воздухе, потому что
--  сон физики не считал разрыв соединения поводом проснуться.
--
--  ЧТО НУЖНО, ЧТОБЫ ЗАРАБОТАЛО: повесьте на объект-якорь (например, пустышку
--  под потолком). Груз скрипт создаёт сам — никаких других объектов в сцене
--  не нужно, кроме пола под ним (RigidBody Static + Collider), чтобы было
--  видно, куда груз в итоге упадёт.
--
--  УПРАВЛЕНИЕ: C — перерезать трос.
-- =============================================================================

rope_length = 3.0
bob_mass = 2.0

local bob

function OnStart(entity)
    Debug.Log("[08_rope_and_cut] " .. entity.Name .. ": подвешиваю груз на трос длиной " ..
              rope_length .. ". Нажмите C, чтобы перерезать трос — груз обязан СРАЗУ начать " ..
              "падать/качаться, а не зависнуть в воздухе.")

    -- Якорь должен быть неподвижным телом; если RigidBody ещё нет — заводим
    -- статику сами, чтобы демо работало без ручной настройки якоря.
    if not entity:HasRigidBody() then
        local anchorRb = entity:AddRigidBody()
        anchorRb.Type = BodyType.Static
    end

    local p = entity.Transform.Position
    bob = SpawnObject("Груз")
    bob.Transform.Position = Vec3(p.x, p.y - rope_length, p.z)
    SetMeshSphere(bob)
    local col = bob:AddCollider()
    col.Shape = ColliderShape.Sphere
    col.Radius = 0.3
    local rb = bob:AddRigidBody()
    rb.Type = BodyType.Dynamic
    rb.Mass = bob_mass

    local jc = bob:AddJoint()
    jc.Type = JointType.Distance
    jc.TargetId = entity.Id
    -- Anchor — смещение точки крепления ОТ ПОЗИЦИИ ГРУЗА (владельца
    -- соединения), поэтому здесь ровно вектор "вверх на длину троса".
    jc.Anchor = Vec3(0.0, rope_length, 0.0)
    jc.MinDistance = 0.0
    jc.MaxDistance = rope_length
end

function OnUpdate(entity, dt)
    if bob and bob:Valid() and Input.IsKeyPressed("C") then
        if bob:HasJoint() then
            bob:RemoveJoint()
            Debug.Log("[08_rope_and_cut] трос перерезан")
        end
    end
end
