#include "ShaderTemplates.h"

#include <fstream>

#include <nlohmann/json.hpp>

#include "Localization.h"
#include "sage/core/Paths.h"
#include "sage/render/ShaderParams.h"

namespace fs = std::filesystem;

namespace sage::editor::shadertemplates {
namespace {

// --- Общий вершинный шейдер материала ---------------------------------------
// Контракт движка (docs/custom_shaders.md): объект приходит ИНСТАНСНЫМИ
// атрибутами (матрица 4..7, цвет 8, metallic 9, roughness 10, альфа 12), а не
// через uModel — иначе тысяча объектов не была бы одним вызовом отрисовки.
const char* kLitVert = R"GLSL(#version 330 core
// Вершинный шейдер материала SAGE. Локации атрибутов задаёт движок.
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aUV;
layout (location = 4) in vec4 iM0;
layout (location = 5) in vec4 iM1;
layout (location = 6) in vec4 iM2;
layout (location = 7) in vec4 iM3;
layout (location = 8) in vec3 iColor;      // albedo материала × цвет объекта (линейный)
layout (location = 9) in float iMetallic;
layout (location = 10) in float iRoughness;
layout (location = 12) in float iAlpha;

uniform mat4 uView;
uniform mat4 uProjection;
uniform float uTime;

out vec3 FragPos;
out vec3 Normal;
out vec2 vUV;
out vec3 vColor;
out float vMetallic;
out float vRoughness;
out float vAlpha;

void main() {
    mat4 model = mat4(iM0, iM1, iM2, iM3);
    vec4 world = model * vec4(aPos, 1.0);
    FragPos = world.xyz;
    Normal = mat3(transpose(inverse(model))) * aNormal;
    vUV = aUV;
    vColor = iColor;
    vMetallic = iMetallic;
    vRoughness = iRoughness;
    vAlpha = iAlpha;
    gl_Position = uProjection * uView * world;
}
)GLSL";

const char* kLitFrag = R"GLSL(#version 330 core
// Освещённый материал SAGE: свой цвет и своя логика поверх штатного PBR —
// солнце с тенями, лампы, отражения, GI и туман движка считает ShadePBR.
in vec3 FragPos;
in vec3 Normal;
in vec2 vUV;
in vec3 vColor;
in float vMetallic;
in float vRoughness;
in float vAlpha;
layout (location = 0) out vec4 FragColor;

// Параметры: строка uniform + подсказка в комментарии — инспектор материала
// покажет их сам. Формат: [color] умолчание [мин, макс] подпись.
uniform vec3 uTint;          // color 1 1 1 Оттенок
uniform float uRim;          // 0.35 [0, 2] Свечение по краю
uniform vec3 uRimColor;      // color 0.55 0.75 1.0 Цвет края

#include <sage_pbr>

void main() {
    vec3 N = normalize(gl_FrontFacing ? Normal : -Normal);
    vec3 V = normalize(uViewPos - FragPos);
    vec3 color = ShadePBR(N, FragPos, vColor * uTint, vMetallic, vRoughness);
    // Своё поверх освещения: контур по краю силуэта.
    float rim = pow(1.0 - max(dot(N, V), 0.0), 3.0) * uRim;
    FragColor = vec4(color + uRimColor * rim, vAlpha);
}
)GLSL";

// --- Вода ---------------------------------------------------------------------
// Волны — сумма трёх синусоид в МИРОВЫХ координатах: соседние плитки воды
// стыкуются без швов. Нормаль считается в ПИКСЕЛЕ, а не в вершине: у плоскости
// четыре вершины, и волна, посчитанная только в них, была бы не видна вовсе.
const char* kWaterVert = R"GLSL(#version 330 core
// Вода SAGE: вершины поднимаются волной (видно на плотной сетке), нормаль —
// в пиксельном шейдере (видно на любой).
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aUV;
layout (location = 4) in vec4 iM0;
layout (location = 5) in vec4 iM1;
layout (location = 6) in vec4 iM2;
layout (location = 7) in vec4 iM3;
layout (location = 8) in vec3 iColor;
layout (location = 12) in float iAlpha;

uniform mat4 uView;
uniform mat4 uProjection;
uniform float uTime;
uniform float uWaveHeight;   // 0.25 [0, 2] Высота волн
uniform float uWaveLength;   // 7 [0.5, 40] Длина волны, м
uniform float uWaveSpeed;    // 1 [0, 4] Скорость волн

out vec3 FragPos;
out vec3 Normal;
out vec3 vColor;
out float vAlpha;

float WaveHeight(vec2 p) {
    float h = 0.0;
    vec2 dirs[3] = vec2[3](vec2(1.0, 0.2), vec2(-0.4, 1.0), vec2(0.7, -0.6));
    float scale[3] = float[3](1.0, 0.61, 0.37);
    for (int i = 0; i < 3; ++i) {
        float k = 6.2831853 / (uWaveLength * scale[i]);
        float phase = dot(normalize(dirs[i]), p) * k + uTime * uWaveSpeed * sqrt(9.8 * k);
        h += uWaveHeight * scale[i] * sin(phase);
    }
    return h;
}

void main() {
    mat4 model = mat4(iM0, iM1, iM2, iM3);
    vec4 world = model * vec4(aPos, 1.0);
    world.y += WaveHeight(world.xz);
    FragPos = world.xyz;
    Normal = normalize(mat3(transpose(inverse(model))) * aNormal);
    vColor = iColor;
    vAlpha = iAlpha;
    gl_Position = uProjection * uView * world;
}
)GLSL";

const char* kWaterFrag = R"GLSL(#version 330 core
// Вода SAGE: волновая нормаль в каждом пикселе, отражение неба по Френелю,
// блик солнца, цвет глубины у взгляда сверху и прозрачность у края.
in vec3 FragPos;
in vec3 Normal;
in vec3 vColor;
in float vAlpha;
layout (location = 0) out vec4 FragColor;

uniform float uTime;
uniform float uWaveHeight;   // 0.25 [0, 2] Высота волн
uniform float uWaveLength;   // 7 [0.5, 40] Длина волны, м
uniform float uWaveSpeed;    // 1 [0, 4] Скорость волн
uniform float uRipple;       // 0.5 [0, 1] Мелкая рябь
uniform vec3 uDeepColor;     // color 0.015 0.10 0.16 Цвет глубины
uniform vec3 uShallowColor;  // color 0.06 0.32 0.36 Цвет у поверхности
uniform float uClarity;      // 0.3 [0, 1] Прозрачность сверху

#include <sage_pbr>

// Наклон суммы волн (производная высоты): из него — нормаль поверхности.
vec2 WaveSlope(vec2 p, float lengthScale, float heightScale) {
    vec2 slope = vec2(0.0);
    vec2 dirs[3] = vec2[3](vec2(1.0, 0.2), vec2(-0.4, 1.0), vec2(0.7, -0.6));
    float scale[3] = float[3](1.0, 0.61, 0.37);
    for (int i = 0; i < 3; ++i) {
        vec2 d = normalize(dirs[i]);
        float k = 6.2831853 / (uWaveLength * scale[i] * lengthScale);
        float phase = dot(d, p) * k + uTime * uWaveSpeed * sqrt(9.8 * k);
        slope += d * (uWaveHeight * heightScale * scale[i] * k * cos(phase));
    }
    return slope;
}

void main() {
    // Крупные волны + мелкая рябь (те же волны в восемь раз мельче).
    vec2 slope = WaveSlope(FragPos.xz, 1.0, 1.0) + WaveSlope(FragPos.xz * 1.37, 0.125, 0.6 * uRipple);
    vec3 up = normalize(gl_FrontFacing ? Normal : -Normal);
    vec3 N = normalize(up + vec3(-slope.x, 0.0, -slope.y));
    vec3 V = normalize(uViewPos - FragPos);
    float ndv = clamp(dot(N, V), 0.0, 1.0);

    // Сверху вода тёмная и прозрачная, у горизонта — светлая и зеркальная.
    float fresnel = 0.02 + 0.98 * pow(1.0 - ndv, 5.0);
    vec3 body = mix(uDeepColor, uShallowColor, pow(1.0 - ndv, 2.0)) * vColor;
    // Почти зеркальная гладь: блик солнца и отражение окружения — от ShadePBR.
    vec3 lit = ShadePBR(N, FragPos, body, 0.0, 0.04);
    float alpha = mix(1.0 - uClarity, 1.0, fresnel) * vAlpha;
    FragColor = vec4(lit, clamp(alpha, 0.0, 1.0));
}
)GLSL";

// --- Без освещения --------------------------------------------------------------
const char* kUnlitFrag = R"GLSL(#version 330 core
// Материал без освещения: цвет как есть — для светящихся вывесок, голограмм,
// стилизации. Тени и лампы его не трогают.
in vec3 FragPos;
in vec3 Normal;
in vec2 vUV;
in vec3 vColor;
in float vMetallic;
in float vRoughness;
in float vAlpha;
layout (location = 0) out vec4 FragColor;

uniform float uTime;
uniform vec3 uColor;         // color 1 0.55 0.15 Цвет
uniform float uBrightness;   // 2 [0, 10] Яркость
uniform float uPulse;        // 0.5 [0, 4] Пульсация, раз в секунду
uniform float uStripes;      // 0 [0, 40] Полосы голограммы

void main() {
    float pulse = 1.0 + 0.35 * sin(uTime * uPulse * 6.2831853);
    float stripes = uStripes > 0.0 ? 0.65 + 0.35 * step(0.5, fract(FragPos.y * uStripes - uTime)) : 1.0;
    FragColor = vec4(vColor * uColor * uBrightness * pulse * stripes, vAlpha);
}
)GLSL";

// --- Шейдер экрана --------------------------------------------------------------
const char* kScreenFrag = R"GLSL(#version 330 core
// Шейдер экрана SAGE — звено «Свой шейдер» пост-обработки камеры.
// Вход: готовый кадр (после тон-маппинга), глубина, время и четыре числа
// звена из инспектора: «Сила» (uStrength), «A», «B» и «Оттенок».
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uScene;     // кадр
uniform sampler2D uDepth;     // глубина сцены (0..1)
uniform float uTime;          // секунды
uniform vec2 uResolution;     // размер кадра в пикселях
uniform float uStrength;      // «Сила»: 0 — эффект выключен, 1 — полностью
uniform float uParamA;        // «A»: здесь — размер пикселя
uniform float uParamB;        // «B»: здесь — линии развёртки
uniform vec4 uTint;           // «Оттенок»

void main() {
    vec3 original = texture(uScene, vUV).rgb;
    // Пиксель-арт: кадр крупными клетками.
    float px = mix(1.0, 12.0, uParamA);
    vec2 cell = (floor(vUV * uResolution / px) + 0.5) * px / uResolution;
    vec3 c = texture(uScene, cell).rgb;
    // Линии развёртки старого экрана.
    float scan = 1.0 - uParamB * 0.4 * (0.5 + 0.5 * sin(vUV.y * uResolution.y * 3.14159));
    vec3 styled = c * scan * uTint.rgb;
    FragColor = vec4(mix(original, styled, uStrength), 1.0);
}
)GLSL";

bool WriteText(const fs::path& path, const std::string& text, std::string& error) {
    std::error_code ec;
    if (fs::exists(path, ec)) {
        error = T("A file with this name already exists: ") + sage::PathToUtf8(path.filename());
        return false;
    }
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) {
        error = T("Could not write: ") + sage::PathToUtf8(path);
        return false;
    }
    out << text;
    return true;
}

std::string Rel(const fs::path& p, const fs::path& root) {
    std::error_code ec;
    const fs::path r = fs::relative(p, root, ec);
    return sage::PathToUtf8(ec || r.empty() ? p : r);
}

} // namespace

const std::vector<Template>& All() {
    static const std::vector<Template> list = {
        {"water", "Water", "Waves, sky reflections, transparency", "drop",
         Target::Material, kWaterVert, kWaterFrag,
         R"({"albedo": [1, 1, 1], "opacity": 0.85, "roughness": 0.04, "metallic": 0.0, "cull": "back"})"},
        {"lit", "Lit material", "Your logic over the engine lighting", "light",
         Target::Material, kLitVert, kLitFrag,
         R"({"albedo": [0.8, 0.8, 0.8], "opacity": 1.0, "roughness": 0.5, "metallic": 0.0, "cull": "back"})"},
        {"unlit", "Unlit material", "Glowing signs, holograms, stylization", "sun", Target::Material,
         kLitVert, kUnlitFrag,
         R"({"albedo": [1, 1, 1], "opacity": 1.0, "roughness": 1.0, "metallic": 0.0, "cull": "back"})"},
        {"screen", "Screen effect", "Camera post-processing: pixel art, CRT",
         "camera", Target::Screen, nullptr, kScreenFrag, nullptr},
    };
    return list;
}

const Template* Find(const std::string& id) {
    for (const Template& t : All())
        if (id == t.Id) return &t;
    return nullptr;
}

bool Create(const Template& t, const std::string& name, const fs::path& projectDir, const fs::path& assetsDir,
            fs::path& created, std::string& error) {
    if (name.empty()) {
        error = T("Enter a name");
        return false;
    }
    std::error_code ec;
    const fs::path shaders = assetsDir / "shaders";
    fs::create_directories(shaders, ec);
    const fs::path frag = shaders / sage::PathFromUtf8(name + ".frag");
    if (t.Kind == Target::Screen) {
        if (!WriteText(frag, t.Fragment, error)) return false;
        created = frag;
        return true;
    }

    const fs::path vert = shaders / sage::PathFromUtf8(name + ".vert");
    const fs::path materials = assetsDir / "materials";
    fs::create_directories(materials, ec);
    const fs::path mat = materials / sage::PathFromUtf8(name + ".sagemat");
    if (fs::exists(vert, ec) || fs::exists(frag, ec) || fs::exists(mat, ec)) {
        error = T("A file with this name already exists: ") + name;
        return false;
    }
    nlohmann::json m = nlohmann::json::parse(t.MaterialJson, nullptr, false);
    if (!m.is_object()) m = nlohmann::json::object();
    m["vertexShader"] = Rel(vert, projectDir);
    m["fragmentShader"] = Rel(frag, projectDir);
    // Параметры — сразу со значениями из подсказок шейдера: материал выглядит
    // так, как задумано шаблоном, а не нулями.
    nlohmann::json params = nlohmann::json::object();
    for (const char* src : {t.Vertex, t.Fragment}) {
        for (const sage::render::ShaderUniformInfo& u : sage::render::ParseShaderUniforms(src)) {
            const int n = u.Type == ShaderParam::Type::Float ? 1 : u.Type == ShaderParam::Type::Vec2 ? 2
                        : u.Type == ShaderParam::Type::Vec3 ? 3 : 4;
            if (n == 1) params[u.Name] = u.Default.x;
            else {
                nlohmann::json arr = nlohmann::json::array();
                for (int i = 0; i < n; ++i) arr.push_back(u.Default[i]);
                params[u.Name] = arr;
            }
        }
    }
    m["params"] = params;
    if (!WriteText(vert, t.Vertex, error) || !WriteText(frag, t.Fragment, error) ||
        !WriteText(mat, m.dump(2) + "\n", error))
        return false;
    created = mat;
    return true;
}

} // namespace sage::editor::shadertemplates
