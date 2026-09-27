-- Скрипты между собой: поставить скрипт на объект кодом, позвать его метод,
-- прочитать его таблицу; подписки скрипта уходят вместе с его объектом.
local S = {}
local HELPER = "assets/tests/lua/helper_counter.lua"

function S:test_add_script_starts_it()
    local o = Scene.Create("LT_Counter")
    local script = o:AddComponent("Script", {path = HELPER})
    Test.expect(script ~= nil, "AddComponent(Script) не вернул таблицу скрипта")
    Test.expect(script.started, "Start не позвался сразу")
    Test.eq(o:GetScript(), script)
    o:Destroy()
end

function S:test_call_a_method_of_another_script()
    local o = Scene.Create("LT_Counter")
    o:AddComponent("Script", {path = HELPER})
    Test.eq(o:Call("add", 5), 5)
    Test.eq(o:Call("add"), 6)
    Test.eq(o:GetScript().count, 6)
    Test.eq(o:Call("no_such_method"), nil, "несуществующий метод — nil, а не падение")
    o:Destroy()
end

function S:test_script_subscriptions_die_with_the_object()
    local o = Scene.Create("LT_Counter")
    local script = o:AddComponent("Script", {path = HELPER})
    Events.emit("lt_counter_tick")
    Test.eq(script.count, 1)
    Test.eq(Events.count("lt_counter_tick"), 1)
    o:Destroy()
    Events.emit("lt_counter_tick")   -- мёртвый обработчик молчит, без ошибок
    Test.eq(script.count, 1)
    Test.frames(2)                    -- кадр: рантайм убирает экземпляр
    Test.eq(Events.count("lt_counter_tick"), 0, "подписка пережила объект")
end

function S:test_bad_script_path_is_an_error()
    local o = Scene.Create("LT_BadScript")
    Test.errors(function() o:AddComponent("Script", {path = "assets/tests/lua/nope.lua"}) end, "nope.lua")
    Test.errors(function() o:AddComponent("Script", {}) end, "путь")
    o:Destroy()
end

return S
