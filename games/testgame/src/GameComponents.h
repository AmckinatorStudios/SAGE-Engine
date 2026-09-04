#pragma once
#include <string>
#include "sage/core/Math.h"

// ---------------------------------------------------------------------------
// Кастомные ECS-компоненты testgame — живут В ИГРЕ, движок про них не знает.
//
// Это официальный паттерн расширения (см. README, «Система сцен»): игра
// навешивает произвольные структуры на сущности через
// scene.Registry().emplace<T>(entity, ...) и итерирует их своими системами
// (обычные entt-view в коде игры) — ядро движка не правится.
//
// ВАЖНО (известное ограничение, проверено этой игрой): SceneSerializer
// сохраняет только встроенные компоненты движка (Name/Id/Transform/
// MeshRenderer/Script) — кастомные компоненты в .sage-файл НЕ попадают.
// Поэтому testgame строит комнаты кодом, а сериализацию проверяет отдельным
// round-trip-тестом на движковой части сцены (см. VerifySerializationRoundTrip).
// ---------------------------------------------------------------------------

// Здоровье. В testgame навешивается на сущность "Player" каждой комнаты.
struct HealthComponent {
    float Current = 100.0f;
    float Max = 100.0f;
};

// Подбираемый предмет: очки за подбор и радиус срабатывания.
struct PickupComponent {
    int Score = 10;
    float Radius = 0.9f;
};

// Патрулирующий враг: ходит между точками A и B, при касании ранит игрока.
struct PatrolComponent {
    glm::vec3 PointA{0.0f};
    glm::vec3 PointB{0.0f};
    float Speed = 2.0f;
    float Damage = 15.0f;
    bool TowardB = true;
};

// Маркер статической коллизии: AABB берётся из Transform (Position ± Scale/2,
// куб движка — единичный, отцентрованный). Игрок не проходит сквозь такие.
struct StaticColliderComponent {};

// Портал в другую комнату-сцену: имя целевой сцены + где в ней появиться.
struct PortalComponent {
    std::string TargetScene;
    glm::vec3 SpawnPos{0.0f};
    float Radius = 1.4f;
};
