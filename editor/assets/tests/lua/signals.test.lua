-- Сигналы и события: connect, once, disconnect, emit, свои события,
-- глобальная шина, уничтожение объекта, ошибки.
local S = {}

function S:test_connect_emit_and_multiple_listeners()
    local o = Scene.Create("LT_Sig")
    local a, b, data = 0, 0, nil
    o:signal("ping"):connect(function(event, d) a = a + 1; data = d end)
    o:on("ping", function() b = b + 1 end)
    o:emit("ping", {n = 7})
    Test.eq(a, 1)
    Test.eq(b, 1)
    Test.eq(data.n, 7, "таблица данных не дошла как есть")
    Test.eq(o:signal("ping"):count(), 2)
    o:Destroy()
end

function S:test_disconnect_is_safe_to_repeat()
    local o = Scene.Create("LT_Disc")
    local n = 0
    local c = o:on("tick", function() n = n + 1 end)
    Test.eq(c.connected, true)
    o:emit("tick")
    Test.eq(c:disconnect(), true)
    Test.eq(c:disconnect(), false, "повторный disconnect не false")
    Test.eq(c.connected, false)
    o:emit("tick")
    Test.eq(n, 1)
    o:Destroy()
end

function S:test_once_fires_once()
    local o = Scene.Create("LT_Once")
    local n = 0
    o:once("boom", function() n = n + 1 end)
    o:emit("boom")
    o:emit("boom")
    Test.eq(n, 1)
    o:Destroy()
end

function S:test_event_table_has_name_sender_data()
    local o = Scene.Create("LT_Event")
    local got
    o:on("player_died", function(event) got = event end)
    o:emit("player_died", 42)
    Test.eq(got.name, "player_died")
    Test.eq(got.sender.id, o.id)
    Test.eq(got.data, 42)
    o:Destroy()
end

function S:test_signals_are_per_object()
    local a, b = Scene.Create("LT_A"), Scene.Create("LT_B")
    local na, nb = 0, 0
    a:on("hit", function() na = na + 1 end)
    b:on("hit", function() nb = nb + 1 end)
    a:emit("hit")
    Test.eq(na, 1)
    Test.eq(nb, 0, "сигнал одного объекта услышал другой")
    a:Destroy(); b:Destroy()
end

function S:test_global_events()
    local got, n = nil, 0
    local c = Events.on("lt_level_loaded", function(event, name) got = name; n = n + 1 end)
    Events.emit("lt_level_loaded", "forest")
    Test.eq(got, "forest")
    Test.eq(Events.count("lt_level_loaded"), 1)
    Events.off(c)
    Events.emit("lt_level_loaded", "cave")
    Test.eq(n, 1)
    -- Прежние имена — те же функции.
    local legacy = 0
    Events.On("lt_old", function() legacy = legacy + 1 end)
    Events.Emit("lt_old")
    Test.eq(legacy, 1)
    Events.off("lt_old")
end

function S:test_destroying_an_object_drops_its_connections()
    local o = Scene.Create("LT_Doomed")
    local sig = o:signal("ping")
    local n = 0
    local c = sig:connect(function() n = n + 1 end)
    o:Destroy()
    Test.eq(c.connected, false, "подписка пережила объект")
    Test.errors(function() sig:connect(function() end) end, "уничтожен")
    Test.errors(function() o:emit("ping") end, "уничтожен")
    Test.eq(n, 0)
end

function S:test_handler_error_does_not_stop_neighbours()
    local o = Scene.Create("LT_Err")
    local second = false
    o:on("x", function() error("намеренная ошибка теста") end)
    o:on("x", function() second = true end)
    o:emit("x")
    Test.expect(second, "ошибка первого обработчика остановила второй")
    o:Destroy()
end

function S:test_bad_arguments_are_explained()
    local o = Scene.Create("LT_Bad")
    Test.errors(function() o:on("", function() end) end, "пустое имя")
    Test.errors(function() o:on(5, function() end) end, "строкой")
    Test.errors(function() o:signal("a b") end, "пробелы")
    Test.errors(function() o:signal("x"):connect(42) end, "функцией")
    Test.errors(function() o:signal("x").connect(function() end) end, "через точку")
    o:Destroy()
end

function S:test_emit_from_a_handler_and_disconnect_inside()
    local o = Scene.Create("LT_Nest")
    local chain = {}
    local c
    c = o:on("a", function() chain[#chain + 1] = "a"; c:disconnect(); o:emit("b") end)
    o:on("b", function() chain[#chain + 1] = "b" end)
    o:emit("a")
    o:emit("a")
    Test.eq(table.concat(chain, ","), "a,b")
    o:Destroy()
end

return S
