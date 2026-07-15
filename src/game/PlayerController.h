#pragma once
#include "../voxel/World.h"
#include <glm/glm.hpp>

// Игрок от первого лица с "майнкрафтовской" физикой: AABB-коллизии
// с воксельным миром, гравитация, прыжок, ходьба/бег, плавание в воде.
//
// Position — точка НОГ игрока (низ AABB). Камера ставится на EyeHeight выше.
class PlayerController {
public:
    glm::vec3 Position{0.0f};
    glm::vec3 Velocity{0.0f};

    static constexpr float HalfWidth = 0.3f;   // AABB: 0.6 x 1.8 x 0.6
    static constexpr float Height = 1.8f;
    static constexpr float EyeHeight = 1.62f;

    float WalkSpeed = 4.0f;
    float RunSpeed = 6.5f;
    float SwimSpeed = 2.2f;
    float JumpVelocity = 8.0f;
    float Gravity = 22.0f;

    float SeaLevel = 8.0f;

    bool OnGround = false;
    bool InWater = false;

    glm::vec3 EyePosition() const { return Position + glm::vec3(0.0f, EyeHeight, 0.0f); }

    // moveDir — желаемое направление движения в горизонтальной плоскости
    // (уже спроецированное из camera Front/Right, ненормализованное — нормализуем сами),
    // wantsJump/wantsRun — состояние клавиш.
    void Update(World& world, float dt, glm::vec3 moveDir, bool wantsJump, bool wantsRun, bool canRun);

    bool IsMovingFast() const { return m_isRunning || (InWater && m_isSwimmingActive); }

private:
    bool m_isRunning = false;
    bool m_isSwimmingActive = false;

    // Проверяет, пересекает ли AABB игрока в позиции pos хоть один твёрдый блок
    bool CollidesAt(World& world, const glm::vec3& pos) const;

    // Двигает по одной оси с откатом при столкновении (классическое
    // пошаговое разрешение: ось за осью, чтобы скользить вдоль стен)
    void MoveAxis(World& world, int axis, float delta);
};
