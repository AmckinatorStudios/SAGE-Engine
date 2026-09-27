#include "sage/ecs/DayNightCycle.h"

#include <cmath>

#include "sage/ecs/LightSystem.h"
#include "sage/scene/Scene.h"

namespace sage::ecs {

glm::vec3 SunDirectionAt(const DayNightCycle& cycle) {
    // Солнце идёт по большому кругу: восток (откуда встаёт) -> зенит под углом
    // NoonElevation -> запад. Угол по кругу: 0 в 6 часов (восход), 90° в
    // полдень, 180° на закате, 270° в полночь — ровно под горизонтом.
    const float angle = glm::radians((cycle.Time - 6.0f) / 24.0f * 360.0f);
    const float az = glm::radians(cycle.SunAzimuth);
    const float noon = glm::radians(glm::clamp(cycle.NoonElevation, 1.0f, 90.0f));
    const glm::vec3 east(std::sin(az), 0.0f, std::cos(az));
    // «Юг» — горизонтальное направление, куда наклонён круг: перпендикуляр к
    // востоку. Высота в полдень задаётся долей вертикали в этой оси.
    const glm::vec3 south(east.z, 0.0f, -east.x);
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    const glm::vec3 toSun = std::cos(angle) * east +
                            std::sin(angle) * (std::sin(noon) * up + std::cos(noon) * south);
    return -glm::normalize(toSun);
}

void AdvanceDayNight(DayNightCycle& cycle, float dt) {
    if (dt <= 0.0f || cycle.Speed == 0.0f) return;
    float t = cycle.Time + dt * cycle.HoursPerSecond() * cycle.Speed;
    // По кругу в обе стороны: отрицательная скорость — время назад.
    t = std::fmod(t, 24.0f);
    if (t < 0.0f) t += 24.0f;
    cycle.Time = t;
}

bool ApplyDayNight(Scene& scene) {
    DayNightCycle& cycle = scene.Lighting.Cycle;
    if (!cycle.Enabled) return false;
    const glm::vec3 dir = SunDirectionAt(cycle);
    const entt::entity sun = FindSunEntity(scene);
    if (sun != entt::null) {
        // Пишется только при изменении: сцена, переписанная тем же значением
        // каждый кадр, выглядела бы «изменённой» для всех, кто это отслеживает.
        glm::vec3& rot = scene.Registry().get<Transform>(sun).Rotation;
        const glm::vec3 want = EulerFromForward(dir);
        if (glm::any(glm::greaterThan(glm::abs(rot - want), glm::vec3(1e-4f)))) rot = want;
    }
    // Солнце окружения — всегда: без объекта-солнца светит оно.
    scene.Lighting.Sun.Direction = dir;
    return true;
}

void UpdateDayNight(Scene& scene, float dt) {
    DayNightCycle& cycle = scene.Lighting.Cycle;
    if (!cycle.Enabled) return;
    if (cycle.RunInPlay) AdvanceDayNight(cycle, dt);
    ApplyDayNight(scene);
}

} // namespace sage::ecs
