-- Прежний API (sage.*) — рядом с новым и в той же шине событий.
local G = {}

function G:test_sage_events_and_Events_are_one_bus()
    local fromNew, fromOld = nil, nil
    local id = sage.events.On("lt_bridge", function(p) fromOld = p end)
    local c = Events.on("lt_bridge", function(event, data) fromNew = data end)
    sage.events.Emit("lt_bridge", {x = 1})
    Test.eq(fromOld and fromOld.x, 1, "sage.events не услышал своё событие")
    Test.eq(fromNew and fromNew.x, 1, "Events.on не услышал sage.events.Emit")
    fromOld = nil
    Events.emit("lt_bridge", {x = 2})
    Test.eq(fromOld and fromOld.x, 2, "sage.events не услышал Events.emit")
    sage.events.Off(id)
    c:disconnect()
end

function G:test_timers_fire_later()
    local fired = false
    sage.time.Schedule(0.2, function() fired = true end)
    Test.eq(fired, false)
    Test.waitUntil(function() return fired end, 3, "sage.time.Schedule не сработал")
end

function G:test_math_helpers()
    Test.near(sage.math.Clamp(5, 0, 3), 3)
    Test.near(sage.math.Lerp(0, 10, 0.25), 2.5)
end

return G
