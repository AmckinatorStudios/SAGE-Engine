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
    // Только то, что движок заливает МАТЕРИАЛУ. Имена контракта шейдера экрана
    // (uScene, uDepth, uTexel, uResolution, uStrength, uParamA, uParamB, uTint,
    // uProj, uInvProj) сюда не входят: материалу их никто не задаёт, и
    // считать их служебными значило отнять у материала-шейдера его же
    // параметр. Так uTint материала оставался без умолчания из подсказки —
    // нулём, — и всё, что им умножалось, рисовалось чёрным, пока соседний
    // материал с тем же шейдером случайно не оставлял в программе своё
    // значение. Шейдеру экрана эти юниформы движок задаёт сам при каждом
    // проходе (PostFXPasses, «Свой шейдер»), поверх любых умолчаний.
    static const std::set<std::string> contract = {
        "uView", "uProjection", "uModel", "uViewPos", "uTime", "uShadingMode", "uAlbedoFactor", "uMetallic",
        "uRoughness", "uOpacity", "uAlphaCutoff", "uTranslucency", "uLightmapEnabled", "uLightmap", "uUnlit",
        "uEmissive", "uUVOffset"};
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
    std::string line, header;
    while (std::getline(in, line)) {
        std::string code = line, comment;
        const size_t cpos = line.find("//");
        if (cpos != std::string::npos) {
            code = line.substr(0, cpos);
            comment = Trim(line.substr(cpos + 2));
        }
        code = Trim(code);
        if (code.empty() && comment.rfind("@header", 0) == 0) {
            header = Trim(comment.substr(7));
            continue;
        }
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
        else if (type == "sampler2D") info.Texture = true;
        else continue; // матрицы, int, кубические карты — не параметры материала
        if (IsEngineUniform(name) || !seen.insert(name).second) continue;
        info.Name = name;
        info.Header = header;
        if (info.Texture) {
            std::istringstream cs(comment);
            std::string tok, label;
            while (cs >> tok) {
                if (tok == "linear" && label.empty()) info.Linear = true;
                else label += (label.empty() ? "" : " ") + tok;
            }
            info.Label = label.empty() ? UniformLabel(name) : label;
            out.push_back(std::move(info));
            continue;
        }

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

std::vector<ShaderUniformInfo> MergeShaderUniforms(const std::vector<std::vector<ShaderUniformInfo>>& sources) {
    std::vector<ShaderUniformInfo> merged;
    for (const std::vector<ShaderUniformInfo>& list : sources)
        for (const ShaderUniformInfo& u : list) {
            auto it = std::find_if(merged.begin(), merged.end(),
                                   [&](const ShaderUniformInfo& m) { return m.Name == u.Name; });
            if (it == merged.end()) {
                merged.push_back(u);
                continue;
            }
            if (it->Header.empty()) it->Header = u.Header;
            // Подпись «из имени» уступает написанной автором.
            if (it->Label == UniformLabel(it->Name) && u.Label != UniformLabel(u.Name)) it->Label = u.Label;
        }
    std::vector<std::string> order{""};
    for (const ShaderUniformInfo& u : merged)
        if (std::find(order.begin(), order.end(), u.Header) == order.end()) order.push_back(u.Header);
    std::stable_sort(merged.begin(), merged.end(), [&](const ShaderUniformInfo& a, const ShaderUniformInfo& b) {
        return std::find(order.begin(), order.end(), a.Header) < std::find(order.begin(), order.end(), b.Header);
    });
    return merged;
}

} // namespace sage::render
