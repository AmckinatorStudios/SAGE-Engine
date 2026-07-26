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
//   auto sky = Skybox::LoadFromDirectory("assets/textures/sky");
//   ...
//   sky->Draw(view, projection);   // рисовать ПЕРВЫМ, до остальной сцены
//
// Шейдер ВСТРОЕН (как у SkyRenderer): внешних файлов не требуется. Раньше Draw
// принимал Shader извне, а подходящего assets/shaders/skybox.* в репозитории не
// было — класс формально существовал, но воспользоваться им было нечем. Старая
// перегрузка со своим шейдером сохранена: она нужна тем, кто хочет свой разбор
// освещения неба.
class Skybox {
public:
    // faces — ровно 6 путей к картинкам, в порядке:
    // +X, -X, +Y (верх), -Y (низ), +Z, -Z
    explicit Skybox(const std::array<std::string, 6>& faces);

    // Грани по общепринятым именам внутри каталога: px/nx/py/ny/pz/nz с
    // расширением png, jpg, jpeg, tga или bmp (ищутся в этом порядке). Так
    // распространяются почти все наборы неба, и пользователю достаточно указать
    // ОДНУ папку вместо шести путей. nullptr, если каталога нет или в нём не
    // хватает граней; причина уходит в лог.
    static std::unique_ptr<Skybox> LoadFromDirectory(const std::string& directory);

    // Имена граней в порядке +X, -X, +Y, -Y, +Z, -Z (без расширения).
    static const std::array<const char*, 6>& FaceNames();

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

    // Отрисовка встроенным шейдером — обычный путь.
    //   intensity  — множитель яркости неба (экспозиция окружения);
    //   rotationDeg— поворот неба вокруг вертикали: позволяет развернуть готовый
    //                набор так, чтобы солнце на картинке совпало с солнцем сцены.
    void Draw(const glm::mat4& view, const glm::mat4& projection,
              float intensity = 1.0f, float rotationDeg = 0.0f) const;

private:
    void DrawInternal(const glm::mat4& view, const glm::mat4& projection) const;

    std::unique_ptr<sage::rhi::Geometry> m_geometry;
    std::unique_ptr<sage::rhi::TextureCube> m_cubemap;
};
