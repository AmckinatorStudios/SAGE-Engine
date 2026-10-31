#pragma once
#include <glm/glm.hpp>

#include "World.h"

// ---------------------------------------------------------------------------
// Игрок: коробка 0.6×1.8, ходьба с гравитацией или полёт, столкновения с
// блоками по осям по очереди — так игрок скользит вдоль стены, а не
// застревает в ней углом.
// ---------------------------------------------------------------------------
namespace voxel {

struct PlayerInput {
    glm::vec2 Move{0.0f}; // x — вбок, y — вперёд
    bool Jump = false;    // в полёте — вверх
    bool Down = false;    // в полёте — вниз
};

class Player {
public:
    glm::vec3 Position{0.0f}; // ноги
    glm::vec3 Velocity{0.0f};
    float Yaw = -90.0f, Pitch = -15.0f;
    bool Flying = false;
    bool OnGround = false;

    static constexpr float kHalfWidth = 0.3f;
    static constexpr float kHeight = 1.8f;
    static constexpr float kEye = 1.62f;

    glm::vec3 Eye() const { return Position + glm::vec3(0.0f, kEye, 0.0f); }
    glm::vec3 Forward() const;
    void Look(float dx, float dy);
    void Update(const World& world, const PlayerInput& in, float dt);
    // Пересекает ли коробка игрока блок (нельзя ставить блок в себя).
    bool Overlaps(glm::ivec3 block) const;

private:
    bool Collides(const World& world, glm::vec3 pos) const;
    void MoveAxis(const World& world, int axis, float delta);
};

} // namespace voxel
