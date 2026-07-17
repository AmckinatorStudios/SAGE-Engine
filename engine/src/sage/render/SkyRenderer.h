#pragma once
#include <memory>
#include <glm/glm.hpp>
#include "sage/rhi/GraphicsDevice.h"

// ---------------------------------------------------------------------------
// SkyRenderer — процедурный градиентный скайбокс без ассетов. Рисует
// полноэкранный треугольник, восстанавливает мировое направление луча из
// обратной матрицы вид-проекции и заливает фон градиентом от HorizonColor
// (низ) к TopColor (зенит) по вертикали луча. Часть ЯДРА рендера, доступна
// и редактору, и играм.
//
// Вызывать ПЕРВЫМ в кадре (до отрисовки сцены): пишет только цвет, без
// глубины (depth-mask off, depth-test off) — сцена рисуется поверх.
//
//   sky.Draw(view, proj, env.Skybox.TopColor, env.Skybox.HorizonColor);
//   ... затем обычный проход сцены ...
//
// Шейдер встроен строкой — внешних файлов не требует.
// ---------------------------------------------------------------------------
class SkyRenderer {
public:
    SkyRenderer();

    SkyRenderer(const SkyRenderer&) = delete;
    SkyRenderer& operator=(const SkyRenderer&) = delete;

    void Draw(const glm::mat4& view, const glm::mat4& proj, glm::vec3 topColor, glm::vec3 horizonColor);

private:
    std::unique_ptr<sage::rhi::ShaderProgram> m_shader;
    std::unique_ptr<sage::rhi::Geometry> m_geometry;
};
