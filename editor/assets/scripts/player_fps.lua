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

    -- Камера взгляда. Пусто — берётся дочерняя камера, а если камера висит на
    -- САМОМ персонаже, для неё заводится дочерний «взгляд» (см. FindEye).
    Camera   = field.entity(),
    -- Высота глаз над точкой объекта — для «взгляда», созданного автоматически.
    EyeHeight = field.number(0.7, -2.0, 3.0),
    Animator = field.component("Animation"),
    Character = field.component("CharacterController"),
}

-- НАКЛОНЯЕТСЯ НЕ ТЕЛО, А ВЗГЛЯД. Тело крутится только вокруг Y (рыскание),
-- вверх-вниз смотрит дочерний объект с камерой. Камера на самом теле означала
-- две беды разом: при взгляде вниз заваливалась вся капсула, а наклон, выданный
-- тому же объекту, что и рыскание, стирал рыскание (SetLocalRotation(pitch, 0,
-- 0)) — голова «крутилась только по X». Такую камеру переносим на дочерний
-- объект «взгляд» вместе со всеми её настройками.
function Player:FindEye()
    local me = self.gameObject
    if self.Camera and self.Camera.id ~= me.id then return self.Camera end
    for _, child in ipairs(me.children) do
        if child:HasComponent("Camera") then return child end
    end
    if me:HasComponent("Camera") then
        local eye = Scene:Create(me.name .. " (взгляд)")
        eye:SetParent(me)
        eye.transform.localPosition = Vector3(0, self.EyeHeight, 0)
        me:MoveComponent("Camera", eye)
        return eye
    end
    return nil
end

function Player:Start()
    self.pitch = 0
    -- Рыскание считаем сами и ставим телу целиком: (0, yaw, 0). Наклон X/Z,
    -- оставшийся от гизмо, иначе копился бы в теле и давал крен взгляду.
    self.yaw = self.transform.localRotation.y
    self.Eye = self:FindEye()

    -- Свои умолчания раскладки. Без них игра не управляется ничем сразу после
    -- создания объекта — а это первое, что делают, проверяя персонажа.
    Input:BindVector("Move", "W", "S", "A", "D")
    Input:BindAction("Sprint", "LEFT_SHIFT")
    Input:BindAction("Jump", "SPACE")

    -- Контроллер можно не расставлять мышью: если поле пустое, берём свой.
    self.Character = self.Character or self:GetCharacterController()

    -- Курсор — одно поведение, без настроек: захвачен в игре, ESC отпускает,
    -- щелчок по окну захватывает снова. Обзор крутится только в захвате —
    -- иначе камера вертелась бы от наведения мыши на кнопку.
    Input:SetCursorCaptured(true)
end

function Player:Update(dt)
    -- --- Курсор --------------------------------------------------------
    if Input:IsCursorCaptured() then
        if Input:IsKeyPressed("ESCAPE") then Input:SetCursorCaptured(false) end
    elseif Input:IsMouseButtonPressed("MOUSE_LEFT") then
        Input:SetCursorCaptured(true)
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
    if Input:IsCursorCaptured() then
        local look = Input:GetMouseDelta()
        local lookY = self.InvertLookY and -look.y or look.y

        self.yaw = self.yaw - look.x * self.LookSensitivity

        self.pitch = self.pitch + lookY * self.LookSensitivity
        self.pitch = math.max(-89, math.min(89, self.pitch))
    end

    self.transform:SetLocalRotation(0, self.yaw, 0)
    if self.Eye then
        self.Eye.transform:SetLocalRotation(self.pitch, 0, 0)
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
