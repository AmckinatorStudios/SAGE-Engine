#pragma once
#include <glm/glm.hpp>

class Scene;
struct DayNightCycle;

// ---------------------------------------------------------------------------
// ЦИКЛ ДНЯ И НОЧИ В ДЕЙСТВИИ (данные — DayNightCycle в sage/scene/Light.h).
//
// Время суток превращается в направление солнца, а направление — в поворот
// ОБЪЕКТА-солнца: светит в кадре именно он (см. CollectLighting), и гизмо в
// редакторе обязано показывать то же, что видит игрок. Нет объекта-солнца —
// правится солнце окружения сцены.
// ---------------------------------------------------------------------------
namespace sage::ecs {

// Куда светит солнце в момент cycle.Time (направление полёта света, как у
// DirectionalLight::Direction). В 12 часов солнце на высоте NoonElevation, в
// 6 и 18 — на горизонте, в полночь — под ним.
glm::vec3 SunDirectionAt(const DayNightCycle& cycle);

// Сдвинуть время на dt секунд (с учётом длины суток и скорости), по кругу 0..24.
void AdvanceDayNight(DayNightCycle& cycle, float dt);

// Поставить солнце сцены по времени цикла. false — цикл выключен.
bool ApplyDayNight(Scene& scene);

// Кадр игры: время идёт (если RunInPlay) и солнце встаёт на место.
void UpdateDayNight(Scene& scene, float dt);

} // namespace sage::ecs
