-- Крутится вокруг оси. Скорость (градусов в секунду) и ось — в инспекторе.

speed = 90.0
axis = Vector3.new(0, 1, 0)

function Update(dt)
    self.transform.rotation = self.transform.rotation + axis * speed * dt
end
