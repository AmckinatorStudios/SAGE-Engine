#pragma once
#include <cmath>

#include "physics/builtin/BuiltinWorld.h"

// Постоянные и помощники встроенной физики, общие для шага (BuiltinWorld.cpp)
// и решателя (BuiltinWorld_Solver.cpp): разойдись допуски у двух файлов —
// контакт заводился бы по одному запасу, а гасился по другому.
namespace sage::physics::builtin_detail {

using builtin::Manifold;
using builtin::Part;
using builtin::Pose;

// Фиксированный шаг. Физика с переменным шагом невоспроизводима: одна и та же
// сцена на разных машинах расходится, а на просадке кадра пружины и стопки
// взрываются. Кадр разбивается на целое число таких шагов, остаток
// переносится на следующий кадр.
constexpr float kFixedStep = 1.0f / 120.0f;
constexpr int kMaxSubSteps = 8;   // потолок «спирали смерти» на тяжёлом кадре

// Итерации решателя. Десять — обычный компромисс между устойчивостью стопки и
// ценой шага; меньше — стопка проседает, больше — платим за незаметное.
constexpr int kVelocityIterations = 10;
constexpr int kRelaxIterations = 3;

// Допуск проникновения. Выталкивать контакт до нуля нельзя: тела начинают
// подрагивать, потому что решатель каждый кадр отталкивает их чуть сильнее,
// чем нужно, и они отскакивают обратно в контакт.
constexpr float kSlop = 0.005f;
constexpr float kBaumgarte = 0.2f;            // доля ошибки, исправляемая за шаг
constexpr float kMaxCorrectionSpeed = 3.0f;   // м/с — потолок выталкивания

// Спекулятивный запас: на сколько вперёд заводить контакт до касания.
constexpr float kSpeculativeMargin = 0.04f;

// Упругость ниже этого порога сближения не применяется — иначе лежащее тело
// бесконечно мелко подпрыгивает на собственной упругости.
constexpr float kRestitutionThreshold = 1.0f;

constexpr float kSleepLinear = 0.06f;    // м/с
constexpr float kSleepAngular = 0.12f;   // рад/с
constexpr float kSleepDelay = 0.5f;      // с покоя до засыпания

constexpr float kEps = 1e-8f;

inline builtin::Part PartFromChild(const ChildShape& c) {
    Part p;
    p.Shape = c.Shape;
    p.Half = c.HalfExtents;
    p.Radius = c.Radius;
    p.HalfHeight = c.HalfHeight;
    p.Position = c.Position;
    p.Rotation = c.Rotation;
    return p;
}

// Две касательные к нормали. Ортонормированный базис строим от той оси, что
// наименее сонаправлена с нормалью, — иначе на вертикальной нормали
// касательные вырождаются в ноль и трение исчезает ровно там, где оно нужнее
// всего (на полу).
inline void TangentBasis(const glm::vec3& n, glm::vec3& t1, glm::vec3& t2) {
    if (std::abs(n.x) >= 0.57735f) t1 = glm::normalize(glm::vec3(n.y, -n.x, 0.0f));
    else t1 = glm::normalize(glm::vec3(0.0f, n.z, -n.y));
    t2 = glm::cross(n, t1);
}

inline glm::mat3 Skew(const glm::vec3& v) {
    return glm::mat3(0.0f, v.z, -v.y, -v.z, 0.0f, v.x, v.y, -v.x, 0.0f);
}

} // namespace sage::physics::builtin_detail
