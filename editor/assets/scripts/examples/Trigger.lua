-- Зона-триггер: кто вошёл — тому сообщение, а всем — событие event_name.
-- Объекту нужен коллайдер с галочкой «Сенсор» (rigid_body, sensor = true).

---@export
---@tooltip "Событие, которое зона посылает при входе"
event_name = "door_open"

entered_count = 0

events.connect(object, "trigger_entered", function(event)
    entered_count = entered_count + 1
    debug.log("Entered trigger: " .. event.data.name)
    events.trigger(event_name, event.data.name)
end)

-- Гостя могли удалить, пока он стоял в зоне: тогда data пусто.
events.connect(object, "trigger_exited", function(event)
    if event.data then debug.log("Left trigger: " .. event.data.name) end
end)
