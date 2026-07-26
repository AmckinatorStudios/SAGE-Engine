#include "sage/render/GridRenderer.h"

#include <algorithm>

#include <glm/gtc/matrix_inverse.hpp>

#include "sage/render/Shader.h"
#include "sage/rhi/GraphicsDevice.h"

namespace sage::render {

namespace {

// Полноэкранный треугольник без вершинных данных: позиция выводится из
// gl_VertexID. Тот же приём, что в PostFX, — ни буфера, ни атрибутов.
const char* kGridVert = R"(#version 330 core
out vec2 vNdc;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    vNdc = p * 2.0 - 1.0;
    gl_Position = vec4(vNdc, 0.0, 1.0);
}
)";

const char* kGridFrag = R"(#version 330 core
in vec2 vNdc;
out vec4 FragColor;

uniform mat4 uInvViewProj;  // NDC -> мир
uniform mat4 uViewProj;     // мир -> clip (для записи глубины)
uniform vec3 uCameraPos;
uniform float uHeight;      // высота плоскости по Y

uniform float uCellSize;
uniform float uMajorEvery;
uniform vec3 uMinorColor;
uniform vec3 uMajorColor;
uniform vec3 uXAxisColor;
uniform vec3 uZAxisColor;
uniform int uShowAxes;
uniform float uOpacity;

uniform int uRadiusMode;    // 1 — обрезать по радиусу
uniform float uRadius;
uniform float uFadeDistance;

// Покрытие линии для сетки с шагом cell. Ключевой приём: делим расстояние до
// ближайшей линии на ПРОИЗВОДНУЮ координаты (fwidth). Тогда толщина линии
// получается в пикселях экрана, а не в единицах мира: у горизонта, где клетка
// сжимается в доли пикселя, линия не превращается в рябь, а плавно гаснет.
float GridCoverage(vec2 pos, float cell) {
    vec2 coord = pos / cell;
    vec2 derivative = fwidth(coord);
    // Расстояние до ближайшей целой линии в единицах производной.
    vec2 grid = abs(fract(coord - 0.5) - 0.5) / max(derivative, vec2(1e-6));
    float line = min(grid.x, grid.y);
    return 1.0 - min(line, 1.0);
}

// Покрытие одной оси (линия x == 0 или z == 0) той же логикой.
float AxisCoverage(float pos, float derivative) {
    return 1.0 - min(abs(pos) / max(derivative, 1e-6), 1.0);
}

void main() {
    // Луч из камеры через пиксель: две точки на ближней и дальней плоскостях,
    // развёрнутые обратно в мир.
    vec4 nearPoint = uInvViewProj * vec4(vNdc, -1.0, 1.0);
    vec4 farPoint  = uInvViewProj * vec4(vNdc,  1.0, 1.0);
    vec3 ro = nearPoint.xyz / nearPoint.w;
    vec3 rd = normalize(farPoint.xyz / farPoint.w - ro);

    // Пересечение с горизонтальной плоскостью. Луч почти параллелен ей —
    // пересечения нет (или оно бесконечно далеко), пиксель пропускаем.
    if (abs(rd.y) < 1e-5) discard;
    float t = (uHeight - ro.y) / rd.y;
    if (t <= 0.0) discard; // плоскость позади камеры

    vec3 world = ro + rd * t;
    vec2 plane = world.xz;

    // Две частоты: мелкая клетка и крупная. Мелкая гаснет с расстоянием
    // раньше — так сетка не превращается в сплошную заливку у горизонта.
    float minor = GridCoverage(plane, uCellSize);
    float major = GridCoverage(plane, uCellSize * uMajorEvery);

    // Затухание по расстоянию от камеры вдоль плоскости.
    float distance = length(world - uCameraPos);
    float fade = uFadeDistance > 0.0
        ? clamp(1.0 - distance / uFadeDistance, 0.0, 1.0)
        : 1.0;
    // Квадрат делает переход мягче: линейное затухание обрывается заметной
    // границей, за которой сетка исчезает разом.
    fade = fade * fade;

    // Мелкая сетка гаснет вдвое быстрее крупной.
    float minorFade = fade * fade;

    vec3 color = uMinorColor;
    float alpha = minor * minorFade * 0.75;
    if (major > 0.001) {
        color = mix(color, uMajorColor, major);
        alpha = max(alpha, major * fade);
    }

    if (uShowAxes == 1) {
        vec2 derivative = fwidth(plane);
        float axisZ = AxisCoverage(plane.x, derivative.x); // линия x=0 идёт вдоль Z
        float axisX = AxisCoverage(plane.y, derivative.y); // линия z=0 идёт вдоль X
        if (axisX > 0.001) { color = mix(color, uXAxisColor, axisX); alpha = max(alpha, axisX * fade); }
        if (axisZ > 0.001) { color = mix(color, uZAxisColor, axisZ); alpha = max(alpha, axisZ * fade); }
    }

    if (uRadiusMode == 1) {
        // Мягкий край: жёсткая обрезка дала бы рваную окружность из ступенек.
        float edge = 1.0 - smoothstep(uRadius * 0.88, uRadius, length(plane));
        alpha *= edge;
    }

    alpha *= uOpacity;
    if (alpha < 0.002) discard; // пустые пиксели не должны писать глубину

    // Глубина ТОЧКИ ПЕРЕСЕЧЕНИЯ, а не плоскости треугольника: только так сетка
    // корректно уходит под геометрию, а не ложится поверх всей сцены.
    //
    // Минус крошечное смещение к камере: сетку почти всегда ставят на том же
    // уровне, что и пол сцены, и без смещения две совпадающие плоскости
    // борются за глубину — сетка проступает рваными пятнами. Смещение меньше
    // любого осмысленного зазора между объектами, поэтому настоящая геометрия
    // перед сеткой по-прежнему её перекрывает.
    vec4 clip = uViewProj * vec4(world, 1.0);
    gl_FragDepth = (clip.z / clip.w) * 0.5 + 0.5 - 2e-5;

    FragColor = vec4(color, alpha);
}
)";

Shader& GridShader() {
    // Намеренно НЕ уничтожается: деструктор статика сработал бы уже после
    // разрушения GL-контекста (тот же случай, что у шейдеров SkinnedModel).
    static Shader* shader = new Shader(Shader::FromSource(kGridVert, kGridFrag, "Grid"));
    return *shader;
}

} // namespace

void GridRenderer::EnsureGeometry() {
    if (m_fsTri) return;
    sage::rhi::VertexLayout layout; // без атрибутов — позиция из gl_VertexID
    m_fsTri = sage::rhi::GraphicsDevice::Get().CreateGeometry(layout);
}

void GridRenderer::Draw(const glm::mat4& view, const glm::mat4& proj, const glm::vec3& cameraPos,
                        const GridSettings& s) {
    if (!s.Enabled || s.CellSize <= 0.0001f) return;
    EnsureGeometry();

    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    const glm::mat4 viewProj = proj * view;

    Shader& shader = GridShader();
    shader.Use();
    shader.SetMat4("uInvViewProj", glm::inverse(viewProj));
    shader.SetMat4("uViewProj", viewProj);
    shader.SetVec3("uCameraPos", cameraPos);
    shader.SetFloat("uHeight", s.Height);
    shader.SetFloat("uCellSize", s.CellSize);
    shader.SetFloat("uMajorEvery", (float)std::max(s.MajorEvery, 2));
    shader.SetVec3("uMinorColor", s.MinorColor);
    shader.SetVec3("uMajorColor", s.MajorColor);
    shader.SetVec3("uXAxisColor", s.XAxisColor);
    shader.SetVec3("uZAxisColor", s.ZAxisColor);
    shader.SetInt("uShowAxes", s.ShowAxes ? 1 : 0);
    shader.SetFloat("uOpacity", std::clamp(s.Opacity, 0.0f, 1.0f));
    shader.SetInt("uRadiusMode", s.Mode == GridSettings::Extent::Radius ? 1 : 0);
    shader.SetFloat("uRadius", std::max(s.Radius, 0.01f));

    // Автоматическая дальность затухания — от высоты камеры над плоскостью.
    // Фиксированное число одинаково плохо в обоих концах: с высоты птичьего
    // полёта сетка обрывалась бы под ногами, а вплотную к полу тянулась бы за
    // горизонт сплошной кашей.
    //
    // Затухание нужно ТОЛЬКО бесконечной сетке: оно существует ради горизонта,
    // где клетка сжимается в рябь. У сетки с радиусом горизонта нет — её
    // обрезает край, и лишнее затухание поверх него гасило бы всю площадку
    // целиком, стоит отвести камеру на пару радиусов. Ноль здесь означает «не
    // гасить»: край делает smoothstep в шейдере.
    float fade = s.FadeDistance;
    if (s.Mode == GridSettings::Extent::Infinite && fade <= 0.0f) {
        const float height = std::abs(cameraPos.y - s.Height);
        fade = std::clamp(height * 12.0f, 30.0f, 4000.0f);
    }
    shader.SetFloat("uFadeDistance", fade);

    // Сетка полупрозрачна, поэтому смешивается; глубину ПИШЕТ (иначе она не
    // перекрывала бы то, что за ней), но пишет из точки пересечения.
    device.SetBlend(true);
    device.SetDepthTest(true);
    device.SetDepthWrite(true);
    m_fsTri->DrawArrays(3);
    device.SetBlend(false);
}

} // namespace sage::render
