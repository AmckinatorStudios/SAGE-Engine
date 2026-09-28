-- =============================================================================
--  01_fps_controller.lua — ходьба от первого лица через КОНТРОЛЛЕР ПЕРСОНАЖА
--  движка (CharacterControllerComponent), с прыжком и осмотром мышью.
--
--  ЧТО ПРОВЕРЯЕТ. Реальный контроллер персонажа движка (не самодельная физика):
--  тяготение, прыжок, опору и ступеньку ведёт сам движок (PhysicsScene ->
--  Jolt), а скрипт только просит «иди туда» и «прыгни». Это заодно проверяет
--  саму физику мира — контроллер сталкивается с НАСТОЯЩИМИ RigidBody/Collider
--  сцены, в том числе с новыми Mesh/Convex Hull коллайдерами.
--
--  ЧТО НУЖНО, ЧТОБЫ ЗАРАБОТАЛО:
--   1. Повесьте этот скрипт на объект-игрока (обычная пустышка, не нужен ни
--      RigidBody, ни Collider — контроллер персонажа отдельная система).
--      Component "Character Controller" скрипт добавит сам в Start(), но
--      его размеры (Radius/Height/Slope Limit/Step Offset) можно поправить в
--      инспекторе уже после первого запуска Play.
--   2. Создайте ДОЧЕРНИЙ объект-камеру: выделите игрока, в Hierarchy — Add
--      Object -> Camera (или меню Object -> Camera), проверьте, что камера
--      стала ребёнком игрока, поставьте ей высоту глаз, например (0, 1.6, 0).
--   3. В инспекторе ЭТОГО скрипта (секция Script) перетащите созданную
--      камеру в поле "Camera Object" — без этого скрипт откажется стартовать
--      и напишет об этом в консоль.
--   4. В сцене должен быть пол и препятствия — объекты с RigidBody
--      (Static) + Collider (подойдёт и обычный Box, и новый Mesh/Convex Hull
--      коллайдер по мешу пола/стен — см. docs/physics.md, "Коллайдер по мешу").
--      Без пола персонаж будет вечно падать — это ожидаемо и тоже проверка
--      (тяготение работает).
--
--  УПРАВЛЕНИЕ: WASD — ходьба, Shift — бег, Space — прыжок (только стоя на
--  земле), мышь — осмотр, Escape — освободить/захватить курсор мыши.
-- =============================================================================

CameraObject = field.entity()

walk_speed = 5.0
run_speed = 9.0
jump_height = 1.2          -- метров; высота прыжка, а не скорость прыжка
look_sensitivity = 0.12    -- градусов на пиксель движения мыши

local yaw = 0.0
local pitch = 0.0

function Start()
    if not CameraObject then
        error("01_fps_controller: перетащите дочернюю камеру в поле 'Camera Object' в инспекторе " ..
              "(см. инструкцию в начале файла скрипта)")
    end

    -- Контроллер персонажа заводится сам — тестеру не нужно искать его в
    -- списке компонентов, но настройки (радиус/рост/ступенька) видны и
    -- правятся в инспекторе как обычно.
    self.character = self.gameObject:AddComponent("CharacterController")
    -- Камера, добавленная кодом, не главная по умолчанию — делаем её главной,
    -- иначе кадр рисует не она, а любая случайная камера сцены.
    CameraObject:AddComponent("Camera", {primary = true})

    Input.SetCursorCaptured(true)
    Debug.Log("01_fps_controller: старт. WASD — ходьба, Shift — бег, Space — прыжок, " ..
              "мышь — осмотр, Escape — курсор.")
end

function Update(dt)
    -- Start() уже написал о причине в консоль (нет камеры) — не спамить ещё и
    -- ошибкой на каждый кадр поверх неё.
    if not self.character then return end

    if Input.IsKeyPressed("Escape") then
        Input.SetCursorCaptured(not Input.IsCursorCaptured())
    end

    -- Осмотр мышью — только пока курсор захвачен: иначе игрок крутит камеру,
    -- просто наводя мышь на кнопку интерфейса.
    if Input.IsCursorCaptured() then
        local delta = Input.GetMouseDelta()
        yaw = yaw - delta.x * look_sensitivity
        pitch = pitch - delta.y * look_sensitivity
        if pitch > 89.0 then pitch = 89.0 end
        if pitch < -89.0 then pitch = -89.0 end
        -- Поворот ТЕЛА — только по рысканию (yaw): тангаж (pitch) — только
        -- у камеры. Если наклонять тело целиком, направление ходьбы вверх-вниз
        -- «подныривало» бы при взгляде вниз — известная ошибка самодельных
        -- контроллеров.
        self.transform.rotation.y = yaw
        CameraObject.transform.rotation.x = pitch
    end

    -- Желаемое направление — из ТЕКУЩЕГО поворота тела (уже только рыскание,
    -- поэтому Forward()/Right() лежат строго в горизонтальной плоскости).
    local moveForward = 0.0
    local moveRight = 0.0
    if Input.IsKeyDown("W") then moveForward = moveForward + 1.0 end
    if Input.IsKeyDown("S") then moveForward = moveForward - 1.0 end
    if Input.IsKeyDown("D") then moveRight = moveRight + 1.0 end
    if Input.IsKeyDown("A") then moveRight = moveRight - 1.0 end

    local forward = self.transform:Forward()
    local right = self.transform:Right()
    local wish = Vector3.new(forward.x * moveForward + right.x * moveRight,
                             0.0,
                             forward.z * moveForward + right.z * moveRight)
    local len = wish:length()
    if len > 0.001 then wish = wish / len end

    local speed = Input.IsKeyDown("LeftShift") and run_speed or walk_speed
    -- Move() — это СКОРОСТЬ, вертикаль (тяготение, прыжок, ступеньку, склон)
    -- ведёт сам контроллер: тут нет ни одной строчки самодельной физики.
    self.character:Move(wish * speed)

    if Input.IsKeyPressed("Space") and self.character:IsGrounded() then
        self.character:Jump(jump_height)
    end
end
