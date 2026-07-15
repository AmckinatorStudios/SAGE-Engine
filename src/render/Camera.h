#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

enum class CameraMove { Forward, Backward, Left, Right, Up, Down };

// Простая камера-полёт (свободное перемещение + поворот мышью)
class Camera {
public:
    glm::vec3 Position{0.0f, 0.0f, 3.0f};
    glm::vec3 Front{0.0f, 0.0f, -1.0f};
    glm::vec3 Up{0.0f, 1.0f, 0.0f};
    glm::vec3 Right{1.0f, 0.0f, 0.0f};
    glm::vec3 WorldUp{0.0f, 1.0f, 0.0f};

    float Yaw = -90.0f;
    float Pitch = 0.0f;
    float MovementSpeed = 4.0f;
    float MouseSensitivity = 0.1f;
    float Fov = 60.0f;

    Camera() { UpdateVectors(); }

    glm::mat4 GetViewMatrix() const {
        return glm::lookAt(Position, Position + Front, Up);
    }

    glm::mat4 GetProjectionMatrix(float aspect) const {
        return glm::perspective(glm::radians(Fov), aspect, 0.1f, 200.0f);
    }

    void ProcessKeyboard(CameraMove dir, float deltaTime) {
        float velocity = MovementSpeed * deltaTime;
        switch (dir) {
            case CameraMove::Forward:  Position += Front * velocity; break;
            case CameraMove::Backward: Position -= Front * velocity; break;
            case CameraMove::Left:     Position -= Right * velocity; break;
            case CameraMove::Right:    Position += Right * velocity; break;
            case CameraMove::Up:       Position += WorldUp * velocity; break;
            case CameraMove::Down:     Position -= WorldUp * velocity; break;
        }
    }

    void ProcessMouse(float xoffset, float yoffset) {
        Yaw += xoffset * MouseSensitivity;
        Pitch += yoffset * MouseSensitivity;
        if (Pitch > 89.0f) Pitch = 89.0f;
        if (Pitch < -89.0f) Pitch = -89.0f;
        UpdateVectors();
    }

private:
    void UpdateVectors() {
        glm::vec3 front;
        front.x = cos(glm::radians(Yaw)) * cos(glm::radians(Pitch));
        front.y = sin(glm::radians(Pitch));
        front.z = sin(glm::radians(Yaw)) * cos(glm::radians(Pitch));
        Front = glm::normalize(front);
        Right = glm::normalize(glm::cross(Front, WorldUp));
        Up = glm::normalize(glm::cross(Right, Front));
    }
};
