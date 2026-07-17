#pragma once
#include <unordered_map>
#include "sage/physics/PhysicsWorld.h"

namespace sage::physics {

// Null-бэкенд — физика отключена: тела «создаются» (чтобы код не ветвился), но
// Step ничего не делает, тела остаются на месте. IsAvailable() == false, чтобы
// вызывающий мог не тратить кадр на бесполезный шаг.
class NullWorld : public PhysicsWorld {
public:
    const char* BackendName() const override { return "Null"; }
    bool IsAvailable() const override { return false; }
    void SetGravity(const glm::vec3&) override {}

    BodyHandle CreateBody(const BodyDesc& desc) override {
        BodyHandle h = m_next++;
        m_pose[h] = {desc.Position, desc.Rotation};
        return h;
    }
    void RemoveBody(BodyHandle body) override { m_pose.erase(body); }
    void Step(float) override {}

    void GetBodyTransform(BodyHandle body, glm::vec3& position, glm::quat& rotation) const override {
        auto it = m_pose.find(body);
        if (it != m_pose.end()) { position = it->second.first; rotation = it->second.second; }
    }
    void SetBodyTransform(BodyHandle body, const glm::vec3& position, const glm::quat& rotation) override {
        auto it = m_pose.find(body);
        if (it != m_pose.end()) it->second = {position, rotation};
    }
    void SetLinearVelocity(BodyHandle, const glm::vec3&) override {}
    glm::vec3 GetLinearVelocity(BodyHandle) const override { return glm::vec3(0.0f); }

private:
    BodyHandle m_next = 1;
    std::unordered_map<BodyHandle, std::pair<glm::vec3, glm::quat>> m_pose;
};

} // namespace sage::physics
