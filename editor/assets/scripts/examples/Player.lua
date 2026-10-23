-- Игрок: WASD двигает объект, пробел — прыжок, столкновения — в консоль.
-- Переменные сверху видны в инспекторе (Script → speed, jump_force).
--
-- «Вперёд» берётся у самого объекта (transform:Forward() — туда, куда смотрит
-- камера), а не осью мира: повёрнутый игрок обязан идти туда, куда повёрнут.

speed = 5.0
jump_force = 8.0

function Start()
    Debug.log("Player started: " .. self.name)
end

function Update(dt)
    local ahead = 0
    local side = 0

    if Input.is_key_down("W") then ahead = ahead + 1 end
    if Input.is_key_down("S") then ahead = ahead - 1 end
    if Input.is_key_down("D") then side = side + 1 end
    if Input.is_key_down("A") then side = side - 1 end

    local direction = self.transform:Forward() * ahead + self.transform:Right() * side
    direction.y = 0

    self.transform.position =
        self.transform.position + direction * speed * dt

    if Input.is_key_pressed("Space") then
        Debug.log("Jump")
        self.transform.position.y = self.transform.position.y + jump_force * 0.1
    end
end

function OnCollisionEnter(other)
    Debug.log("Hit " .. other.name)
end
