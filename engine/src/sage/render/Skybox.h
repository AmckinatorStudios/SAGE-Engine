#pragma once
#include <memory>
#include <glm/glm.hpp>
#include <string>
#include <array>
#include "Shader.h"
#include "sage/rhi/Resources.h"

// Skybox: окружает сцену кубической текстурой (cubemap), создающей
// иллюзию бескрайнего неба/окружения. Часть ЯДРА движка — не зависит
// от вокселей и подходит для любой 3D-игры.
//
// Использование:
//   Skybox skybox({
//       "assets/textures/skybox/px.png", "assets/textures/skybox/nx.png",
//       "assets/textures/skybox/py.png", "assets/textures/skybox/ny.png",
//       "assets/textures/skybox/pz.png", "assets/textures/skybox/nz.png"
//   });
//   ...
//   skybox.Draw(shader, view, projection); // рисовать ПЕРВЫМ, до остальной сцены
class Skybox {
public:
    // faces — ровно 6 путей к картинкам, в порядке:
    // +X, -X, +Y (верх), -Y (низ), +Z, -Z
    explicit Skybox(const std::array<std::string, 6>& faces);

    Skybox(const Skybox&) = delete;
    Skybox& operator=(const Skybox&) = delete;
    Skybox(Skybox&&) noexcept = default;
    Skybox& operator=(Skybox&&) noexcept = default;

    // shader должен быть заранее слинкован из assets/shaders/skybox.vert/frag
    // (или совместимого). Функция сама выставляет uView/uProjection и
    // временно ослабляет depth-тест/culling, чтобы куб skybox был виден
    // "изнутри" и всегда оставался на заднем плане.
    void Draw(Shader& shader, const glm::mat4& view, const glm::mat4& projection,
              const glm::vec3& tint = glm::vec3(1.0f)) const;

private:
    std::unique_ptr<sage::rhi::Geometry> m_geometry;
    std::unique_ptr<sage::rhi::TextureCube> m_cubemap;
};
