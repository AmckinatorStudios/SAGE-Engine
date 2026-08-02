#include "sage/render/SkyRenderer.h"

#include "sage/scene/Light.h"

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
uniform float uCelestials;   // 0 — светил нет (голый градиент, как было)
uniform vec3 uSunDir;
uniform vec3 uSunColor;
uniform float uSunSize;
uniform float uMoon;
uniform vec3 uMoonColor;
uniform float uMoonSize;
uniform float uStars;
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

    // --- Светила ------------------------------------------------------------
    //
    // Диск рисуется по УГЛУ между лучом и направлением на светило, а не
    // проекцией на плоскость экрана: угол не зависит ни от поля зрения, ни от
    // соотношения сторон, поэтому солнце остаётся круглым в любом окне и не
    // растягивается по краям кадра.
    if (uCelestials > 0.5) {
        vec3 dir = normalize(vDir);

        // Солнце. Мягкий край — это не размытие ради красоты: диск в один
        // пиксель без сглаживания мерцает при движении камеры.
        float sunCos = dot(dir, uSunDir);
        float sunAng = acos(clamp(sunCos, -1.0, 1.0));
        float sunDisk = 1.0 - smoothstep(uSunSize * 0.85, uSunSize, sunAng);
        // Ореол вокруг диска: без него солнце выглядит наклейкой на небе.
        float sunGlow = pow(max(0.0, sunCos), 220.0) * 0.6
                      + pow(max(0.0, sunCos), 12.0) * 0.10;
        col += uSunColor * (sunDisk * 6.0 + sunGlow);

        if (uMoon > 0.5) {
            float moonCos = dot(dir, -uSunDir);
            float moonAng = acos(clamp(moonCos, -1.0, 1.0));
            float moonDisk = 1.0 - smoothstep(uMoonSize * 0.8, uMoonSize, moonAng);
            // Фаза: тень наползает сбоку. Без неё луна — просто белый кружок.
            vec3 east = normalize(cross(vec3(0.0, 1.0, 0.0), -uSunDir) + vec3(1e-5));
            float phase = smoothstep(-0.25, 0.35, dot(dir - (-uSunDir), east) / max(uMoonSize, 1e-4));
            float moonGlow = pow(max(0.0, moonCos), 400.0) * 0.25;
            col += uMoonColor * (moonDisk * mix(0.15, 1.6, phase) + moonGlow) * uStars;
        }

        // Звёзды — только там, где темно, и только ночью. Шум по направлению
        // луча: звёзды обязаны стоять на месте при повороте камеры, а не
        // мерцать вместе с ней, поэтому решётка берётся в МИРОВОМ направлении.
        if (uStars > 0.01 && dir.y > -0.05) {
            vec3 g = floor(dir * 260.0);
            float h = fract(sin(dot(g, vec3(12.9898, 78.233, 37.719))) * 43758.5453);
            float star = smoothstep(0.9965, 1.0, h);
            col += vec3(star) * uStars * smoothstep(-0.05, 0.25, dir.y) * 1.2;
        }
    }

    FragColor = vec4(col, 1.0);
}
)GLSL";

} // namespace

SkyCelestials CelestialsFromEnvironment(const LightingEnvironment& env) {
    SkyCelestials c;
    c.Enabled = env.Skybox.Celestials;
    // У направленного света хранится, КУДА он светит; нарисовать солнце надо
    // там, ОТКУДА — то есть в противоположной стороне.
    const glm::vec3 d = env.Sun.Direction;
    c.SunDir = glm::length(d) > 1e-6f ? -glm::normalize(d) : glm::vec3(0.0f, 1.0f, 0.0f);
    c.SunColor = env.Skybox.SunColor;
    c.SunSize = env.Skybox.SunSize;
    c.Moon = env.Skybox.Moon;
    c.MoonColor = env.Skybox.MoonColor;
    c.MoonSize = env.Skybox.MoonSize;
    c.StarIntensity = env.Skybox.StarIntensity;
    return c;
}

SkyRenderer::SkyRenderer() {
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    m_shader = device.CreateShaderProgram(kVertexSrc, kFragmentSrc);
    // Геометрия без атрибутов — вершины считаются из gl_VertexID (см. VS).
    m_geometry = device.CreateGeometry(sage::rhi::VertexLayout{});
}

void SkyRenderer::Draw(const glm::mat4& view, const glm::mat4& proj,
                       glm::vec3 topColor, glm::vec3 horizonColor, const SkyCelestials& sky) {
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();

    m_shader->Use();
    m_shader->SetMat4("uInvProj", glm::inverse(proj));
    // view — ортонормальная (R*T): view->world поворот = transpose(mat3(view)),
    // упаковываем в mat4 (у ShaderProgram нет SetMat3; VS берёт mat3(...)).
    m_shader->SetMat4("uInvViewRot", glm::mat4(glm::transpose(glm::mat3(view))));
    m_shader->SetVec3("uTop", topColor);
    m_shader->SetVec3("uHorizon", horizonColor);
    m_shader->SetFloat("uCelestials", sky.Enabled ? 1.0f : 0.0f);
    const glm::vec3 sunDir = glm::length(sky.SunDir) > 1e-6f ? glm::normalize(sky.SunDir)
                                                             : glm::vec3(0.0f, 1.0f, 0.0f);
    m_shader->SetVec3("uSunDir", sunDir);
    m_shader->SetVec3("uSunColor", sky.SunColor);
    m_shader->SetFloat("uSunSize", sky.SunSize);
    m_shader->SetFloat("uMoon", sky.Moon ? 1.0f : 0.0f);
    m_shader->SetVec3("uMoonColor", sky.MoonColor);
    m_shader->SetFloat("uMoonSize", sky.MoonSize);
    // Звёзды и луна проступают по мере ухода солнца под горизонт. Считается
    // здесь, а не в игре: это свойство неба, а не игровой логики.
    const float night = glm::clamp(-sunDir.y * 4.0f + 0.25f, 0.0f, 1.0f);
    m_shader->SetFloat("uStars", night * sky.StarIntensity);

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
