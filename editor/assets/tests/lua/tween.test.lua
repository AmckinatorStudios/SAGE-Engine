-- Твины в настоящем Play: 3D-объект, интерфейс, свет, камера, цвет;
-- последовательность, параллель, повтор, туда-обратно, задержка, кривые.
-- Всё — в стороне от сцены (x = 400), чтобы не задеть её объекты.
local T = {}
local NAMES = {"LT_Tw", "LT_TwLamp", "LT_TwCam", "LT_TwBtn"}

local function obj(name)
    local o = Scene.create(name or "LT_Tw")
    o.transform.position = Vector3.new(400, 0, 0)
    return o
end

-- Время в Play — настоящие кадры, поэтому проверки с допуском на кадр.
local function near(a, b, eps, msg) Test.near(a, b, eps or 0.15, msg) end

function T:after_each()
    for _, n in ipairs(NAMES) do
        for _, o in ipairs(Scene.find_all(n)) do
            Tween.cancel(o)
            o:destroy()
        end
    end
end

function T:test_3d_position_rotation_scale()
    local o = obj()
    local done = false
    Tween.to(o, {position = Vector3.new(404, 2, 0), rotation = Vector3.new(0, 90, 0), scale = 2}, 0.5, Ease.Linear)
        :on_complete(function() done = true end)
    Test.wait(0.25)
    near(o.transform.position.x, 402, 0.4, "на полпути по x")
    Test.waitUntil(function() return done end, 2, "on_complete не пришёл")
    near(o.transform.position.x, 404, 1e-3)
    near(o.transform.position.y, 2, 1e-3)
    near(o.transform.rotation.y, 90, 1e-3)
    near(o.transform.scale.z, 2, 1e-3, "число — на все три оси")
end

function T:test_ui_position_opacity_scale()
    local b = UI.create("Button", "LT_TwBtn")
    Tween.to(b, {position = Vector2(120, 40), opacity = 0, scale = 1.5}, 0.3, Ease.Out)
    Test.waitUntil(function() return not Tween.is_playing(b) end, 2)
    near(b:get("opacity"), 0, 1e-3)
    near(b:get("scale"), 1.5, 1e-3)
    near(b:get("position").x, 120, 1e-3)
    -- from: из прозрачного к нынешнему.
    Tween.from(b, "opacity", 1, 0.2, Ease.Linear)
    Test.frames(1)
    Test.expect(b:get("opacity") > 0.5, "from начинает с заданного")
end

function T:test_light_intensity_and_camera_fov()
    local lamp = obj("LT_TwLamp")
    lamp:add_component("Light", {intensity = 1})
    local cam = obj("LT_TwCam")
    cam:add_component("Camera", {fov = 60})
    Tween.to(lamp, "intensity", 5, 0.3, Ease.Linear)
    Tween.to(cam, "fov", 90, 0.3, Ease.InOut)
    Test.waitUntil(function() return not Tween.is_playing(lamp) and not Tween.is_playing(cam) end, 2)
    near(lamp:get("intensity"), 5, 1e-3)
    near(cam:get("fov"), 90, 1e-3)
end

function T:test_color()
    local o = obj()
    o:add_component("Mesh", {color = Vector3.new(1, 1, 1)})
    Tween.to(o, "color", Vector3.new(1, 0, 0), 0.3)
    Test.waitUntil(function() return not Tween.is_playing(o) end, 2)
    local c = o:get("color")
    near(c.x, 1, 1e-3)
    near(c.y, 0, 1e-3)
end

function T:test_sequence_parallel_and_wait()
    local o = obj()
    local tw = Tween.to(o, "position", Vector3.new(403, 0, 0), 0.3, Ease.Linear)
        :wait(0.3)
        :then_to("scale", 2, 0.3, Ease.Linear)
        :with("rotation", Vector3.new(0, 180, 0), 0.3, Ease.Linear)
    near(tw:duration(), 0.9, 1e-4)
    Test.wait(0.45)                                  -- в паузе
    near(o.transform.position.x, 403, 1e-3, "первый шаг закончен")
    near(o.transform.scale.x, 1, 1e-3, "второй шаг ещё не начался")
    Test.waitUntil(function() return not tw:is_playing() end, 2)
    near(o.transform.scale.x, 2, 1e-3)
    near(o.transform.rotation.y, 180, 1e-3, "шёл вместе со вторым")
end

function T:test_loop_pingpong_and_delay()
    local o = obj()
    local loop = Tween.to(o, "position", Vector3.new(0, 4, 0) + o.transform.position, 0.2, Ease.Linear):loop()
    Test.wait(0.5)
    Test.expect(loop:is_playing(), "повтор не кончается сам")
    loop:cancel()

    local p = obj()
    local ping = Tween.to(p, "scale", 3, 0.2, Ease.Linear):ping_pong()
    Test.wait(0.3)                                   -- обратный ход
    Test.expect(p.transform.scale.x < 3 and p.transform.scale.x > 1, "туда-обратно идёт назад")
    ping:cancel()

    local d = obj()
    Tween.to(d, "scale", 2, 0.2, Ease.Linear):delay(0.4)
    Test.wait(0.2)
    near(d.transform.scale.x, 1, 1e-4, "в задержке не двигается")
    Test.waitUntil(function() return not Tween.is_playing(d) end, 2)
    near(d.transform.scale.x, 2, 1e-3)
end

function T:test_easing_and_curve()
    local a = obj()
    local peak = 0
    Tween.to(a, "scale", 2, 0.6, Ease.OutBack)
    for _ = 1, 60 do
        peak = math.max(peak, a.transform.scale.x)
        if not Tween.is_playing(a) then break end
        Test.frames(1)
    end
    Test.expect(peak > 2.02, "OutBack перелетает цель, вышло " .. peak)
    near(a.transform.scale.x, 2, 1e-3)

    local b = obj()
    Tween.to(b, "scale", 2, 0.5, {0, 0, 0.2, 1})     -- своя кривая: резкий старт
    Test.wait(0.12)
    Test.expect(b.transform.scale.x > 1.35, "своя кривая: к 20 % времени — больше трети пути")
end

function T:test_pause_resume_and_errors()
    local o = obj()
    local tw = Tween.to(o, "scale", 3, 0.4, Ease.Linear)
    Test.frames(2)
    tw:pause()
    local s = o.transform.scale.x
    Test.wait(0.2)
    near(o.transform.scale.x, s, 1e-5, "пауза держит")
    tw:resume()
    Test.waitUntil(function() return not tw:is_playing() end, 2)
    near(o.transform.scale.x, 3, 1e-3)
    Test.errors(function() Tween.to(o, "postion", 1, 1) end, "postion")
end

-- Учебный скрипт Pulse.lua: твин из Start и последовательность по пробелу.
function T:test_pulse_example_breathes_and_jumps()
    local o = obj()
    o:add_component("Script", {path = "assets/scripts/examples/Pulse.lua"})
    Test.wait(0.3)
    Test.expect(o.transform.scale.x > 1.03, "Pulse не дышит")
    Test.key("Space")
    Test.frames(2)
    Test.key("Space", false)
    Test.waitUntil(function() return o.transform.position.y > 1.5 end, 2, "по пробелу не подпрыгнул")
    Test.waitUntil(function() return o.transform.position.y < 0.01 end, 3, "не вернулся на место")
end

-- Твин, собранный в редакторе (TweenComponent с «Играть при запуске»),
-- в Play стартует сам и находится по имени. Объект ставит самопроверка
-- редактора; в чужом проекте его нет — тогда проверять нечего.
function T:test_editor_tween_plays_on_start_and_by_name()
    local o = Scene.find("LT_EditorTween")
    if not o then return end
    Test.waitUntil(function() return o.transform.position.y > 2.99 end, 3, "твин редактора не стартовал сам")
    Test.expect(Tween.play(o, "Pulse") ~= nil, "Tween.play не нашёл твин по имени")
    Test.eq(Tween.play(o, "NoSuchTween"), nil)
end

return T
