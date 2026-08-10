#pragma once
#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "sage/physics/PhysicsTypes.h"

// ---------------------------------------------------------------------------
// Узкая фаза встроенного движка: две выпуклые формы -> манифольд контактов
// (нормаль + до восьми точек с глубиной).
//
// Манифольд, а не одна точка: с одной точкой ящик на полу качается, а стопка
// не стоит. Плоскость на плоскости даёт несколько точек отсечением грани по
// грани (Sutherland-Hodgman).
//
// Коробка — честный OBB: разделяющая ось ищется по всем пятнадцати кандидатам
// (три грани каждой + девять рёберных произведений). С осевым габаритом
// повёрнутый на 45° ящик висел бы над полом на треть диагонали.
//
// Точки возвращаются и с положительным зазором (до margin) — спекулятивные
// контакты. Решатель по зазору ограничивает скорость сближения и тормозит тело
// у поверхности; это снимает туннелирование без отдельного прохода.
// ---------------------------------------------------------------------------
namespace sage::physics::builtin {

// Поза формы в мире. Отдельным типом, а не парой аргументов: пара
// «позиция+поворот» ходит по всей узкой фазе, и перепутать их порядок в
// вызове из шести аргументов — вопрос времени.
struct Pose {
    glm::vec3 Position{0.0f};
    glm::quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};

    glm::vec3 ToWorld(const glm::vec3& local) const { return Position + Rotation * local; }
    glm::vec3 ToLocal(const glm::vec3& world) const {
        return glm::conjugate(Rotation) * (world - Position);
    }
    glm::vec3 DirToLocal(const glm::vec3& world) const { return glm::conjugate(Rotation) * world; }
};

// Одна выпуклая часть тела в ЛОКАЛЬНЫХ координатах тела. Составное тело —
// набор таких частей; каждая сталкивается сама по себе.
struct Part {
    ShapeType Shape = ShapeType::Box;
    glm::vec3 Half{0.5f, 0.5f, 0.5f};  // Box: полуразмеры
    float Radius = 0.5f;               // Sphere/Capsule
    float HalfHeight = 0.5f;           // Capsule: половина ЦИЛИНДРИЧЕСКОЙ части (без шапок)
    glm::vec3 Position{0.0f};          // смещение внутри тела
    glm::quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
};

// Точка контакта.
struct ContactPoint {
    glm::vec3 Position{0.0f};  // мировая точка на поверхности тела B
    float Separation = 0.0f;   // < 0 — проникновение, > 0 — спекулятивный зазор

    // Устойчивый между кадрами номер точки: пара «какая грань какой формы».
    // Нужен ТЁПЛОМУ СТАРТУ — переносу накопленного импульса на следующий кадр.
    // Без переноса решателю приходится каждый кадр заново нащупывать силу,
    // держащую стопку ящиков, и нижний ящик заметно проседает под верхними.
    uint32_t Id = 0;

    // Скорость сближения В МОМЕНТ ЗАВЕДЕНИЯ контакта. Упругость обязана
    // считаться от неё, а не от текущей: к тому времени, когда до отскока
    // доходит дело, решатель эту скорость уже погасил, и мяч, ударившийся о
    // пол на восьми метрах в секунду, отскакивал бы «от нуля».
    float RelativeVelocity = 0.0f;

    // Накопленные импульсы (заполняет решатель, узкая фаза их не трогает).
    float NormalImpulse = 0.0f;
    float TangentImpulse[2] = {0.0f, 0.0f};
};

struct Manifold {
    glm::vec3 Normal{0.0f, 1.0f, 0.0f};  // единичная, ОТ A К B
    int Count = 0;
    ContactPoint Points[8];
};

// Сталкивает две части. margin — до какого положительного зазора ещё
// возвращать точки (спекулятивные контакты). Возвращает false, если формы
// дальше margin друг от друга.
bool Collide(const Part& a, const Pose& poseA, const Part& b, const Pose& poseB, float margin,
             Manifold& out);

// Габарит части в мировых осях (для широкой фазы и запросов).
void PartAabb(const Part& p, const Pose& pose, glm::vec3& min, glm::vec3& max);

// Луч против одной части. Возвращает расстояние до входа и нормаль.
bool RaycastPart(const Part& p, const Pose& pose, const glm::vec3& origin, const glm::vec3& dir,
                 float maxDistance, float& outDistance, glm::vec3& outNormal);

// Пересекается ли часть со сферой (запрос «кто рядом»).
bool OverlapSpherePart(const Part& p, const Pose& pose, const glm::vec3& center, float radius);

// Тензор инерции части относительно её собственного центра при единичной
// массе, и сам центр в координатах тела. Решателю нужна честная инерция: с
// шаровым приближением длинная доска крутится вокруг длинной оси так же
// тяжело, как вокруг короткой, и всякое вращение выглядит ватным.
void PartInertiaUnitMass(const Part& p, glm::mat3& outInertia, glm::vec3& outCenter,
                         float& outVolume);

} // namespace sage::physics::builtin
