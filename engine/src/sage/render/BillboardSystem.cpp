#include "sage/render/BillboardSystem.h"

// Встроенный шейдер билбордов. Тот же способ разворота к камере, что и у
// частиц (uCameraRight/uCameraUp), — поэтому спрайт и частица в одном кадре
// ведут себя одинаково и их можно ставить рядом.
//
// Отдельный от частиц шейдер, а не общий: у частицы параметры приходят
// ПОТОКОМ ИНСТАНСОВ (их тысячи и они меняются каждый кадр), у билборда —
// юниформами (их десятки и они стоят на месте). Свести их в один шейдер
// значило бы гонять инстанс-буфер ради трёх маркеров.
namespace {

const char* kBillboardVert = R"(#version 330 core
layout (location = 0) in vec2 aCorner;   // -0.5..0.5 угол квада
layout (location = 1) in vec2 aUV;

out vec2 vUV;

uniform mat4 uView;
uniform mat4 uProjection;
uniform vec3 uCameraRight;
uniform vec3 uCameraUp;
uniform vec3 uWorldPos;
uniform vec2 uSize;
uniform vec2 uPivot;      // сдвиг квада в его плоскости (низ спрайта на точке)
uniform float uRotation;

void main() {
    vec2 corner = aCorner + uPivot;
    float c = cos(uRotation), s = sin(uRotation);
    vec2 r = vec2(corner.x * c - corner.y * s, corner.x * s + corner.y * c);
    vec3 world = uWorldPos + uCameraRight * (r.x * uSize.x) + uCameraUp * (r.y * uSize.y);
    gl_Position = uProjection * uView * vec4(world, 1.0);
    vUV = aUV;
}
)";

const char* kBillboardFrag = R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uTexture;
uniform bool uUseTexture;
uniform vec4 uTint;

void main() {
    vec4 c = uTint;
    if (uUseTexture) c *= texture(uTexture, vUV);
    // Полностью прозрачные пиксели выбрасываем: маркер с текстурой-иконкой
    // иначе кладёт на сцену прямоугольник пустоты там, где у иконки фон.
    if (c.a < 0.004) discard;
    FragColor = c;
}
)";

} // namespace

Shader& BillboardSystem::BuiltinShader() {
    // Намеренно не уничтожается (как встроенный шейдер частиц): function-local
    // static Shader снёс бы GL-программу уже после разрушения контекста.
    static Shader* shader =
        new Shader(Shader::FromSource(kBillboardVert, kBillboardFrag, "BillboardSystem"));
    return *shader;
}
