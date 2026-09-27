-- События: своя подписка (Events.on), своё событие (Events.emit) и
-- событие клавиши (OnKeyDown) — без регистрации и классов.

key = "E"
opened_doors = 0

function Start()
    -- Подписка: функция зовётся при каждом "door_opened".
    Events.on("door_opened", function(event, door_name)
        opened_doors = opened_doors + 1
        Debug.log("Door opened: " .. tostring(door_name))
    end)
end

-- Нажатие клавиши приходит само.
function OnKeyDown(pressed)
    if pressed == key then
        Events.emit("door_open")
    end
end
