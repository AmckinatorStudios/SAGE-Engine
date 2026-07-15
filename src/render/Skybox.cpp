#include "Skybox.h"
#include <stb_image.h>
#include <stdexcept>
#include "../core/Log.h"

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
    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kSkyboxVertices), kSkyboxVertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glBindVertexArray(0);

    stbi_set_flip_vertically_on_load(false); // у cubemap другая конвенция — грани НЕ переворачиваем

    glGenTextures(1, &m_textureId);
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_textureId);

    for (unsigned int i = 0; i < faces.size(); ++i) {
        int width, height, channels;
        unsigned char* data = stbi_load(faces[i].c_str(), &width, &height, &channels, 0);
        if (!data) {
            glDeleteTextures(1, &m_textureId);
            throw std::runtime_error("Не удалось загрузить грань skybox: " + faces[i] + " (" + stbi_failure_reason() + ")");
        }
        GLenum format = (channels == 4) ? GL_RGBA : GL_RGB;
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, format, width, height, 0, format, GL_UNSIGNED_BYTE, data);
        stbi_image_free(data);
    }

    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

    stbi_set_flip_vertically_on_load(true); // возвращаем конвенцию обратно для обычных Texture

    LOG_INFO("Skybox") << "Skybox загружен (6 граней)";
}

Skybox::~Skybox() {
    glDeleteVertexArrays(1, &m_vao);
    glDeleteBuffers(1, &m_vbo);
    glDeleteTextures(1, &m_textureId);
}

void Skybox::Draw(Shader& shader, const glm::mat4& view, const glm::mat4& projection,
                  const glm::vec3& tint) const {
    // Рисуем skybox ПЕРЕД остальной сценой, но так, чтобы он всегда оставался
    // позади всего (это надёжнее, чем полагаться на порядок отрисовки с
    // depth-тестом: GL_LEQUAL + запись глубины 1.0 в вершинном шейдере
    // гарантируют, что skybox никогда не перекроет реальную геометрию,
    // даже если её отрисовать позже).
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);
    // Skybox рисуется "изнутри" куба — при обычном backface culling (CCW
    // снаружи) все грани оказались бы отвёрнуты от камеры, поэтому на время
    // отрисовки отключаем culling.
    glDisable(GL_CULL_FACE);

    shader.Use();
    // Убираем сдвиг (позицию камеры) из view-матрицы — skybox должен всегда
    // казаться бесконечно далёким и не двигаться при перемещении камеры,
    // реагируя только на поворот.
    glm::mat4 viewNoTranslation = glm::mat4(glm::mat3(view));
    shader.SetMat4("uView", viewNoTranslation);
    shader.SetMat4("uProjection", projection);
    shader.SetInt("uSkybox", 0);
    shader.SetVec3("uTint", tint);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_textureId);

    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLES, 0, 36);
    glBindVertexArray(0);

    glEnable(GL_CULL_FACE);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);
}
