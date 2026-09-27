-- Объект сцены: имя, поиск, трансформ, родитель, теги, компоненты, удаление.
local T = {}

function T:test_create_find_rename()
    local o = Scene.Create("LT_Object")
    Test.expect(o ~= nil, "Scene.Create вернул nil")
    Test.eq(o.name, "LT_Object")
    Test.expect(o.id > 0, "id объекта не положительный")
    Test.eq(Scene.Find("LT_Object").id, o.id)
    Test.eq(Scene.FindById(o.id).name, "LT_Object")
    o.name = "LT_Renamed"
    Test.eq(Scene.Find("LT_Renamed").id, o.id)
    Test.eq(Scene.Find("LT_Object"), nil, "старое имя всё ещё находится")
    o:Destroy()
end

function T:test_transform_properties_and_methods()
    local o = Scene.Create("LT_Transform")
    local t = o.transform
    t.position = Vector3(1, 2, 3)
    Test.near(t.position.y, 2)
    t:Translate(Vector3(1, 0, 0))
    Test.near(t.position.x, 2)
    t:SetScale(2, 3, 4)
    Test.near(t.scale.z, 4)
    t:SetRotation(0, 90, 0)
    Test.near(t.rotation.y, 90)
    t:Rotate(0, 10, 0)
    Test.near(t.rotation.y, 100)
    Test.near(t.localPosition.x, t.position.x)
    o:Destroy()
end

function T:test_look_at_turns_forward_to_the_target()
    local o = Scene.Create("LT_Look")
    o.transform.position = Vector3(0, 0, 0)
    o.transform:LookAt(Vector3(10, 0, 0))
    Test.near(o.transform:Forward().x, 1, 1e-3, "Forward после LookAt(+X)")
    o.transform:LookAt(Vector3(0, 0, -5))
    Test.near(o.transform:Forward().z, -1, 1e-3, "Forward после LookAt(-Z)")
    o.transform:LookAt(Vector3(0, 5, 0.0001))
    Test.expect(o.transform:Forward().y > 0.99, "Forward после LookAt(вверх)")
    o:Destroy()
end

function T:test_parent_children_and_world_position()
    local p = Scene.Create("LT_Parent")
    local c = Scene.Create("LT_Child")
    c:SetParent(p)
    Test.eq(c.parent.id, p.id)
    Test.eq(#p.children, 1)
    Test.eq(p.children[1].id, c.id)
    p.transform.position = Vector3(10, 0, 0)
    c.transform.position = Vector3(1, 0, 0)
    Test.near(c.transform:WorldPosition().x, 11, 1e-3)
    c:SetParent(nil)
    Test.eq(c.parent, nil)
    Test.eq(#p.children, 0)
    p:Destroy()
    c:Destroy()
end

function T:test_destroy_takes_children_and_is_safe_twice()
    local p = Scene.Create("LT_P2")
    local c = Scene.Create("LT_C2")
    c:SetParent(p)
    p:Destroy()
    Test.expect(not p:IsValid(), "родитель жив после Destroy")
    Test.expect(not c:IsValid(), "ребёнок пережил родителя")
    Test.errors(function() return p.name end, "уничтожен")
    p:Destroy()   -- повторно: не ошибка
    Scene.Destroy(c)
end

function T:test_active_and_tag()
    local o = Scene.Create("LT_Tagged")
    o.tag = "lt_pickup"
    Test.eq(o.tag, "lt_pickup")
    Test.eq(Scene.FindByTag("lt_pickup").id, o.id)
    Test.eq(#Scene.FindAllByTag("lt_pickup"), 1)
    o.active = false
    Test.eq(o.active, false)
    Test.eq(o.visible, false, "visible и active разошлись у объекта сцены")
    o:show()
    Test.eq(o.active, true)
    o:Destroy()
end

function T:test_components_add_has_remove()
    local o = Scene.Create("LT_Components")
    Test.eq(o:HasComponent("RigidBody"), false)
    o:AddComponent("RigidBody", {type = "static", mass = 3})
    Test.eq(o:HasComponent("RigidBody"), true)
    o:AddComponent("Collider", {shape = "sphere", radius = 0.25})
    Test.eq(o:HasComponent("Collider"), true)
    Test.errors(function() o:AddComponent("RigidBody", {type = "flying"}) end, "flying")
    Test.errors(function() o:AddComponent("Teleporter") end, "Teleporter")
    Test.eq(o:GetComponent("Animation"), nil, "компонента нет — должен быть nil")
    Test.expect(o:RemoveComponent("RigidBody"))
    Test.eq(o:HasComponent("RigidBody"), false)
    o:Destroy()
end

function T:test_scene_create_and_destroy_by_id()
    local o = Scene.Create("LT_ById")
    local id = o.id
    Scene.Destroy(id)
    Test.eq(Scene.FindById(id), nil)
    Test.eq(Scene.Instantiate("assets/prefabs/nothing_here.sageprefab"), nil,
            "несуществующий префаб — nil, а не падение")
end

function T:test_math()
    Test.near(Vector3.Distance(Vector3(0, 0, 0), Vector3(3, 4, 0)), 5)
    Test.near(Vector3.Dot(Vector3(1, 0, 0), Vector3(0, 1, 0)), 0)
    Test.near(Vector3.MoveTowards(Vector3(0, 0, 0), Vector3(10, 0, 0), 2).x, 2)
    Test.near(Vector3.Angle(Vector3(1, 0, 0), Vector3(0, 1, 0)), 90, 1e-3)
    local v = Vector3(1, 2, 3) + Vector3(1, 1, 1)
    Test.near(v.z, 4)
    Test.near((v * 2).x, 4)
end

return T
