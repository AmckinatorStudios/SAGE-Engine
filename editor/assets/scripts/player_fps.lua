-- Пример: персонаж от первого лица целиком на скрипте.
--
-- Ни одного игрового понятия в движке при этом нет: он знает Transform, Input,
-- CharacterController, Animation и Camera. Что «Move» значит ходьбу, а
-- «Sprint» — бег, решает ЭТОТ файл.
--
-- Действия объявляются здесь же (Input:BindVector / BindAction) — раскладка
-- принадлежит игре, а не движку. Настройки проекта, если они есть, её
-- перекрывают.
--
-- ЗНАКИ, на которых всё стоит (см. docs/scripting.md, «Направления»):
--   * transform:Forward() — куда смотрит камера (локальный −Z);
--   * GetVector2("Move"): y > 0 — вперёд, x > 0 — вправо;
--   * GetMouseDelta(): x > 0 — мышь вправо, y > 0 — мышь ОТ СЕБЯ (вверх).
-- Поворот вокруг Y положительным углом — ВЛЕВО (правая тройка осей), поэтому
-- мышь вправо уменьшает рыскание. Наклон вверх — положительный угол X.

local Player = {}

Player.public = {
    MoveSpeed       = field.number(5.0, 0.0, 20.0),
    SprintSpeed     = field.number(8.0, 0.0, 30.0),
    JumpHeight      = field.number(1.5, 0.0, 10.0),
    LookSensitivity = field.number(0.1, 0.001, 2.0),
    InvertLookY     = field.boolean(false),

    -- --- Курсор ----------------------------------------------------------
    -- Захватить мышь при старте: курсор прячется и не упирается в край окна,
    -- обзор крутится без предела. Выключают для персонажа, которым правят
    -- мышью по интерфейсу (стратегия, меню поверх игры).
    LockCursor      = field.boolean(true),
    -- Клавиша, которая отпускает и снова захватывает курсор. Пусто — нет такой.
    CursorToggleKey = field.string("ESCAPE"),
    -- Крутить обзор только пока курсор захвачен: иначе камера вертится от
    -- простого наведения мыши на кнопку.
    LookOnlyWhenLocked = field.boolean(true),

    Camera   = field.entity(),
    Animator = field.component("Animation"),
    Character = field.component("CharacterController"),
}

function Player:Start()
    self.pitch = 0

    -- Свои умолчания раскладки. Без них игра не управляется ничем сразу после
    -- создания объекта — а это первое, что делают, проверяя персонажа.
    Input:BindVector("Move", "W", "S", "A", "D")
    Input:BindAction("Sprint", "LEFT_SHIFT")
    Input:BindAction("Jump", "SPACE")

    -- Контроллер можно не расставлять мышью: если поле пустое, берём свой.
    self.Character = self.Character or self:GetCharacterController()

    if self.LockCursor then Input:SetCursorCaptured(true) end
end

function Player:Update(dt)
    -- --- Курсор --------------------------------------------------------
    if self.CursorToggleKey ~= "" and Input:IsKeyPressed(self.CursorToggleKey) then
        Input:SetCursorCaptured(not Input:IsCursorCaptured())
    end

    local character = self.Character
    if not character then return end

    -- --- Ходьба --------------------------------------------------------
    local move = Input:GetVector2("Move")

    local forward = self.transform:Forward()
    local right = self.transform:Right()
    forward.y = 0
    right.y = 0
    forward = Vector3.Normalize(forward)
    right = Vector3.Normalize(right)

    local direction = right * move.x + forward * move.y
    if Vector3.Length(direction) > 1 then
        direction = Vector3.Normalize(direction)
    end

    local speed = self.MoveSpeed
    if Input:IsActionDown("Sprint") then
        speed = self.SprintSpeed
    end

    -- Move задаёт СКОРОСТЬ, а тяготение, опору, склон и ступеньку ведёт сам
    -- контроллер: своей физики персонажа скрипт не пишет.
    character:Move(direction * speed)

    if character:IsGrounded() and Input:IsActionPressed("Jump") then
        character:Jump(self.JumpHeight)
    end

    -- --- Обзор ---------------------------------------------------------
    if not self.LookOnlyWhenLocked or Input:IsCursorCaptured() then
        local look = Input:GetMouseDelta()
        local lookY = self.InvertLookY and -look.y or look.y

        self.transform:Rotate(0, -look.x * self.LookSensitivity, 0)

        self.pitch = self.pitch + lookY * self.LookSensitivity
        self.pitch = math.max(-89, math.min(89, self.pitch))
    end

    if self.Camera then
        self.Camera.transform:SetLocalRotation(self.pitch, 0, 0)
    end

    -- --- Анимация ------------------------------------------------------
    local velocity = character:GetVelocity()

    if self.Animator then
        self.Animator:SetFloat("Speed",
            Vector3.Length(Vector3(velocity.x, 0, velocity.z)))
        self.Animator:SetBool("IsGrounded", character:IsGrounded())
    end
end

return Player
