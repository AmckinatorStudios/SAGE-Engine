-- Игрок: WASD двигает объект, пробел — прыжок, столкновения — в консоль.
-- Переменные сверху видны в инспекторе (Script → speed, jump_force).

speed = 5.0
jump_force = 8.0

function Start()
    Debug.log("Player started: " .. self.name)
end

function Update(dt)
    local direction = Vector3.zero()

    if Input.is_key_down("W") then
        direction.z = direction.z + 1
    end
    if Input.is_key_down("S") then
        direction.z = direction.z - 1
    end
    if Input.is_key_down("A") then
        direction.x = direction.x - 1
    end
    if Input.is_key_down("D") then
        direction.x = direction.x + 1
    end

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
