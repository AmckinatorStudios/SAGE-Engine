-- Физика из скрипта: тела и коллайдеры, собранные кодом, падают, бьются,
-- ловятся лучом, и сигналы collision / trigger_* доходят до подписчика.
-- Всё — в стороне от сцены (x = 200), чтобы не задеть её объекты.
local P = {}

local function box(name, pos, scale, body)
    local o = Scene.Create(name)
    o.transform.position = pos
    if scale then o.transform.scale = scale end
    o:AddComponent("RigidBody", body)
    o:AddComponent("Collider", {shape = "box"})
    return o
end

function P:before_each()
    self.floor = box("LT_Floor", Vector3(200, 0, 0), Vector3(20, 1, 20), {type = "static"})
end

function P:after_each()
    for _, name in ipairs({"LT_Floor", "LT_Crate", "LT_Zone", "LT_Ball"}) do
        local o = Scene.Find(name)
        if o then o:Destroy() end
    end
end

function P:test_body_falls_hits_and_lands()
    local crate = box("LT_Crate", Vector3(200, 4, 0), nil, {type = "dynamic", mass = 1})
    local hits, other = 0, nil
    crate.collision:connect(function(event, o) hits = hits + 1; other = o end)
    Test.waitUntil(function() return hits > 0 end, 5, "ящик не ударился о пол")
    Test.eq(other and other.id, self.floor.id, "ударился не о пол")
    Test.wait(1.0)
    Test.near(crate.transform.position.y, 1.0, 0.25, "ящик не лёг на пол")
end

function P:test_raycast_finds_the_body()
    local crate = box("LT_Crate", Vector3(200, 3, 0), nil, {type = "static"})
    Test.frames(2)   -- тело попадает в мир физики на её шаге
    local hit = Physics.Raycast(Vector3(200, 10, 0), Vector3(0, -1, 0), 50)
    Test.expect(hit ~= nil, "луч никуда не попал")
    Test.eq(hit.object.id, crate.id)
    Test.near(hit.point.y, 3.5, 0.05)
    Test.eq(Physics.Raycast(Vector3(200, 10, 0), Vector3(0, 1, 0), 50), nil, "луч вверх во что-то попал")
end

function P:test_trigger_zone_reports_enter()
    local zone = box("LT_Zone", Vector3(200, 3, 0), Vector3(4, 1, 4), {type = "static", sensor = true})
    local entered
    zone.trigger_entered:connect(function(event, o) entered = o end)
    local ball = Scene.Create("LT_Ball")
    ball.transform.position = Vector3(200, 7, 0)
    ball:AddComponent("RigidBody", {type = "dynamic"})
    ball:AddComponent("Collider", {shape = "sphere", radius = 0.3})
    Test.waitUntil(function() return entered ~= nil end, 5, "зона не заметила шар")
    Test.eq(entered.id, ball.id)
end

return P
