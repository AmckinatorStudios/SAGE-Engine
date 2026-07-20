#pragma once
#include <cstdint>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

// ---------------------------------------------------------------------------
// Общие типы физической подсистемы — бэкенд-независимые (как sage/rhi для
// графики). Движок и игры описывают тела через эти структуры и обращаются к
// интерфейсу PhysicsWorld; конкретный движок физики (Jolt — основной, плюс
// встроенный Simple и пустой Null) прячется за фабрикой PhysicsWorld::Create.
// ---------------------------------------------------------------------------
namespace sage::physics {

// Доступные бэкенды. Jolt — основной (полноценная физика, jrouwe/JoltPhysics),
// подключается опционально через CMake (SAGE_PHYSICS_JOLT). Simple — встроенный
// лёгкий интегратор (гравитация + столкновение со статикой), без зависимостей,
// доступен всегда. Null — заглушка (физика отключена).
enum class Backend { Null, Simple, Jolt };

// Тип тела:
//   Static    — неподвижное препятствие (пол, стены);
//   Dynamic   — управляется физикой (падает, сталкивается);
//   Kinematic — движется извне (скриптом/анимацией), толкает динамику, но само
//               не реагирует на столкновения.
enum class BodyType { Static, Dynamic, Kinematic };

// Форма коллайдера. Simple-бэкенд поддерживает Box (AABB), Sphere и Capsule
// (вертикальный отрезок + радиус) как настоящие формы; Jolt — полноценно все.
enum class ShapeType { Box, Sphere, Capsule };

// Одна дочерняя форма СОСТАВНОГО (compound) тела: форма + её ЛОКАЛЬНОЕ смещение/
// поворот относительно начала тела. Составное тело = несколько таких форм в
// одном твёрдом теле (напр. «молоток» = ручка-бокс + головка-бокс со сдвигом,
// или коллайдер-аппроксимация модели из нескольких примитивов). Jolt строит из
// них StaticCompoundShape; Simple берёт объемлющий AABB (составное — приближение).
struct ChildShape {
    ShapeType Shape = ShapeType::Box;
    glm::vec3 HalfExtents{0.5f, 0.5f, 0.5f}; // Box
    float Radius = 0.5f;                     // Sphere / Capsule
    float HalfHeight = 0.5f;                 // Capsule
    glm::vec3 Position{0.0f};                // локальное смещение внутри тела
    glm::quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
};

// Описание тела для создания. Позиция/поворот — начальные (дальше их ведёт
// физика для Dynamic). Размеры — в ЛОКАЛЬНЫХ единицах формы; масштаб сущности
// применяется при создании тела (см. PhysicsScene).
struct BodyDesc {
    BodyType Type = BodyType::Dynamic;
    ShapeType Shape = ShapeType::Box;
    glm::vec3 HalfExtents{0.5f, 0.5f, 0.5f}; // Box
    float Radius = 0.5f;                     // Sphere / Capsule
    float HalfHeight = 0.5f;                 // Capsule (половина цилиндрической части)

    // Составная форма: если непусто, тело строится ИЗ ЭТИХ дочерних форм, а
    // Shape/HalfExtents/Radius/HalfHeight выше игнорируются. Пусто — одиночная
    // форма как раньше (обратная совместимость).
    std::vector<ChildShape> Children;

    glm::vec3 Position{0.0f};
    glm::quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};

    float Mass = 1.0f;        // кг (для Dynamic; Static/Kinematic игнорируют)
    float Friction = 0.5f;    // 0..1
    float Restitution = 0.1f; // упругость 0..1 (0 — не отскакивает)
};

// Дескриптор тела внутри мира (0 — невалидный). Непрозрачный для вызывающего.
using BodyHandle = uint32_t;
constexpr BodyHandle kInvalidBody = 0;

// Тип соединения (constraint/joint) между двумя телами (или телом и миром):
//   Fixed    — жёсткая связь (фиксирует взаимное положение/поворот; сварка);
//   Point    — шарнир-точка (ball-socket): точка совпадает, вращение свободно;
//   Hinge    — петля вокруг оси (дверь/колесо), опц. пределы угла [Min,Max]°;
//   Slider   — призма вдоль оси (поршень/лифт), опц. пределы хода [Min,Max];
//   Distance — держит две точки на расстоянии [MinDistance, MaxDistance] (трос);
//   Cone     — конус (ограничивает отклонение оси на ConeHalfAngle°) — основа
//              суставов ragdoll (плечи/бёдра болтаются в конусе).
enum class JointType { Fixed, Point, Hinge, Slider, Distance, Cone };

// Описание соединения. Точка/оси — в МИРОВЫХ координатах на момент создания
// (бэкенд вычисляет из них локальные рамки тел по их текущим позам). BodyB ==
// kInvalidBody — прикрепить BodyA к миру (неподвижный якорь).
struct JointDesc {
    JointType Type = JointType::Point;
    BodyHandle BodyA = kInvalidBody;
    BodyHandle BodyB = kInvalidBody;   // kInvalidBody -> к миру
    glm::vec3 Anchor{0.0f};            // мировая точка крепления (pivot)
    glm::vec3 Axis{0.0f, 1.0f, 0.0f};  // ось: Hinge/Slider — вдоль; Cone — twist

    bool UseLimits = false;            // Hinge/Slider: включить пределы ниже
    float MinLimit = 0.0f;             // Hinge: угол °; Slider: смещение (ед.)
    float MaxLimit = 0.0f;

    float MinDistance = 0.0f;          // Distance
    float MaxDistance = 1.0f;
    float ConeHalfAngle = 45.0f;       // Cone: полу-угол конуса, градусы
};

// Дескриптор соединения внутри мира (0 — невалидный).
using JointHandle = uint32_t;
constexpr JointHandle kInvalidJoint = 0;

} // namespace sage::physics
