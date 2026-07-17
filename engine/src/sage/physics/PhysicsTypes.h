#pragma once
#include <cstdint>
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

// Форма коллайдера. Capsule приближается боксом во встроенном Simple-бэкенде;
// Jolt поддерживает её полноценно.
enum class ShapeType { Box, Sphere, Capsule };

// Описание тела для создания. Позиция/поворот — начальные (дальше их ведёт
// физика для Dynamic). Размеры — в ЛОКАЛЬНЫХ единицах формы; масштаб сущности
// применяется при создании тела (см. PhysicsScene).
struct BodyDesc {
    BodyType Type = BodyType::Dynamic;
    ShapeType Shape = ShapeType::Box;
    glm::vec3 HalfExtents{0.5f, 0.5f, 0.5f}; // Box
    float Radius = 0.5f;                     // Sphere / Capsule
    float HalfHeight = 0.5f;                 // Capsule (половина цилиндрической части)

    glm::vec3 Position{0.0f};
    glm::quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};

    float Mass = 1.0f;        // кг (для Dynamic; Static/Kinematic игнорируют)
    float Friction = 0.5f;    // 0..1
    float Restitution = 0.1f; // упругость 0..1 (0 — не отскакивает)
};

// Дескриптор тела внутри мира (0 — невалидный). Непрозрачный для вызывающего.
using BodyHandle = uint32_t;
constexpr BodyHandle kInvalidBody = 0;

} // namespace sage::physics
