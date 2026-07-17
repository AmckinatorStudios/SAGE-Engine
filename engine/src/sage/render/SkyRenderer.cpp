#include "sage/render/SkyRenderer.h"

#include <glm/gtc/matrix_transform.hpp>

namespace {

// Полноэкранный треугольник; направление мирового луча восстанавливается из
// обратной проекции + поворота камеры (translation не нужен — небо на
// бесконечности).
const char* kVertexSrc = R"GLSL(
#version 330 core
out vec3 vDir;
uniform mat4 uInvProj;
uniform mat4 uInvViewRot; // поворот view->world в mat4 (SetMat3 в RHI нет)
void main() {
    vec2 pos = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2); // 0,0 / 2,0 / 0,2
    vec2 ndc = pos * 2.0 - 1.0;                                // -1..3 накрывает экран
    vec4 vp = uInvProj * vec4(ndc, 1.0, 1.0);
    vec3 viewDir = normalize(vp.xyz / vp.w);
    vDir = mat3(uInvViewRot) * viewDir;
    gl_Position = vec4(ndc, 1.0, 1.0); // z=1 — дальняя плоскость
}
)GLSL";

const char* kFragmentSrc = R"GLSL(
#version 330 core
in vec3 vDir;
out vec4 FragColor;
uniform vec3 uTop;
uniform vec3 uHorizon;
void main() {
    float y = normalize(vDir).y;
    float t = clamp(y, 0.0, 1.0);            // 0 у горизонта и ниже, 1 в зените
    vec3 col = mix(uHorizon, uTop, pow(t, 0.5)); // мягкий переход к зениту
    FragColor = vec4(col, 1.0);
}
)GLSL";

} // namespace

SkyRenderer::SkyRenderer() {
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    m_shader = device.CreateShaderProgram(kVertexSrc, kFragmentSrc);
    // Геометрия без атрибутов — вершины считаются из gl_VertexID (см. VS).
    m_geometry = device.CreateGeometry(sage::rhi::VertexLayout{});
}

void SkyRenderer::Draw(const glm::mat4& view, const glm::mat4& proj,
                       glm::vec3 topColor, glm::vec3 horizonColor) {
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();

    m_shader->Use();
    m_shader->SetMat4("uInvProj", glm::inverse(proj));
    // view — ортонормальная (R*T): view->world поворот = transpose(mat3(view)),
    // упаковываем в mat4 (у ShaderProgram нет SetMat3; VS берёт mat3(...)).
    m_shader->SetMat4("uInvViewRot", glm::mat4(glm::transpose(glm::mat3(view))));
    m_shader->SetVec3("uTop", topColor);
    m_shader->SetVec3("uHorizon", horizonColor);

    // Небо — фон: без теста и записи глубины, без отсечения. Рисуется первым,
    // сцена ложится поверх по своей глубине.
    device.SetDepthTest(false);
    device.SetDepthWrite(false);
    device.SetCullMode(sage::rhi::CullMode::Off);

    m_geometry->DrawArrays(3);

    device.SetCullMode(sage::rhi::CullMode::Back);
    device.SetDepthWrite(true);
    device.SetDepthTest(true);
}
