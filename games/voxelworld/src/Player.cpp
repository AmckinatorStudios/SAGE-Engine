#include "Player.h"

#include <algorithm>
#include <cmath>

namespace voxel {

glm::vec3 Player::Forward() const {
    const float y = glm::radians(Yaw), p = glm::radians(Pitch);
    return glm::normalize(glm::vec3(std::cos(y) * std::cos(p), std::sin(p), std::sin(y) * std::cos(p)));
}

void Player::Look(float dx, float dy) {
    Yaw += dx * 0.12f;
    Pitch = std::clamp(Pitch - dy * 0.12f, -89.0f, 89.0f);
}

bool Player::Collides(const World& world, glm::vec3 pos) const {
    const glm::vec3 lo = pos - glm::vec3(kHalfWidth, 0.0f, kHalfWidth);
    const glm::vec3 hi = pos + glm::vec3(kHalfWidth, kHeight, kHalfWidth);
    for (int x = (int)std::floor(lo.x); x <= (int)std::floor(hi.x - 1e-4f); ++x)
        for (int y = (int)std::floor(lo.y); y <= (int)std::floor(hi.y - 1e-4f); ++y)
            for (int z = (int)std::floor(lo.z); z <= (int)std::floor(hi.z - 1e-4f); ++z) {
                // За краем мира — стена: иначе игрок уходит в пустоту.
                if (x < 0 || z < 0 || x >= world.SizeX() || z >= world.SizeZ()) return true;
                if (Info(world.Get(x, y, z)).Solid) return true;
            }
    return false;
}

void Player::MoveAxis(const World& world, int axis, float delta) {
    // Шагами не длиннее трети блока — быстрое падение не проскакивает пол.
    const int steps = std::max(1, (int)std::ceil(std::fabs(delta) / 0.3f));
    const float step = delta / (float)steps;
    for (int i = 0; i < steps; ++i) {
        glm::vec3 next = Position;
        next[axis] += step;
        if (Collides(world, next)) {
            if (axis == 1 && step < 0.0f) OnGround = true;
            Velocity[axis] = 0.0f;
            return;
        }
        Position = next;
    }
}

void Player::Update(const World& world, const PlayerInput& in, float dt) {
    dt = std::min(dt, 0.05f);
    const float yaw = glm::radians(Yaw);
    const glm::vec3 fwd(std::cos(yaw), 0.0f, std::sin(yaw));
    const glm::vec3 right(-fwd.z, 0.0f, fwd.x);
    glm::vec3 wish = fwd * in.Move.y + right * in.Move.x;
    if (glm::length(wish) > 1.0f) wish = glm::normalize(wish);

    const bool inWater = world.Get((int)std::floor(Position.x), (int)std::floor(Position.y + 0.4f),
                                   (int)std::floor(Position.z)) == Water;
    if (Flying) {
        const float speed = 11.0f;
        Velocity = wish * speed;
        Velocity.y = (in.Jump ? speed : 0.0f) - (in.Down ? speed : 0.0f);
    } else {
        const float speed = inWater ? 2.6f : 4.4f;
        Velocity.x = wish.x * speed;
        Velocity.z = wish.z * speed;
        if (inWater) {
            Velocity.y = std::max(Velocity.y - 9.0f * dt, -3.0f);
            if (in.Jump) Velocity.y = 3.2f;
        } else {
            Velocity.y = std::max(Velocity.y - 28.0f * dt, -50.0f);
            if (in.Jump && OnGround) Velocity.y = 8.6f;
        }
    }
    OnGround = false;
    MoveAxis(world, 0, Velocity.x * dt);
    MoveAxis(world, 2, Velocity.z * dt);
    MoveAxis(world, 1, Velocity.y * dt);
}

bool Player::Overlaps(glm::ivec3 b) const {
    const glm::vec3 lo = Position - glm::vec3(kHalfWidth, 0.0f, kHalfWidth);
    const glm::vec3 hi = Position + glm::vec3(kHalfWidth, kHeight, kHalfWidth);
    return lo.x < b.x + 1 && hi.x > b.x && lo.y < b.y + 1 && hi.y > b.y && lo.z < b.z + 1 && hi.z > b.z;
}

} // namespace voxel
