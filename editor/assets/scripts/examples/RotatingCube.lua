-- Крутится вокруг оси. Скорость (градусов в секунду) и ось — в инспекторе.

---@export_range 0 360
speed = 90.0

---@export
axis = vector3.new(0, 1, 0)

function update(delta)
    object.transform.rotation = object.transform.rotation + axis * speed * delta
end
