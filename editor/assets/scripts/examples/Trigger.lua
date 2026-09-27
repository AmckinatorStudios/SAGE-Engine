-- Зона-триггер: кто вошёл — тому сообщение, а всем — событие event_name.
-- Объекту нужен коллайдер с галочкой «Сенсор» (RigidBody sensor = true).

event_name = "door_open"
entered_count = 0

function OnTriggerEnter(other)
    entered_count = entered_count + 1
    Debug.log("Entered trigger: " .. other.name)
    Events.emit(event_name, other.name)
end

function OnTriggerExit(other)
    if other then Debug.log("Left trigger: " .. other.name) end
end
