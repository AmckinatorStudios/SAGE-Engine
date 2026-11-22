#define GLM_ENABLE_EXPERIMENTAL
#include "sage/assets/import/ImportSettings.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <unordered_map>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <nlohmann/json.hpp>

#include "sage/assets/Pack.h"
#include "sage/assets/import/MeshNormalize.h"
#include "sage/assets/import/ModelProbe.h"
#include "sage/core/Log.h"
#include "sage/core/Paths.h"

namespace sage::assets {

// ПУТЁМ, а не строкой: по ней открывают файл, а узкая строка на Windows
// читается как ANSI — у модели в папке с кириллицей настройки импорта молча
// не читались и не сохранялись (см. scripts/check_paths.py).
std::filesystem::path ImportSidecarPath(const std::string& modelPath) {
    std::filesystem::path p = sage::PathFromUtf8(modelPath);
    p += ".sageimport";
    return p;
}

ImportSettings LoadImportSettings(const std::string& modelPath) {
    ImportSettings s;
    // Через vfs: сайдкар едет в пакет вместе с моделью, и без него игра
    // собирала модель с другим масштабом и поворотом, чем редактор.
    std::string text;
    if (!vfs::ReadText(sage::PathToUtf8(ImportSidecarPath(modelPath)), text)) return s;
    try {
        nlohmann::json j = nlohmann::json::parse(text);
        s.Scale = j.value("scale", s.Scale);
        s.Recenter = j.value("recenter", s.Recenter);
        s.NormalizeSize = j.value("normalize", s.NormalizeSize);
        auto vec3 = [&j](const char* key, glm::vec3 fallback) {
            if (!j.contains(key) || !j[key].is_array() || j[key].size() != 3) return fallback;
            return glm::vec3(j[key][0].get<float>(), j[key][1].get<float>(), j[key][2].get<float>());
        };
        s.Rotation = vec3("rotation", s.Rotation);
        s.Offset = vec3("offset", s.Offset);
        // Режимы — СЛОВАМИ, а не номерами: файл читают глазами и правят руками,
        // а номер молча сменит смысл, если порядок в перечислении поменяется.
        const std::string normals = j.value("normals", std::string("import"));
        s.Normals = normals == "smooth" ? ImportSettings::NormalMode::Smooth
                  : normals == "flat"   ? ImportSettings::NormalMode::Flat
                                        : ImportSettings::NormalMode::Import;
        s.FlipUV = j.value("flipUV", s.FlipUV);
        s.FlipWinding = j.value("flipWinding", s.FlipWinding);
        s.ImportMaterials = j.value("importMaterials", s.ImportMaterials);
        const std::string alpha = j.value("alpha", std::string("auto"));
        s.Alpha = alpha == "opaque" ? ImportSettings::AlphaMode::Opaque
                : alpha == "cutout" ? ImportSettings::AlphaMode::Cutout
                                    : ImportSettings::AlphaMode::Auto;
        s.AlphaCutoff = j.value("alphaCutoff", s.AlphaCutoff);
        const std::string twoSided = j.value("twoSided", std::string("auto"));
        s.DoubleSided = twoSided == "on"  ? ImportSettings::TwoSided::On
                      : twoSided == "off" ? ImportSettings::TwoSided::Off
                                          : ImportSettings::TwoSided::Auto;
        s.ImportAnimation = j.value("importAnimation", s.ImportAnimation);
        s.Hierarchy = j.value("hierarchy", s.Hierarchy);
        s.AutoUnits = j.value("autoUnits", s.AutoUnits);
    } catch (const std::exception& e) {
        LOG_WARN("Model") << "Битый .sageimport (" << modelPath << "): " << e.what();
    }
    return s;
}

bool SaveImportSettings(const std::string& modelPath, const ImportSettings& s) {
    static const char* kNormals[] = {"import", "smooth", "flat"};
    static const char* kAlpha[] = {"auto", "opaque", "cutout"};
    static const char* kTwoSided[] = {"auto", "on", "off"};
    nlohmann::json j = {
        {"scale", s.Scale}, {"recenter", s.Recenter}, {"normalize", s.NormalizeSize},
        {"rotation", {s.Rotation.x, s.Rotation.y, s.Rotation.z}},
        {"offset", {s.Offset.x, s.Offset.y, s.Offset.z}},
        {"normals", kNormals[(int)s.Normals]},
        {"flipUV", s.FlipUV},
        {"flipWinding", s.FlipWinding},
        {"importMaterials", s.ImportMaterials},
        {"hierarchy", s.Hierarchy},
        {"alpha", kAlpha[(int)s.Alpha]},
        {"alphaCutoff", s.AlphaCutoff},
        {"twoSided", kTwoSided[(int)s.DoubleSided]},
        {"importAnimation", s.ImportAnimation},
        {"autoUnits", s.AutoUnits},
    };
    std::ofstream f(ImportSidecarPath(modelPath));
    if (!f) return false;
    f << j.dump(2) << "\n";
    return (bool)f;
}

void ApplyImportSettings(std::vector<Vertex>& vertices, const ImportSettings& s) {
    if (vertices.empty()) return;
    const bool needsBounds = s.Recenter || s.NormalizeSize;
    glm::vec3 lo(std::numeric_limits<float>::max()), hi(std::numeric_limits<float>::lowest());
    if (needsBounds) {
        for (const Vertex& v : vertices) { lo = glm::min(lo, v.Position); hi = glm::max(hi, v.Position); }
    }
    glm::vec3 center = needsBounds ? (lo + hi) * 0.5f : glm::vec3(0.0f);
    float normFactor = 1.0f;
    if (s.NormalizeSize) {
        glm::vec3 size = hi - lo;
        float maxDim = glm::max(size.x, glm::max(size.y, size.z));
        if (maxDim > 1e-6f) normFactor = 1.0f / maxDim;
    }
    float scale = normFactor * (s.Scale > 0.0f ? s.Scale : 1.0f);
    for (Vertex& v : vertices) {
        if (s.Recenter) v.Position -= center;   // центр AABB -> 0
        v.Position *= scale;                     // нормализация + равномерный масштаб
    }
}

float AutoUnitScale(const ImportSettings& s, bool hasSkeleton, float maxExtent) {
    return s.AutoUnits && hasSkeleton && maxExtent > kCentimetreGuess ? 0.01f : 1.0f;
}

void RobustBounds(const std::vector<PartBounds>& parts, glm::vec3& lo, glm::vec3& hi) {
    lo = glm::vec3(std::numeric_limits<float>::max());
    hi = glm::vec3(std::numeric_limits<float>::lowest());
    auto extentOf = [](const PartBounds& p) {
        const glm::vec3 d = p.Hi - p.Lo;
        return glm::max(d.x, glm::max(d.y, d.z));
    };
    float limit = std::numeric_limits<float>::max();
    if (parts.size() >= 3) {
        // Медиана габаритов, взвешенная числом вершин: тело с основной массой
        // вершин задаёт норму, а пара мелких деталей её не сдвигает.
        std::vector<const PartBounds*> byExtent;
        size_t total = 0;
        for (const PartBounds& p : parts) { byExtent.push_back(&p); total += p.Vertices; }
        std::sort(byExtent.begin(), byExtent.end(),
                  [&](const PartBounds* a, const PartBounds* b) { return extentOf(*a) < extentOf(*b); });
        size_t acc = 0;
        float median = extentOf(*byExtent.back());
        for (const PartBounds* p : byExtent) {
            acc += p->Vertices;
            if (acc * 2 >= total) { median = extentOf(*p); break; }
        }
        if (median > 1e-6f) limit = median * 20.0f;
    }
    for (const PartBounds& p : parts) {
        if (p.Vertices == 0 || extentOf(p) > limit) continue;
        lo = glm::min(lo, p.Lo);
        hi = glm::max(hi, p.Hi);
    }
}

ImportSettings ResolveImportSettings(const std::string& path, const glm::vec3& lo,
                                     const glm::vec3& hi) {
    ImportSettings s = LoadImportSettings(path);
    if (!s.AutoUnits || hi.x < lo.x) return s;
    const glm::vec3 size = hi - lo;
    const float extent = glm::max(size.x, glm::max(size.y, size.z));
    if (extent <= kCentimetreGuess) return s;   // проба скелета — только когда есть о чём
    const float unit = AutoUnitScale(s, ModelHasSkeleton(path), extent);
    if (unit != 1.0f) {
        s.Scale *= unit;
        LOG_INFO("Model") << "Модель " << path << " высотой " << extent
                          << " — похоже, в сантиметрах: уменьшена в 100 раз "
                          << "(настройки импорта -> «Сантиметры в метры»)";
    }
    return s;
}

glm::mat4 ImportMatrix(const ImportSettings& s, const glm::vec3& lo, const glm::vec3& hi) {
    const bool bounds = hi.x >= lo.x;
    const glm::vec3 center = bounds ? (lo + hi) * 0.5f : glm::vec3(0.0f);
    float norm = 1.0f;
    if (s.NormalizeSize && bounds) {
        const glm::vec3 size = hi - lo;
        const float maxDim = glm::max(size.x, glm::max(size.y, size.z));
        if (maxDim > 1e-6f) norm = 1.0f / maxDim;
    }
    const float scale = norm * (s.Scale > 0.0f ? s.Scale : 1.0f);
    glm::mat4 m(1.0f);
    m = glm::translate(m, s.Offset);
    if (s.Rotation != glm::vec3(0.0f))
        m *= glm::eulerAngleZYX(glm::radians(s.Rotation.z), glm::radians(s.Rotation.y),
                                glm::radians(s.Rotation.x));
    m = glm::scale(m, glm::vec3(scale));
    if (s.Recenter) m = glm::translate(m, -center);
    return m;
}

// Настройки для уже прочитанной геометрии: границы — по её вершинам.
ImportSettings ResolveImportSettings(const std::string& path, const MeshData& mesh) {
    glm::vec3 lo(std::numeric_limits<float>::max()), hi(std::numeric_limits<float>::lowest());
    for (const Vertex& v : mesh.Vertices) {
        lo = glm::min(lo, v.Position);
        hi = glm::max(hi, v.Position);
    }
    return ResolveImportSettings(path, lo, hi);
}

bool ImportSettings::ChangesGeometry() const {
    return Scale != 1.0f || Recenter || NormalizeSize || Rotation != glm::vec3(0.0f) ||
           Offset != glm::vec3(0.0f) || Normals != NormalMode::Import || FlipUV || FlipWinding;
}

namespace {

// Нормали заново. Гладкие — усреднение по ПОЛОЖЕНИЮ, а не по номеру вершины:
// у OBJ и FBX каждый угол треугольника — своя вершина, и усреднение по номеру
// дало бы те же плоские грани. Плоские — каждому треугольнику свои вершины
// (общие вершины glTF не могут нести две нормали сразу).
void RecomputeNormals(MeshData& mesh, bool smooth) {
    std::vector<Vertex>& v = mesh.Vertices;
    std::vector<unsigned int>& idx = mesh.Indices;
    if (!smooth) {
        std::vector<Vertex> flat;
        flat.reserve(idx.size());
        for (size_t i = 0; i + 2 < idx.size(); i += 3) {
            if (idx[i] >= v.size() || idx[i + 1] >= v.size() || idx[i + 2] >= v.size()) continue;
            Vertex a = v[idx[i]], b = v[idx[i + 1]], c = v[idx[i + 2]];
            glm::vec3 n = glm::cross(b.Position - a.Position, c.Position - a.Position);
            const float l = glm::length(n);
            n = l > 1e-12f ? n / l : glm::vec3(0, 1, 0);
            a.Normal = b.Normal = c.Normal = n;
            flat.push_back(a); flat.push_back(b); flat.push_back(c);
        }
        // Разметка по материалам — в индексах, а индексы стали 0..N подряд в
        // том же порядке: отрезки подмешей остаются теми же.
        v.swap(flat);
        idx.resize(v.size());
        for (size_t i = 0; i < idx.size(); ++i) idx[i] = (unsigned int)i;
        return;
    }
    // Ключ положения — с округлением: швы развёртки дают вершины в одной точке
    // с разницей в последнем знаке, и без округления шов остался бы резким.
    auto key = [](const glm::vec3& p) {
        const long long x = std::llround(p.x * 1e4), y = std::llround(p.y * 1e4),
                        z = std::llround(p.z * 1e4);
        return std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(z);
    };
    std::unordered_map<std::string, glm::vec3> acc;
    acc.reserve(v.size());
    for (size_t i = 0; i + 2 < idx.size(); i += 3) {
        if (idx[i] >= v.size() || idx[i + 1] >= v.size() || idx[i + 2] >= v.size()) continue;
        const glm::vec3& a = v[idx[i]].Position;
        const glm::vec3 n = glm::cross(v[idx[i + 1]].Position - a, v[idx[i + 2]].Position - a);
        for (int c = 0; c < 3; ++c) acc[key(v[idx[i + (size_t)c]].Position)] += n;   // вес — площадь
    }
    for (Vertex& vert : v) {
        auto it = acc.find(key(vert.Position));
        if (it == acc.end()) continue;
        const float l = glm::length(it->second);
        if (l > 1e-12f) vert.Normal = it->second / l;
    }
}

} // namespace

void ApplyImportSettings(MeshData& mesh, const ImportSettings& s) {
    if (mesh.Vertices.empty()) return;
    if (!s.ChangesGeometry()) return;
    if (s.FlipWinding) {
        for (size_t i = 0; i + 2 < mesh.Indices.size(); i += 3)
            std::swap(mesh.Indices[i + 1], mesh.Indices[i + 2]);
        // Вывернутая модель — это и нормали внутрь: иначе освещена будет
        // изнанка того, что теперь стало лицом.
        for (Vertex& v : mesh.Vertices) v.Normal = -v.Normal;
    }
    if (s.FlipUV)
        for (Vertex& v : mesh.Vertices) v.TexCoords.y = 1.0f - v.TexCoords.y;
    if (s.Normals != ImportSettings::NormalMode::Import)
        RecomputeNormals(mesh, s.Normals == ImportSettings::NormalMode::Smooth);

    // Центрирование, нормировка и масштаб — прежней функцией (её числа
    // сторожат тесты), затем поворот и сдвиг.
    ApplyImportSettings(mesh.Vertices, s);
    if (s.Rotation != glm::vec3(0.0f)) {
        const glm::mat3 r = glm::mat3(glm::eulerAngleZYX(glm::radians(s.Rotation.z),
                                                         glm::radians(s.Rotation.y),
                                                         glm::radians(s.Rotation.x)));
        for (Vertex& v : mesh.Vertices) {
            v.Position = r * v.Position;
            v.Normal = r * v.Normal;
            v.Tangent = glm::vec4(r * glm::vec3(v.Tangent), v.Tangent.w);
        }
    }
    if (s.Offset != glm::vec3(0.0f))
        for (Vertex& v : mesh.Vertices) v.Position += s.Offset;

    // Касательные зависят от нормалей и развёртки: изменились они — пересчёт.
    if (s.FlipUV || s.FlipWinding || s.Normals != ImportSettings::NormalMode::Import)
        GenerateTangents(mesh);
}

} // namespace sage::assets
