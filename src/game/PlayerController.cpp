#include "PlayerController.h"
#include <cmath>
#include <algorithm>

bool PlayerController::CollidesAt(World& world, const glm::vec3& pos) const {
    // Перебираем все воксели, которые может задевать AABB игрока.
    // Небольшой epsilon по верхней границе — чтобы стоя ровно на границе
    // блока не считаться "внутри" блока над головой.
    const float eps = 0.001f;
    int minX = (int)std::floor(pos.x - HalfWidth);
    int maxX = (int)std::floor(pos.x + HalfWidth - eps);
    int minY = (int)std::floor(pos.y);
    int maxY = (int)std::floor(pos.y + Height - eps);
    int minZ = (int)std::floor(pos.z - HalfWidth);
    int maxZ = (int)std::floor(pos.z + HalfWidth - eps);

    for (int x = minX; x <= maxX; ++x)
        for (int y = minY; y <= maxY; ++y)
            for (int z = minZ; z <= maxZ; ++z)
                if (IsSolid(world.GetBlock(x, y, z))) return true;
    return false;
}

void PlayerController::MoveAxis(World& world, int axis, float delta) {
    if (delta == 0.0f) return;

    glm::vec3 newPos = Position;
    newPos[axis] += delta;

    if (!CollidesAt(world, newPos)) {
        Position = newPos;
        return;
    }

    // Столкновение: придвигаемся к препятствию мелкими шагами и гасим
    // скорость по этой оси. Шаг 1/32 блока — достаточно точно и дёшево.
    float step = (delta > 0.0f) ? 0.03125f : -0.03125f;
    float moved = 0.0f;
    while (std::abs(moved + step) <= std::abs(delta)) {
        glm::vec3 probe = Position;
        probe[axis] += step;
        if (CollidesAt(world, probe)) break;
        Position = probe;
        moved += step;
    }

    if (axis == 1 && delta < 0.0f) OnGround = true;
    Velocity[axis] = 0.0f;
}

void PlayerController::Update(World& world, float dt, glm::vec3 moveDir, bool wantsJump, bool wantsRun, bool canRun) {
    // Вода "начинается" чуть ниже уровня моря, чтобы стоя на палубе у
    // ватерлинии не считаться пловцом
    InWater = (Position.y + 0.4f) < SeaLevel;

    m_isRunning = false;
    m_isSwimmingActive = false;

    // --- горизонтальное движение ---
    glm::vec3 horiz(moveDir.x, 0.0f, moveDir.z);
    float horizLen = glm::length(horiz);
    float speed = 0.0f;
    if (horizLen > 0.001f) {
        horiz /= horizLen;
        if (InWater) {
            speed = SwimSpeed;
            m_isSwimmingActive = true;
        } else if (wantsRun && canRun) {
            speed = RunSpeed;
            m_isRunning = true;
        } else {
            speed = WalkSpeed;
        }
    }

    Velocity.x = horiz.x * speed;
    Velocity.z = horiz.z * speed;

    // --- вертикальное движение ---
    if (InWater) {
        // В воде: слабая гравитация, Space — грести вверх; вода мягко
        // гасит вертикальную скорость (сопротивление).
        Velocity.y -= Gravity * 0.15f * dt;
        if (wantsJump) {
            Velocity.y = std::min(Velocity.y + 20.0f * dt, 3.0f);
            m_isSwimmingActive = true;
        }
        Velocity.y *= (1.0f - std::min(1.0f, 2.5f * dt)); // сопротивление воды
        Velocity.y = std::max(Velocity.y, -2.0f);          // тонем медленно

        // Океан "движется" мимо статичного корабля: упавшего за борт
        // игрока честно сносит течением по +Z — обратно к корме и дальше.
        Velocity.z += 1.5f;
    } else {
        Velocity.y -= Gravity * dt;
        if (wantsJump && OnGround) {
            Velocity.y = JumpVelocity;
            OnGround = false;
        }
    }

    // --- интеграция с коллизиями, ось за осью ---
    OnGround = false;
    MoveAxis(world, 0, Velocity.x * dt);
    MoveAxis(world, 1, Velocity.y * dt);
    MoveAxis(world, 2, Velocity.z * dt);

    // Не даём провалиться сквозь дно океана (страховка)
    if (Position.y < SeaLevel - 6.0f) {
        Position.y = SeaLevel - 6.0f;
        Velocity.y = 0.0f;
    }
}
