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

    // ВАЖНО: НЕ нормализуем здесь.
    //
    // Здесь и была «кривизна» неба. Растеризатор интерполирует varying ЛИНЕЙНО,
    // и линейно интерполировать можно только точки дальней плоскости — они
    // лежат на плоскости, и промежуточное значение остаётся на ней. Нормаль же
    // делит каждую вершину на СВОЮ длину, а длины у вершин полноэкранного
    // треугольника отличаются в разы (NDC идёт от -1 до 3, угол куда дальше
    // центра). После такого деления линейная интерполяция даёт направления,
    // которые не соответствуют ни одному лучу камеры: градиент неба сжимается,
    // горизонт уезжает и перекашивается, и чем шире кадр, тем сильнее.
    //
    // Поэтому в varying уходит ПОЗИЦИЯ на дальней плоскости, а нормализация —
    // во фрагментном шейдере, где она уже поточечная и ничего не искажает.
    vDir = mat3(uInvViewRot) * (vp.xyz / vp.w);
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

    // pow(t, 0.5) даёт нужный вид — почти всё небо цвета зенита, у горизонта
    // узкая светлая полоса, — но у корня БЕСКОНЕЧНАЯ производная в нуле. Ниже
    // горизонта при этом лежит плоская плита ровно одного цвета. Стык плиты с
    // бесконечно крутым подъёмом — разрыв по производной, и выглядит он как
    // жёсткая линия: на самом небе полосой у горизонта, а на отражающем шаре —
    // ступенькой поперёк, которая читается как дефект рендера, а не как небо.
    //
    // Окно smoothstep гасит подъём в первых ~15° над горизонтом: у него нулевая
    // производная в нуле, поэтому стык становится гладким. Выше окна множитель
    // равен единице, и картина остаётся ровно той, ради которой брали корень.
    float soften = smoothstep(0.0, 0.25, y);
    vec3 col = mix(uHorizon, uTop, pow(t, 0.5) * soften);
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
