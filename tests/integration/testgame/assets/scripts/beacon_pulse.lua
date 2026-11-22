-- Маяк портала: пульсирует цветом и масштабом, чтобы портал читался в мире.
local t = 0

function OnUpdate(entity, dt)
    t = t + dt
    local pulse = 0.5 + 0.5 * math.sin(t * 3.0)
    entity.Color = Vec3(0.3 + 0.5 * pulse, 0.5, 1.0)
    local s = 0.9 + 0.15 * pulse
    entity.Transform.Scale = Vec3(s, entity.Transform.Scale.y, s)
end
