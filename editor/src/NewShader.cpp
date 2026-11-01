#include "NewShader.h"

#include <fstream>

#include <nlohmann/json.hpp>

#include "Localization.h"
#include "sage/core/Paths.h"

namespace fs = std::filesystem;

namespace sage::editor::newshader {
namespace {

// Контракт движка (docs/custom_shaders.md): объект приходит ИНСТАНСНЫМИ
// атрибутами (матрица 4..7, цвет 8, metallic 9, roughness 10, альфа 12), а не
// через uModel — иначе тысяча объектов не была бы одним вызовом отрисовки.
const char* kVert = R"GLSL(#version 330 core
// Вершинный шейдер материала. Локации атрибутов задаёт движок
// (docs/custom_shaders.md); всё остальное — ваше.
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aUV;
layout (location = 4) in vec4 iM0;
layout (location = 5) in vec4 iM1;
layout (location = 6) in vec4 iM2;
layout (location = 7) in vec4 iM3;
layout (location = 12) in float iAlpha;

uniform mat4 uView;
uniform mat4 uProjection;
uniform float uTime;

out vec3 FragPos;
out vec3 Normal;
out vec2 vUV;
out float vAlpha;

void main() {
    mat4 model = mat4(iM0, iM1, iM2, iM3);
    vec4 world = model * vec4(aPos, 1.0);
    FragPos = world.xyz;
    Normal = mat3(transpose(inverse(model))) * aNormal;
    vUV = aUV;
    vAlpha = iAlpha;
    gl_Position = uProjection * uView * world;
}
)GLSL";

// Пустой пиксельный: один цвет-параметр и освещение движка. Как объявить свои
// настройки — в комментарии; что рисовать — решает автор.
const char* kFrag = R"GLSL(#version 330 core
in vec3 FragPos;
in vec3 Normal;
in vec2 vUV;
in float vAlpha;
layout (location = 0) out vec4 FragColor;

// Настройки материала — строками uniform; инспектор покажет их сам:
//   // @header Группа             — заголовок группы
//   uniform float uX; // 0.5 [0, 1] Подпись   — ползунок
//   uniform vec3 uC;  // color 1 1 1 Подпись  — цвет
//   uniform sampler2D uT; // Подпись          — текстура (linear — данные)
// Общий код — «#include "файл.glsl"» относительно этого файла.
uniform vec3 uColor; // color 0.8 0.8 0.8 Цвет
uniform float uOpacity; // от движка: «Непрозрачность» материала (меньше 1 — прозрачный проход)

// Освещение движка (солнце, тени, лампы, GI, туман): ShadePBR(...).
#include <sage_pbr>

void main() {
    vec3 N = normalize(gl_FrontFacing ? Normal : -Normal);
    FragColor = vec4(ShadePBR(N, FragPos, uColor, 0.0, 0.5), vAlpha * uOpacity);
}
)GLSL";

const char* kScreen = R"GLSL(#version 330 core
// Шейдер экрана — звено «Свой шейдер» пост-обработки камеры.
// Вход: готовый кадр (uScene), глубина (uDepth), время (uTime), размер
// (uResolution, uTexel) и числа звена из инспектора: uStrength, uParamA,
// uParamB, uTint. Свои параметры — строками uniform с подсказкой.
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uScene;
uniform float uStrength;

void main() {
    vec3 color = texture(uScene, vUV).rgb;
    FragColor = vec4(color, 1.0);
}
)GLSL";

bool WriteText(const fs::path& path, const std::string& text, std::string& error) {
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
    return (ec || r.empty() ? p : r).generic_string();
}

} // namespace

const char* MaterialVertex() { return kVert; }
const char* MaterialFragment() { return kFrag; }
const char* ScreenFragment() { return kScreen; }

bool Create(Kind kind, const std::string& name, const fs::path& projectDir, const fs::path& assetsDir,
            fs::path& created, std::string& error) {
    if (name.empty()) {
        error = T("Enter a name");
        return false;
    }
    std::error_code ec;
    const fs::path shaders = assetsDir / "shaders";
    fs::create_directories(shaders, ec);
    const fs::path frag = shaders / sage::PathFromUtf8(name + ".frag");
    const fs::path vert = shaders / sage::PathFromUtf8(name + ".vert");
    const fs::path mat = assetsDir / "materials" / sage::PathFromUtf8(name + ".sagemat");
    const bool taken = fs::exists(frag, ec) || (kind == Kind::Material && (fs::exists(vert, ec) || fs::exists(mat, ec)));
    if (taken) {
        error = T("A file with this name already exists: ") + name;
        return false;
    }
    if (kind == Kind::Screen) {
        if (!WriteText(frag, kScreen, error)) return false;
        created = frag;
        return true;
    }
    fs::create_directories(mat.parent_path(), ec);
    nlohmann::json m;
    m["vertexShader"] = Rel(vert, projectDir);
    m["fragmentShader"] = Rel(frag, projectDir);
    m["params"] = {{"uColor", {0.8, 0.8, 0.8}}};
    if (!WriteText(vert, kVert, error) || !WriteText(frag, kFrag, error) || !WriteText(mat, m.dump(2) + "\n", error))
        return false;
    created = mat;
    return true;
}

} // namespace sage::editor::newshader
