#include "ModelLoader.h"

#include "sage/assets/import/Importer.h"
#include "sage/render/MeshData.h"
#define TINYOBJLOADER_IMPLEMENTATION_ALREADY_IN_LIB
#include <tiny_obj_loader.h>
#include <algorithm>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <unordered_map>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include "sage/core/Log.h"

namespace ModelLoader {

std::string ImportSidecarPath(const std::string& modelPath) {
    return modelPath + ".sageimport";
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
    } catch (const std::exception& e) {
        LOG_WARN("Model") << "Битый .sageimport (" << modelPath << "): " << e.what();
    }
    return s;
}

bool SaveImportSettings(const std::string& modelPath, const ImportSettings& s) {
    nlohmann::json j = {
        {"scale", s.Scale}, {"recenter", s.Recenter}, {"normalize", s.NormalizeSize},
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

std::shared_ptr<Mesh> LoadObj(const std::string& path) {
    sage::render::MeshData d = LoadObjData(path);
    return std::make_shared<Mesh>(d.Vertices, d.Indices, d.Submeshes);
}

sage::render::MeshData LoadObjData(const std::string& path,
                                   std::vector<sage::assets::ImportedMaterial>* materialsOut) {
    tinyobj::ObjReaderConfig config;
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
        for (const tinyobj::material_t& m : materials) {
            sage::assets::ImportedMaterial im;
            im.Name = m.name;
            im.Albedo = glm::vec3(m.diffuse[0], m.diffuse[1], m.diffuse[2]);
            im.Emissive = glm::vec3(m.emission[0], m.emission[1], m.emission[2]);
            im.Metallic = m.metallic;
            // Roughness в .mtl есть далеко не всегда, и ноль по умолчанию
            // означал бы зеркало на каждой модели без PBR-полей.
            im.Roughness = m.roughness > 0.0f ? m.roughness : 0.5f;
            im.Opacity = m.dissolve;
            im.AlbedoTexture = m.diffuse_texname;
            im.NormalTexture = !m.normal_texname.empty() ? m.normal_texname : m.bump_texname;
            im.MetallicTexture = m.metallic_texname;
            im.RoughnessTexture = m.roughness_texname;
            im.AOTexture = m.ambient_texname;
            im.EmissiveTexture = m.emissive_texname;
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

            for (size_t c = 0; c < faceVerts && corner + c < shape.mesh.indices.size(); ++c) {
                const tinyobj::index_t& idx = shape.mesh.indices[corner + c];
                if (idx.vertex_index < 0 || (size_t)idx.vertex_index >= vertexCount) continue;
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
    ApplyImportSettings(out.Vertices, LoadImportSettings(path));

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
    ApplyImportSettings(d.Vertices, LoadImportSettings(path));
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
