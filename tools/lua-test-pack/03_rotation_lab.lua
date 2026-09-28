-- =============================================================================
--  03_rotation_lab.lua — лаборатория вращения: клавишами дёргает все новые
--  функции вращения физики на одном теле.
--
--  ЧТО ПРОВЕРЯЕТ: AddTorque (момент), AddImpulseAtPoint (удар в точку — должен
--  закручивать), AddImpulse (удар в центр — НЕ должен закручивать),
--  AddAngularImpulse (мгновенная закрутка), SetAngularVelocity (скорость
--  вращения напрямую), заморозку осей (RigidBody.Locks) и сон тела.
--
--  ЧТО НУЖНО, ЧТОБЫ ЗАРАБОТАЛО:
--   1. Повесьте на объект с RigidBody (Dynamic) + Collider (лучше всего Box —
--      удобно видеть вращение по граням). Через меню Object -> Physics Cube.
--   2. Поставьте half_size ниже РАВНЫМ половине размера коллайдера (по
--      умолчанию у Physics Cube это 0.5 — тогда ничего менять не нужно).
--   3. Объект может висеть в воздухе или стоять на полу — оба случая
--      показательны (в воздухе видно чистое вращение, на полу — как трение
--      останавливает качение).
--
--  УПРАВЛЕНИЕ:
--   T — держать: постоянный момент вокруг Y (как мотор)
--   Y — держать: постоянный момент вокруг X
--   I — толчок В УГОЛ верхней грани: тело обязано закрутиться
--   O — толчок В ЦЕНТР: тело обязано полететь БЕЗ вращения
--   U — мгновенная угловая закрутка (без сдвига)
--   P — задать угловую скорость напрямую (3 рад/с вокруг Y)
--   1..6 — переключить заморозку осей: 1/2/3 — позиция X/Y/Z, 4/5/6 — поворот X/Y/Z
-- =============================================================================

torque_strength = 6.0
half_size = 0.5   -- половина размера тела; должна совпадать с Half Extents коллайдера

local reportTimer = 0.0
local axisNames = {"позиция X", "позиция Y", "позиция Z", "поворот X", "поворот Y", "поворот Z"}

function OnStart(entity)
    Debug.Log("[03_rotation_lab] " .. entity.Name .. " готов. T/Y — момент, I — толчок в угол " ..
              "(закрутит), O — толчок в центр (не закрутит), U — угловая закрутка, " ..
              "P — задать угловую скорость, 1-6 — заморозить оси.")
    if not entity:HasRigidBody() then
        Debug.Log("[03_rotation_lab] ВНИМАНИЕ: у объекта нет RigidBody — добавьте через инспектор.")
    end
end

function OnUpdate(entity, dt)
    if Input.IsKeyDown("T") then AddTorque(entity, Vec3(0.0, torque_strength, 0.0)) end
    if Input.IsKeyDown("Y") then AddTorque(entity, Vec3(torque_strength, 0.0, 0.0)) end

    if Input.IsKeyPressed("I") then
        local p = entity.Transform.Position
        AddImpulseAtPoint(entity, Vec3(0.0, 0.0, 4.0),
                          Vec3(p.x + half_size, p.y + half_size, p.z))
    end
    if Input.IsKeyPressed("O") then
        AddImpulse(entity, Vec3(0.0, 0.0, 4.0))
    end
    if Input.IsKeyPressed("U") then
        AddAngularImpulse(entity, Vec3(0.0, 2.0, 0.0))
    end
    if Input.IsKeyPressed("P") then
        SetAngularVelocity(entity, Vec3(0.0, 3.0, 0.0))
    end

    local rb = entity:GetRigidBody()
    if rb then
        for i = 1, 6 do
            if Input.IsKeyPressed(tostring(i)) then
                local bit = 1 << (i - 1)
                if (rb.Locks & bit) ~= 0 then
                    rb.Locks = rb.Locks & ~bit
                    Debug.Log("[03_rotation_lab] снята заморозка: " .. axisNames[i])
                else
                    rb.Locks = rb.Locks | bit
                    Debug.Log("[03_rotation_lab] заморожено: " .. axisNames[i])
                end
            end
        end
    end

    reportTimer = reportTimer + dt
    if reportTimer >= 1.0 then
        reportTimer = 0.0
        local w = GetAngularVelocity(entity):length()
        Debug.Log(string.format("[03_rotation_lab] %s: |угловая скорость|=%.2f рад/с, спит=%s",
            entity.Name, w, tostring(IsSleeping(entity))))
    end
end
