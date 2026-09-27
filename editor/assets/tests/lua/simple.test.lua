-- Простой вид скрипта (функции Start/Update/On…, self, глобальные поля) —
-- на учебных скриптах из assets/scripts/examples, поставленных на объекты в
-- настоящем Play.
local S = {}
local DIR = "assets/scripts/examples/"

local function spawn(name, file, pos)
    local o = Scene.Create(name)
    if pos then o.transform.position = pos end
    local script = o:AddComponent("Script", {path = DIR .. file})
    return o, script
end

function S:after_each()
    for _, name in ipairs({"LT_Player", "LT_Cube", "LT_Door", "LT_Zone", "LT_Ball", "LT_Events", "LT_Floor"}) do
        for _, o in ipairs(Scene.find_all(name)) do o:destroy() end
    end
    Test.key("W", false)
end

function S:test_player_walks_forward_while_W_is_held()
    local player, script = spawn("LT_Player", "Player.lua", Vector3.new(300, 0, 0))
    Test.eq(script.speed, 5.0, "поле speed по умолчанию")
    Test.key("W")
    Test.wait(0.5)
    Test.key("W", false)
    Test.frames(1)
    local z = player.transform.position.z
    Test.expect(z > 1.5 and z < 3.5, "за полсекунды при speed=5 ожидалось ~2.5, вышло " .. z)
    Test.near(player.transform.position.x, 300, 1e-4, "W не должна сдвигать вбок")
end

function S:test_rotating_cube_turns_at_its_speed()
    local cube = spawn("LT_Cube", "RotatingCube.lua", Vector3.new(300, 0, 5))
    local before = cube.transform.rotation.y
    Test.wait(0.5)
    local turned = cube.transform.rotation.y - before
    Test.expect(turned > 30 and turned < 60, "за 0.5 с при 90°/с ожидалось ~45°, вышло " .. turned)
end

function S:test_door_opens_on_event_and_moves_up()
    local door, script = spawn("LT_Door", "Door.lua", Vector3.new(300, 0, 10))
    Test.eq(script.is_open, false)
    Events.emit("door_open")                  -- OnDoorOpen зовётся сам
    Test.eq(script.is_open, true, "OnDoorOpen не позвалась")
    Test.waitUntil(function() return door.transform.position.y > 2.9 end, 4, "дверь не поднялась")
end

function S:test_door_open_can_be_called_from_another_script()
    local door, script = spawn("LT_Door", "Door.lua", Vector3.new(300, 0, 15))
    Scene.find("LT_Door"):call("Open")
    Test.eq(script.is_open, true)
end

function S:test_key_E_opens_the_door_through_events_script()
    local _, events = spawn("LT_Events", "Events.lua", Vector3.new(300, 0, 20))
    local _, door = spawn("LT_Door", "Door.lua", Vector3.new(300, 0, 25))
    Test.key("E")
    Test.frames(2)
    Test.key("E", false)
    Test.eq(door.is_open, true, "OnKeyDown → Events.emit → OnDoorOpen не дошло")
    Test.eq(events.opened_doors, 1, "Events.on не услышал door_opened")
end

function S:test_trigger_zone_opens_the_door()
    local floor = Scene.Create("LT_Floor")
    floor.transform.position = Vector3.new(320, 0, 0)
    floor.transform.scale = Vector3.new(20, 1, 20)
    floor:AddComponent("RigidBody", {type = "static"})
    floor:AddComponent("Collider", {shape = "box"})
    local zone, trigger = spawn("LT_Zone", "Trigger.lua", Vector3.new(320, 3, 0))
    zone.transform.scale = Vector3.new(4, 1, 4)
    zone:AddComponent("RigidBody", {type = "static", sensor = true})
    zone:AddComponent("Collider", {shape = "box"})
    local _, door = spawn("LT_Door", "Door.lua", Vector3.new(320, 0, 8))
    local ball = Scene.Create("LT_Ball")
    ball.transform.position = Vector3.new(320, 7, 0)
    ball:AddComponent("RigidBody", {type = "dynamic"})
    ball:AddComponent("Collider", {shape = "sphere", radius = 0.3})
    Test.waitUntil(function() return trigger.entered_count > 0 end, 5, "OnTriggerEnter не позвался")
    Test.eq(door.is_open, true, "событие триггера не открыло дверь")
end

return S
