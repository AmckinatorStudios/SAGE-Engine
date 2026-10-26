#pragma once
#include <glm/glm.hpp>

#include "sage/physics/PhysicsComponents.h"

// ---------------------------------------------------------------------------
// ПРАВКА ФОРМЫ КОЛЛАЙДЕРА РУЧКАМИ — математика без ImGui и сцены.
//
// Как в Unity: у коробки по ручке на каждой из шести граней. Тянешь грань — она
// едет, а ПРОТИВОПОЛОЖНАЯ стоит на месте (размер и центр меняются вместе), с
// Alt — обе грани расходятся симметрично и центр не трогается. У шара ручки
// меняют радиус, у капсулы боковые — радиус, верх и низ — высоту.
//
// Всё в ЛОКАЛЬНЫХ единицах коллайдера (до масштаба объекта): ColliderComponent
// хранит размеры именно так, а масштаб объекта физика домножает сама. Поэтому
// мировое смещение ручки здесь сначала делится на масштаб своей оси — иначе
// коллайдер у растянутого родителя рос бы быстрее мыши.
// ---------------------------------------------------------------------------
namespace sage::editor::collideredit {

constexpr float kMinSize = 1e-3f;

// Ручка: ось (0..2) и сторона (+1/−1); ось −1 — центр формы.
struct Handle {
    int Axis = -1;
    int Side = 1;
};

// Положение ручки в осях объекта (до масштаба).
glm::vec3 HandleLocal(const ColliderComponent& col, const glm::vec3& scale, Handle h);

// Тянуть ручку h на worldDelta (мировые единицы вдоль её оси, к наружной
// стороне — положительно) от состояния start. symmetric — обе стороны сразу.
ColliderComponent DragHandle(const ColliderComponent& start, const glm::vec3& scale, Handle h, float worldDelta,
                             bool symmetric);

// Сдвинуть центр формы на смещение в осях объекта с масштабом (мировые единицы,
// уже повернутые в оси объекта).
ColliderComponent MoveCenter(const ColliderComponent& start, const glm::vec3& scale, const glm::vec3& delta);

// Есть ли у формы ручки (у форм по мешу размер задаёт модель).
bool Editable(const ColliderComponent& col);

} // namespace sage::editor::collideredit
