#include "sage/render/ShaderParams.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <set>
#include <sstream>

#include "sage/render/PbrShader.h"

namespace sage::render {
namespace {

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

int Components(ShaderParam::Type t) {
    switch (t) {
        case ShaderParam::Type::Float: return 1;
        case ShaderParam::Type::Vec2: return 2;
        case ShaderParam::Type::Vec3: return 3;
        case ShaderParam::Type::Vec4: return 4;
    }
    return 1;
}

// Имена юниформ, объявленных в общем блоке PBR движка, — один раз.
const std::set<std::string>& PbrBlockUniforms() {
    static const std::set<std::string> names = [] {
        std::set<std::string> out;
        std::istringstream in(kPbrSharedGlsl);
        std::string line;
        while (std::getline(in, line)) {
            const std::string t = Trim(line);
            if (t.rfind("uniform ", 0) != 0) continue;
            std::istringstream ws(t);
            std::string kw, type, name;
            ws >> kw >> type >> name;
            while (!name.empty() && (name.back() == ';' || name.back() == ']')) {
                const size_t br = name.find('[');
                name = br == std::string::npos ? name.substr(0, name.size() - 1) : name.substr(0, br);
            }
            if (!name.empty()) out.insert(name);
        }
        return out;
    }();
    return names;
}

} // namespace

bool IsEngineUniform(const std::string& name) {
    static const std::set<std::string> contract = {
        "uView", "uProjection", "uModel", "uViewPos", "uTime", "uShadingMode", "uAlbedoFactor", "uMetallic",
        "uRoughness", "uOpacity", "uAlphaCutoff", "uTranslucency", "uLightmapEnabled", "uLightmap", "uUnlit",
        "uEmissive", "uUVOffset",
        // Шейдер экрана (звено «Свой шейдер» пост-обработки).
        "uScene", "uDepth", "uTexel", "uResolution", "uStrength", "uParamA", "uParamB", "uTint",
        "uProj", "uInvProj"};
    return contract.count(name) > 0 || PbrBlockUniforms().count(name) > 0;
}

std::string UniformLabel(const std::string& name) {
    std::string n = name;
    if (n.size() > 1 && n[0] == 'u' && std::isupper((unsigned char)n[1])) n = n.substr(1);
    std::string out;
    for (size_t i = 0; i < n.size(); ++i) {
        const char c = n[i];
        if (c == '_') {
            out += ' ';
            continue;
        }
        if (i > 0 && std::isupper((unsigned char)c) && std::islower((unsigned char)n[i - 1])) out += ' ';
        out += c;
    }
    return out;
}

std::vector<ShaderUniformInfo> ParseShaderUniforms(const std::string& source) {
    std::vector<ShaderUniformInfo> out;
    std::set<std::string> seen;
    std::istringstream in(source);
    std::string line;
    while (std::getline(in, line)) {
        std::string code = line, comment;
        const size_t cpos = line.find("//");
        if (cpos != std::string::npos) {
            code = line.substr(0, cpos);
            comment = Trim(line.substr(cpos + 2));
        }
        code = Trim(code);
        if (code.rfind("uniform ", 0) != 0) continue;
        std::istringstream ws(code);
        std::string kw, type, name;
        ws >> kw >> type >> name;
        // Точность (highp/mediump/lowp) перед типом.
        if (type == "highp" || type == "mediump" || type == "lowp") {
            type = name;
            ws >> name;
        }
        while (!name.empty() && name.back() == ';') name.pop_back();
        if (name.empty() || name.find('[') != std::string::npos) continue; // массивы — не параметр
        ShaderUniformInfo info;
        if (type == "float") info.Type = ShaderParam::Type::Float;
        else if (type == "vec2") info.Type = ShaderParam::Type::Vec2;
        else if (type == "vec3") info.Type = ShaderParam::Type::Vec3;
        else if (type == "vec4") info.Type = ShaderParam::Type::Vec4;
        else continue; // сэмплеры, матрицы, int — не параметры материала
        if (IsEngineUniform(name) || !seen.insert(name).second) continue;
        info.Name = name;

        // Подсказка: [color] числа [min, max] подпись.
        std::istringstream cs(comment);
        std::string tok;
        std::string label;
        int filled = 0;
        const int need = Components(info.Type);
        bool inLabel = false;
        while (cs >> tok) {
            if (inLabel) {
                label += (label.empty() ? "" : " ") + tok;
                continue;
            }
            if (tok == "color" && filled == 0 && need >= 3) {
                info.Color = true;
                continue;
            }
            if (tok[0] == '[') {
                // Диапазон может быть «[0,» «2]» или «[0, 2]» или «[0,2]».
                std::string range = tok;
                while (range.find(']') == std::string::npos && cs >> tok) range += " " + tok;
                for (char& ch : range)
                    if (ch == '[' || ch == ']' || ch == ',') ch = ' ';
                std::istringstream rs(range);
                float a = 0.0f, b = 1.0f;
                if (rs >> a >> b) {
                    info.Min = a;
                    info.Max = b;
                    info.HasRange = true;
                }
                continue;
            }
            char* end = nullptr;
            const float v = std::strtof(tok.c_str(), &end);
            if (end && *end == '\0' && filled < need) {
                info.Default[filled++] = v;
                continue;
            }
            inLabel = true;
            label = tok;
        }
        // Одно число на вектор — во все компоненты («0.5» у vec3 — серый).
        if (filled == 1 && need > 1)
            for (int i = 1; i < need; ++i) info.Default[i] = info.Default[0];
        if (info.Color && filled == 0) info.Default = glm::vec4(1.0f);
        if (info.Type == ShaderParam::Type::Vec4 && info.Color && filled < 4) info.Default.w = 1.0f;
        if (!info.HasRange && info.Type == ShaderParam::Type::Float) {
            // Без диапазона — ползунок, который вмещает умолчание.
            info.Min = std::min(0.0f, info.Default.x);
            info.Max = std::max(1.0f, info.Default.x * 2.0f);
        }
        info.Label = label.empty() ? UniformLabel(name) : label;
        out.push_back(std::move(info));
    }
    return out;
}

} // namespace sage::render
