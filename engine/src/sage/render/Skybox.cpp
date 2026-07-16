#include "Skybox.h"
#include <stb_image.h>
#include <stdexcept>
#include "sage/core/Log.h"
#include "sage/rhi/GraphicsDevice.h"

using namespace sage::rhi;

// Куб единичного размера для skybox — только позиции, без нормалей/UV
// (текстурные координаты для cubemap — это само направление вершины от центра)
static const float kSkyboxVertices[] = {
    -1.0f,  1.0f, -1.0f,   -1.0f, -1.0f, -1.0f,    1.0f, -1.0f, -1.0f,
     1.0f, -1.0f, -1.0f,    1.0f,  1.0f, -1.0f,   -1.0f,  1.0f, -1.0f,

    -1.0f, -1.0f,  1.0f,   -1.0f, -1.0f, -1.0f,   -1.0f,  1.0f, -1.0f,
    -1.0f,  1.0f, -1.0f,   -1.0f,  1.0f,  1.0f,   -1.0f, -1.0f,  1.0f,

     1.0f, -1.0f, -1.0f,    1.0f, -1.0f,  1.0f,    1.0f,  1.0f,  1.0f,
     1.0f,  1.0f,  1.0f,    1.0f,  1.0f, -1.0f,    1.0f, -1.0f, -1.0f,

    -1.0f, -1.0f,  1.0f,   -1.0f,  1.0f,  1.0f,    1.0f,  1.0f,  1.0f,
     1.0f,  1.0f,  1.0f,    1.0f, -1.0f,  1.0f,   -1.0f, -1.0f,  1.0f,

    -1.0f,  1.0f, -1.0f,    1.0f,  1.0f, -1.0f,    1.0f,  1.0f,  1.0f,
     1.0f,  1.0f,  1.0f,   -1.0f,  1.0f,  1.0f,   -1.0f,  1.0f, -1.0f,

    -1.0f, -1.0f, -1.0f,   -1.0f, -1.0f,  1.0f,    1.0f, -1.0f, -1.0f,
     1.0f, -1.0f, -1.0f,   -1.0f, -1.0f,  1.0f,    1.0f, -1.0f,  1.0f
};

Skybox::Skybox(const std::array<std::string, 6>& faces) {
    GraphicsDevice& device = GraphicsDevice::Get();

    VertexLayout layout;
    layout.Stride = 3 * sizeof(float);
    layout.Attributes = {{0, 3, AttribType::Float, 0}};
    m_geometry = device.CreateGeometry(layout);
    m_geometry->SetVertexData(kSkyboxVertices, sizeof(kSkyboxVertices), /*dynamic=*/false);

    stbi_set_flip_vertically_on_load(false); // у cubemap другая конвенция — грани НЕ переворачиваем

    // Сначала декодируем все 6 граней (падение на любой — без утечки уже
    // декодированных), затем одним вызовом создаём cubemap у бэкенда.
    unsigned char* pixels[6] = {};
    CubeFacePixels cubeFaces[6];
    try {
        for (int i = 0; i < 6; ++i) {
            int width, height, channels;
            pixels[i] = stbi_load(faces[i].c_str(), &width, &height, &channels, 0);
            if (!pixels[i]) {
                throw std::runtime_error("Не удалось загрузить грань skybox: " + faces[i] +
                                         " (" + stbi_failure_reason() + ")");
            }
            cubeFaces[i] = {width, height, channels, pixels[i]};
        }
        m_cubemap = device.CreateTextureCube(cubeFaces);
    } catch (...) {
        for (unsigned char* p : pixels) if (p) stbi_image_free(p);
        stbi_set_flip_vertically_on_load(true);
        throw;
    }
    for (unsigned char* p : pixels) stbi_image_free(p);

    stbi_set_flip_vertically_on_load(true); // возвращаем конвенцию обратно для обычных Texture

    LOG_INFO("Skybox") << "Skybox загружен (6 граней)";
}

void Skybox::Draw(Shader& shader, const glm::mat4& view, const glm::mat4& projection,
                  const glm::vec3& tint) const {
    GraphicsDevice& device = GraphicsDevice::Get();

    // Рисуем skybox ПЕРЕД остальной сценой, но так, чтобы он всегда оставался
    // позади всего: LessEqual + глубина 1.0 из вершинного шейдера гарантируют,
    // что skybox никогда не перекроет реальную геометрию.
    device.SetDepthFunc(DepthFunc::LessEqual);
    device.SetDepthWrite(false);
    // Skybox рисуется "изнутри" куба — при обычном backface culling все грани
    // оказались бы отвёрнуты от камеры, поэтому на время отрисовки выключаем.
    device.SetCullMode(CullMode::Off);

    shader.Use();
    // Убираем сдвиг (позицию камеры) из view-матрицы — skybox должен всегда
    // казаться бесконечно далёким, реагируя только на поворот камеры.
    glm::mat4 viewNoTranslation = glm::mat4(glm::mat3(view));
    shader.SetMat4("uView", viewNoTranslation);
    shader.SetMat4("uProjection", projection);
    shader.SetInt("uSkybox", 0);
    shader.SetVec3("uTint", tint);

    m_cubemap->Bind(0);
    m_geometry->DrawArrays(36);

    device.SetCullMode(CullMode::Back);
    device.SetDepthWrite(true);
    device.SetDepthFunc(DepthFunc::Less);
}
