-- =============================================================================
--  09_trigger_zone_monitor.lua — зона-триггер: красится, пока в ней кто-то
--  есть, и пишет в консоль каждый вход и выход.
--
--  ЧТО ПРОВЕРЯЕТ. Обычные столкновения физики (сенсор всё ещё пересекается
--  с телами, просто не толкает их) — быстрая проверка на живой сцене, что
--  RigidBody/Collider вообще что-то сталкивают.
--
--  ЧТО НУЖНО, ЧТОБЫ ЗАРАБОТАЛО:
--   1. Повесьте на объект с RigidBody + Collider (форма — любая, обычно Box).
--   2. В инспекторе RigidBody поставьте галочку "Is Trigger" — без неё это
--      обычная стена, а не зона.
--   3. Уроните на зону/через неё что угодно с RigidBody (Dynamic) —
--      например, объекты из 06_mesh_collider_dropper.lua или
--      07_stress_crowd_dropper.lua, либо игрока из 01_fps_controller.lua
--      (с CharacterController зона тоже работает — её отдельно проверяет
--      сама сцена, а не бэкенд физики).
-- =============================================================================

local inside = 0

local function paint(entity)
    entity.Color = inside > 0 and Vec3(0.25, 0.9, 0.35) or Vec3(0.7, 0.55, 0.2)
end

function OnStart(entity)
    paint(entity)
    if entity:HasRigidBody() and not entity:GetRigidBody().Sensor then
        Debug.Log("[09_trigger_zone_monitor] ВНИМАНИЕ: у " .. entity.Name ..
                  " не стоит галочка 'Is Trigger' в RigidBody — сейчас это стена, а не зона.")
    end
end

function OnTriggerEnter(entity, other)
    inside = inside + 1
    paint(entity)
    Debug.Log("[09_trigger_zone_monitor] вошёл: " .. other.name .. " (внутри сейчас: " .. inside .. ")")
end

function OnTriggerExit(entity, other)
    inside = math.max(0, inside - 1)
    paint(entity)
    if other then
        Debug.Log("[09_trigger_zone_monitor] вышел: " .. other.name .. " (внутри сейчас: " .. inside .. ")")
    else
        Debug.Log("[09_trigger_zone_monitor] гость был удалён прямо в зоне (внутри сейчас: " .. inside .. ")")
    end
end
