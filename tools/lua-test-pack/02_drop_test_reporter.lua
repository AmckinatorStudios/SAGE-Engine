-- =============================================================================
--  02_drop_test_reporter.lua — репортёр падения: следит за одним падающим
--  телом и печатает в консоль, ушло ли оно в пол при касании и на сколько.
--
--  ЧТО ПРОВЕРЯЕТ. Ровно ту проблему, ради которой чинилась физика: раньше тело
--  при падении заметно уходило в поверхность, а потом «всплывало» обратно
--  несколько кадров подряд. Скрипт печатает НИЗШУЮ точку пути и итоговую
--  высоту покоя — если разница большая, тело провалилось.
--
--  ЧТО НУЖНО, ЧТОБЫ ЗАРАБОТАЛО:
--   1. Повесьте на объект, у которого УЖЕ ЕСТЬ RigidBody (Dynamic) и Collider
--      любой формы (Box/Sphere/Capsule/Convex Hull/Mesh) — их проще всего
--      добавить через меню Object -> Physics Cube (или Physics Sphere) в
--      верхней панели редактора, либо руками через инспектор (Add Component
--      -> Rigid Body, Add Component -> Collider).
--   2. Поднимите объект над полом (или бросьте его туда, где он свалится
--      с чего-то) и нажмите Play.
--   3. Под ним должен быть пол — объект с RigidBody (Static) + Collider.
--
--  Скрипту не нужны никакие настройки и ссылки на другие объекты — он
--  наблюдает только за своим собственным объектом.
-- =============================================================================

report_interval = 0.5   -- как часто печатать промежуточное состояние, секунды

local startY
local lowestY
local elapsed = 0.0
local reportTimer = 0.0
local settled = false

function OnStart(entity)
    startY = entity.Transform.Position.y
    lowestY = startY
    Debug.Log(string.format("[02_drop_test] %s: старт на высоте y=%.3f", entity.Name, startY))
    if not entity:HasRigidBody() then
        Debug.Log("[02_drop_test] ВНИМАНИЕ: у объекта нет RigidBody — добавьте его через инспектор " ..
                  "(см. инструкцию в начале файла), иначе падать нечему.")
    end
end

function OnUpdate(entity, dt)
    if settled then return end
    elapsed = elapsed + dt

    local p = entity.Transform.Position
    if p.y < lowestY then lowestY = p.y end

    reportTimer = reportTimer + dt
    if reportTimer >= report_interval then
        reportTimer = 0.0
        local speed = GetVelocity(entity):length()
        Debug.Log(string.format("[02_drop_test] %s: t=%.1fс y=%.3f скорость=%.3f",
            entity.Name, elapsed, p.y, speed))
    end

    -- IsSleeping — тело улеглось и физика перестала его считать. Это и есть
    -- «упало и успокоилось»: дальше можно смотреть итог.
    if IsSleeping(entity) then
        settled = true
        Debug.Log(string.format(
            "[02_drop_test] %s: УСНУЛО через %.2fс. Низшая точка пути y=%.3f, конечная высота y=%.3f.\n" ..
            "  Если конечная высота ниже видимой поверхности пола (или сильно ниже низшей точки пути,\n" ..
            "  то есть тело потом ВСПЛЫЛО) — контакт работает неправильно.",
            entity.Name, elapsed, lowestY, p.y))
    end
end
