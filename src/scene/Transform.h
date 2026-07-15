#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

struct Transform {
    glm::vec3 Position{0.0f};
    glm::vec3 Rotation{0.0f}; // в градусах, углы Эйлера
    glm::vec3 Scale{1.0f};

    glm::mat4 GetMatrix() const {
        glm::mat4 m = glm::translate(glm::mat4(1.0f), Position);
        m = glm::rotate(m, glm::radians(Rotation.x), glm::vec3(1,0,0));
        m = glm::rotate(m, glm::radians(Rotation.y), glm::vec3(0,1,0));
        m = glm::rotate(m, glm::radians(Rotation.z), glm::vec3(0,0,1));
        m = glm::scale(m, Scale);
        return m;
    }
};
