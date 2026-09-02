-- ---------------------------------------------------------------------------
-- zones.lua — список участков и то, на каком из них сейчас стоит игрок.
--
-- ДОГОВОР УЧАСТКА. Каждый модуль-участок обязан объявить name, about и center,
-- и уметь Build(). Остальное необязательно и берётся, если есть:
--
--   Update(dt, ctx)  — раз в кадр, пока участок ЖИВОЙ (см. ниже)
--   Use()            — действие по E, возвращает строку для экрана
--   Reset()          — вернуть в исходное состояние по R
--   Enter() / Leave()— вход и выход игрока: тут правят общие настройки сцены
--   Solid(bbox)      — твердь участка для контроллера персонажа
--
-- ПОЧЕМУ ОБНОВЛЯЮТСЯ ВСЕ, А НЕ ТОЛЬКО ТЕКУЩИЙ. Огонь, который загорается,
-- когда на него посмотрели, и стопка, которая падает только вблизи, — это
-- витрина, которая работает, пока на неё смотрят, то есть враньё. Участков
-- восемь, и работы в их Update на порядки меньше, чем в одном кадре рендера.
--
-- ТЕКУЩИЙ УЧАСТОК — по расстоянию до центра, а не по вхождению в коробку:
-- у участков нет границ на полу, и «войти» в них некуда. Радиус с ЗАПАСОМ на
-- выход (гистерезис): без него на границе название на экране мигало бы каждый
-- шаг.
-- ---------------------------------------------------------------------------
local Zones = {}

local list = {}
local current = nil

local ENTER_RADIUS = 15.0
local LEAVE_RADIUS = 19.0

function Zones.Load()
    list = {
        require "zone_physics",
        require "zone_material",
        require "zone_particles",
        require "zone_lights",
        require "zone_anim",
        require "zone_sky",
        require "zone_script",
        require "zone_texture",
    }
    return list
end

function Zones.All() return list end

function Zones.Build()
    for _, z in ipairs(list) do z.Build() end
    return #list
end

function Zones.Current() return current end

-- Твердь всех участков разом: контроллер персонажа спрашивает одну функцию, а
-- складывать ответы восьми участков — дело этого модуля, а не игрока.
function Zones.Solid(minX, minY, minZ, maxX, maxY, maxZ)
    for _, z in ipairs(list) do
        if z.Solid and z.Solid(minX, minY, minZ, maxX, maxY, maxZ) then return true end
    end
    return false
end

function Zones.Update(dt, ctx)
    for _, z in ipairs(list) do
        if z.Update then z.Update(dt, ctx) end
    end

    -- Кто ближе всех и достаточно близко.
    local px, pz = ctx.playerPos.x, ctx.playerPos.z
    local best, bestDist = nil, math.huge
    for _, z in ipairs(list) do
        local dx, dz = px - z.center.x, pz - z.center.z
        local d = math.sqrt(dx * dx + dz * dz)
        if d < bestDist then best, bestDist = z, d end
    end

    local want = current
    if best and bestDist <= ENTER_RADIUS then
        want = best
    elseif current then
        local dx, dz = px - current.center.x, pz - current.center.z
        if math.sqrt(dx * dx + dz * dz) > LEAVE_RADIUS then want = nil end
    end

    if want ~= current then
        if current and current.Leave then current.Leave() end
        current = want
        if current and current.Enter then current.Enter() end
        return true      -- участок сменился: вызывающий обновит экран
    end
    return false
end

return Zones
