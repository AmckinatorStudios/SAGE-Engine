-- События: объявить своё (events.create), вызвать (events.trigger),
-- подключить (events.connect), подключить на один раз (events.connect_once),
-- отключить (events.disconnect) — и клавиша «E», просящая открыть дверь.

---@export
key = "E"

opened_doors = 0

-- Своё событие этого объекта: оно появится в инспекторе до запуска игры.
events.create(object, "doors_counted")

local function on_door_opened(event)
    opened_doors = opened_doors + 1
    debug.log("Door opened: " .. tostring(event.data))
    events.trigger(object, "doors_counted", {total = opened_doors})
end

function start()
    -- Обработчик зовётся при каждом "door_opened"…
    events.connect("door_opened", on_door_opened)
    -- …а этот — только при первом.
    events.connect_once("door_opened", function(event)
        debug.log("First door opened")
    end)
    -- Другой скрипт слушает то же событие так: events.connect(object, "doors_counted", fn).
end

function update(delta)
    if input.is_key_pressed(key) then
        events.trigger("door_open")
    end
end

-- Отключить обработчик можно тем же именем и функцией.
function on_destroy()
    events.disconnect("door_opened", on_door_opened)
end
