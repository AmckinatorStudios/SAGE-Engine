#include "sage/core/Paths.h"
#include "ModelLoader.h"

#include "sage/assets/import/Importer.h"
#include "sage/render/MeshData.h"
#define TINYOBJLOADER_IMPLEMENTATION_ALREADY_IN_LIB
#include <tiny_obj_loader.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include <glm/glm.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/euler_angles.hpp>

#include "sage/assets/import/MeshNormalize.h"
#include "sage/assets/import/ModelProbe.h"
#include "sage/assets/import/ObjMtl.h"
#include "sage/assets/import/PolygonTriangulate.h"
#include <nlohmann/json.hpp>

#include "sage/core/Log.h"

namespace ModelLoader {

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
    std::ifstream f(ImportSidecarPath(modelPath));
    if (!f) return s;
    try {
        nlohmann::json j;
        f >> j;
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

ImportSettings ResolveImportSettings(const std::string& path, const glm::vec3& lo,
                                     const glm::vec3& hi) {
    ImportSettings s = LoadImportSettings(path);
    if (!s.AutoUnits || hi.x < lo.x) return s;
    const glm::vec3 size = hi - lo;
    const float extent = glm::max(size.x, glm::max(size.y, size.z));
    if (extent <= kCentimetreGuess) return s;   // проба скелета — только когда есть о чём
    const float unit = AutoUnitScale(s, sage::assets::ModelHasSkeleton(path), extent);
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

namespace {
// Настройки для уже прочитанной геометрии: границы — по её вершинам.
ImportSettings SettingsFor(const std::string& path, const sage::render::MeshData& mesh) {
    glm::vec3 lo(std::numeric_limits<float>::max()), hi(std::numeric_limits<float>::lowest());
    for (const Vertex& v : mesh.Vertices) {
        lo = glm::min(lo, v.Position);
        hi = glm::max(hi, v.Position);
    }
    return ResolveImportSettings(path, lo, hi);
}
} // namespace

bool ImportSettings::ChangesGeometry() const {
    return Scale != 1.0f || Recenter || NormalizeSize || Rotation != glm::vec3(0.0f) ||
           Offset != glm::vec3(0.0f) || Normals != NormalMode::Import || FlipUV || FlipWinding;
}

namespace {

// Нормали заново. Гладкие — усреднение по ПОЛОЖЕНИЮ, а не по номеру вершины:
// у OBJ и FBX каждый угол треугольника — своя вершина, и усреднение по номеру
// дало бы те же плоские грани. Плоские — каждому треугольнику свои вершины
// (общие вершины glTF не могут нести две нормали сразу).
void RecomputeNormals(sage::render::MeshData& mesh, bool smooth) {
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

void ApplyImportSettings(sage::render::MeshData& mesh, const ImportSettings& s) {
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
        sage::assets::GenerateTangents(mesh);
}

std::shared_ptr<Mesh> LoadObj(const std::string& path) {
    sage::render::MeshData d = LoadObjData(path);
    return std::make_shared<Mesh>(d.Vertices, d.Indices, d.Submeshes);
}

sage::render::MeshData LoadObjData(const std::string& path,
                                   std::vector<sage::assets::ImportedMaterial>* materialsOut) {
    tinyobj::ObjReaderConfig config;
    // Грани режем САМИ (TriangulatePolygon), а не силами tinyobj: он делит
    // четырёхугольник по КОРОТКОЙ диагонали, а многоугольник — веером. У
    // вогнутой грани (изгиб ветки, скрученная кора) обе стратегии проводят
    // диагональ СНАРУЖИ грани: один треугольник выворачивается изнанкой и
    // отсекается, другой накрывает чужое место. На модели это «пила» из дыр
    // и тёмных зубцов вдоль изгиба — у .obj, и никогда у той же модели в glTF,
    // которую экспортёр триангулировал сам.
    config.triangulate = false;
    tinyobj::ObjReader reader;

    if (!reader.ParseFromFile(path, config)) {
        std::string err = reader.Error().empty() ? "неизвестная ошибка" : reader.Error();
        throw std::runtime_error("Не удалось загрузить модель " + path + ": " + err);
    }

    const auto& attrib = reader.GetAttrib();
    const auto& shapes = reader.GetShapes();
    const auto& materials = reader.GetMaterials();

    if (materialsOut) {
        materialsOut->clear();
        materialsOut->reserve(materials.size());
        const std::unordered_set<std::string> noKd = sage::assets::MtlTexturedWithoutKd(path);
        for (const tinyobj::material_t& m : materials) {
            sage::assets::ImportedMaterial im;
            im.Name = m.name;
            im.Albedo = glm::vec3(m.diffuse[0], m.diffuse[1], m.diffuse[2]);
            // Kd не записан, цвет — из карты: множитель единица, а не 0.6,
            // подставленные tinyobj (см. ObjMtl.h).
            if (noKd.count(m.name)) im.Albedo = glm::vec3(1.0f);
            im.Emissive = glm::vec3(m.emission[0], m.emission[1], m.emission[2]);
            im.Metallic = m.metallic;
            // Roughness в .mtl есть далеко не всегда, и ноль по умолчанию
            // означал бы зеркало на каждой модели без PBR-полей.
            // Нет Pr — из блеска Ns по формуле Blender (см. ModelMaterial.cpp).
            im.Roughness = m.roughness > 0.0f
                               ? m.roughness
                               : std::clamp(1.0f - std::sqrt(std::max(m.shininess, 0.0f)) / 30.0f,
                                            0.04f, 1.0f);
            im.Opacity = m.dissolve;
            im.AlbedoTexture = m.diffuse_texname;
            im.NormalTexture = !m.normal_texname.empty() ? m.normal_texname : m.bump_texname;
            im.MetallicTexture = m.metallic_texname;
            im.RoughnessTexture = m.roughness_texname;
            im.AOTexture = m.ambient_texname;
            im.EmissiveTexture = m.emissive_texname;
            // map_d — карта выреза (листва): то же правило, что и в
            // ModelMaterial.cpp, иначе .obj через реестр и через извлечение
            // материалов давал бы разную листву.
            if (!m.alpha_texname.empty() && m.dissolve >= 0.999f) {
                im.AlphaMode = 1;
                im.DoubleSided = true;
            } else if (m.dissolve < 0.999f) {
                im.AlphaMode = 2;
            }
            materialsOut->push_back(std::move(im));
        }
    }

    // Границы массивов атрибутов — индексы из битого/вредоносного .obj обязаны
    // проверяться, иначе чтение за границей буфера (crash/UB на крафтовом файле).
    const size_t vertexCount = attrib.vertices.size() / 3;
    const size_t normalCount = attrib.normals.size() / 3;
    const size_t texCount = attrib.texcoords.size() / 2;

    // Грани раскладываются по СВОИМ материалам. Раньше всё складывалось в один
    // буфер, и модель с отдельными материалами на корпус, стекло и колёса
    // приезжала одноцветной болванкой: разметки, по которой их можно было бы
    // покрасить порознь, просто не существовало.
    //
    // Ключ — material_id из .obj; порядок групп — порядок появления в файле.
    struct Bucket {
        int Material = -1;
        std::vector<Vertex> Vertices;
    };
    std::vector<Bucket> buckets;
    auto bucketFor = [&buckets](int material) -> Bucket& {
        for (Bucket& b : buckets)
            if (b.Material == material) return b;
        buckets.push_back(Bucket{material, {}});
        return buckets.back();
    };

    for (const auto& shape : shapes) {
        size_t corner = 0;
        for (size_t f = 0; f < shape.mesh.num_face_vertices.size(); ++f) {
            const size_t faceVerts = (size_t)shape.mesh.num_face_vertices[f];
            const int material =
                f < shape.mesh.material_ids.size() ? shape.mesh.material_ids[f] : -1;
            Bucket& bucket =
                bucketFor(material >= 0 && (size_t)material < materials.size() ? material : -1);

            // Грань целиком или никак: битый индекс в одном углу сдвинул бы
            // тройки всех следующих треугольников.
            bool valid = faceVerts >= 3 && corner + faceVerts <= shape.mesh.indices.size();
            std::vector<glm::vec3> polygon;
            for (size_t c = 0; valid && c < faceVerts; ++c) {
                const int vi = shape.mesh.indices[corner + c].vertex_index;
                if (vi < 0 || (size_t)vi >= vertexCount) { valid = false; break; }
                polygon.emplace_back(attrib.vertices[3 * vi + 0], attrib.vertices[3 * vi + 1],
                                     attrib.vertices[3 * vi + 2]);
            }
            if (!valid) { corner += faceVerts; continue; }
            const std::vector<uint32_t> triangles = sage::assets::TriangulatePolygon(polygon);

            for (uint32_t c : triangles) {
                const tinyobj::index_t& idx = shape.mesh.indices[corner + c];
                Vertex v{};
                v.Position = {
                    attrib.vertices[3 * idx.vertex_index + 0],
                    attrib.vertices[3 * idx.vertex_index + 1],
                    attrib.vertices[3 * idx.vertex_index + 2]
                };
                if (idx.normal_index >= 0 && (size_t)idx.normal_index < normalCount) {
                    v.Normal = {
                        attrib.normals[3 * idx.normal_index + 0],
                        attrib.normals[3 * idx.normal_index + 1],
                        attrib.normals[3 * idx.normal_index + 2]
                    };
                }
                if (idx.texcoord_index >= 0 && (size_t)idx.texcoord_index < texCount) {
                    v.TexCoords = {
                        attrib.texcoords[2 * idx.texcoord_index + 0],
                        attrib.texcoords[2 * idx.texcoord_index + 1]
                    };
                }
                bucket.Vertices.push_back(v);
            }
            corner += faceVerts;
        }
    }

    sage::render::MeshData out;
    for (const Bucket& b : buckets) {
        if (b.Vertices.empty()) continue;
        sage::render::Submesh sub;
        sub.Material = b.Material;
        sub.Name = (b.Material >= 0 && (size_t)b.Material < materials.size())
                       ? materials[(size_t)b.Material].name : std::string();
        sub.FirstIndex = (unsigned int)out.Indices.size();
        sub.IndexCount = (unsigned int)b.Vertices.size();
        for (const Vertex& v : b.Vertices) {
            out.Indices.push_back((unsigned int)out.Vertices.size());
            out.Vertices.push_back(v);
        }
        out.Submeshes.push_back(std::move(sub));
    }
    // Одна группа без материала — это «геометрия без .mtl», то есть состояние по
    // умолчанию, а не разметка (см. ImportedScene::Flatten).
    if (out.Submeshes.size() == 1 && out.Submeshes[0].Material < 0) out.Submeshes.clear();

    // Применяем настройки импорта из сайдкара (масштаб/центрирование/нормализация)
    // ДО создания GPU-меша — модель приходит в сцену уже приведённой.
    ApplyImportSettings(out, SettingsFor(path, out));

    return out;
}

// --- glTF / GLB ---------------------------------------------------------------
//
// tinygltf РАЗВЁРНУТ в Model.cpp (там стоит TINYGLTF_IMPLEMENTATION), поэтому
// здесь только объявления: второе разворачивание дало бы дублирующиеся символы
// на линковке.

namespace {

std::string ExtensionLower(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return "";
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return ext;
}

} // namespace

bool IsSupportedModel(const std::string& path) {
    // Спрашиваем реестр, а не свой список: панель ассетов и диалоги должны
    // предлагать ровно те форматы, которые движок в самом деле откроет — включая
    // те, что зарегистрировала игра или плагин.
    return sage::assets::ImporterRegistry::Instance().CanImport(ExtensionLower(path));
}

sage::render::MeshData LoadGltfData(const std::string& path, bool) {
    // Разбор — у импортёра (assets/import/GltfImporter.cpp): он читает файл
    // УЗЛАМИ, с именами, трансформами и материалами. Здесь узлы склеиваются в
    // один меш, потому что сущность сцены держит один Mesh, — но границы частей
    // и их материалы Flatten() сохраняет, и модель остаётся многоматериальной.
    //
    // Флаг binary больше не нужен (формат определяется по расширению внутри
    // импортёра), но параметр оставлен: на LoadGltfData ссылается существующий
    // код, и менять его сигнатуру ради одного неиспользуемого аргумента значило
    // бы трогать всех вызывающих без всякой для них пользы.
    sage::assets::ImportedScene scene;
    std::string err;
    if (!sage::assets::ImportGltf(path, scene, err)) throw std::runtime_error(err);

    sage::render::MeshData d = scene.Flatten();
    if (d.Empty()) throw std::runtime_error("В файле нет геометрии: " + path);
    // Настройки импорта применяются ко ВСЕМ форматам одинаково: масштаб и
    // центрирование — свойство ассета, а не формата, и разное поведение у .obj
    // и .glb означало бы, что одна и та же модель ведёт себя по-разному в
    // зависимости от того, как её экспортировали.
    ApplyImportSettings(d, SettingsFor(path, d));
    return d;
}

sage::render::MeshData LoadMeshData(const std::string& path) {
    // Отсутствующий файл отличаем от битого ЗДЕСЬ: ниже оба выглядят как
    // «парсер не смог», а человеку, у которого «модель не грузится», нужно
    // знать, опечатался он в пути или у него испорченный экспорт.
    {
        std::ifstream probe(path, std::ios::binary);
        if (!probe) {
            throw std::runtime_error("Файл модели не найден: " + path);
        }
    }

    const std::string ext = ExtensionLower(path);
    if (ext == "obj") return LoadObjData(path);
    if (ext == "gltf") return LoadGltfData(path, false);
    if (ext == "glb") return LoadGltfData(path, true);

    // Всё остальное — через реестр импортёров (sage/assets/import/Importer.h).
    //
    // Три формата выше разбираются здесь потому, что реестр САМ зовёт эту
    // функцию для них: рекурсия оборвалась бы не сразу, а на переполнении
    // стека. Для любого другого расширения — .bbmodel, .blend, .sagemesh и
    // всего, что зарегистрирует игра или плагин — работает общий путь.
    //
    // Это и делает реестр не украшением, а рабочим механизмом: новый формат,
    // зарегистрированный кем угодно, сразу становится загружаемым везде, где
    // движок грузит модели, — и в сцене, и в редакторе, и в собранной игре.
    {
        sage::assets::ImportedScene scene;
        std::string err;
        if (sage::assets::ImporterRegistry::Instance().Import(path, scene, err)) {
            sage::render::MeshData data = scene.Flatten();
            // Настройки импорта — и здесь: FBX, .blend и остальные форматы
            // реестра их раньше не получали вовсе, и масштаб, выставленный в
            // инспекторе FBX-модели, ни на что не влиял.
            ApplyImportSettings(data, SettingsFor(path, data));
            if (!data.Empty()) return data;
            throw std::runtime_error("В файле нет геометрии: " + path);
        }
        throw std::runtime_error(err);
    }
}

std::shared_ptr<Mesh> LoadMesh(const std::string& path, bool keepCpuData) {
    sage::render::MeshData d = LoadMeshData(path);
    // Разметка по материалам едет на видеокарту вместе с геометрией: без неё
    // многоматериальная модель рисовалась бы одним материалом — тем самым, из-за
    // которого «текстуры не работают».
    return std::make_shared<Mesh>(d.Vertices, d.Indices, d.Submeshes, keepCpuData);
}

}
