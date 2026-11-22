#include <algorithm>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#define TINYOBJLOADER_IMPLEMENTATION_ALREADY_IN_LIB
#include <tiny_obj_loader.h>

#include "sage/assets/import/ImportSettings.h"
#include "sage/assets/import/Importer.h"
#include "sage/assets/import/ObjMtl.h"
#include "sage/assets/import/PolygonTriangulate.h"

// Разбор Wavefront OBJ (+ .mtl) в данные ресурсов. Жил в sage/render/ModelLoader
// и делал слой ресурсов зависимым от рендера (импорт OBJ звал загрузчик
// мешей, а тот — импорт). Теперь разбор здесь, а рендер только строит из
// результата Mesh.


namespace sage::assets {

namespace {
// .obj и его .mtl — через vfs (пакет собранной игры), см. ReadObjWithMtl.
bool ParseObjViaVfs(tinyobj::ObjReader& reader, const std::string& path,
                    const tinyobj::ObjReaderConfig& config) {
    std::string objText, mtlText;
    if (!ReadObjWithMtl(path, objText, mtlText)) return false;
    return reader.ParseFromString(objText, mtlText, config);
}
} // namespace

MeshData LoadObjData(const std::string& path,
                                   std::vector<ImportedMaterial>* materialsOut,
                                   std::vector<std::string>* objectOfSubmesh) {
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

    if (!ParseObjViaVfs(reader, path, config)) {
        std::string err = reader.Error().empty() ? "неизвестная ошибка" : reader.Error();
        throw std::runtime_error("Не удалось загрузить модель " + path + ": " + err);
    }

    const auto& attrib = reader.GetAttrib();
    const auto& shapes = reader.GetShapes();
    const auto& materials = reader.GetMaterials();

    if (materialsOut) {
        materialsOut->clear();
        materialsOut->reserve(materials.size());
        const std::unordered_set<std::string> noKd = MtlTexturedWithoutKd(path);
        for (const tinyobj::material_t& m : materials) {
            ImportedMaterial im;
            im.Name = m.name;
            im.Albedo = glm::vec3(m.diffuse[0], m.diffuse[1], m.diffuse[2]);
            // Kd не записан, цвет — из карты: множитель единица, а не 0.6,
            // подставленные tinyobj (см. ObjMtl.h).
            if (noKd.count(m.name)) im.Albedo = glm::vec3(1.0f);
            im.Emissive = glm::vec3(m.emission[0], m.emission[1], m.emission[2]);
            im.Metallic = m.metallic;
            // Roughness в .mtl есть далеко не всегда, и ноль по умолчанию
            // означал бы зеркало на каждой модели без PBR-полей.
            // Нет Pr — из блеска Ns по общепринятой формуле (см. ModelMaterial.cpp).
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
        int Shape = -1;   // объект файла — только когда раскладка по объектам
        std::vector<Vertex> Vertices;
    };
    std::vector<Bucket> buckets;
    int currentShape = -1;
    auto bucketFor = [&buckets, &currentShape, objectOfSubmesh](int material) -> Bucket& {
        const int shape = objectOfSubmesh ? currentShape : -1;
        for (Bucket& b : buckets)
            if (b.Material == material && b.Shape == shape) return b;
        buckets.push_back(Bucket{material, shape, {}});
        return buckets.back();
    };

    for (const auto& shape : shapes) {
        ++currentShape;
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
            const std::vector<uint32_t> triangles = TriangulatePolygon(polygon);

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

    MeshData out;
    for (const Bucket& b : buckets) {
        if (b.Vertices.empty()) continue;
        Submesh sub;
        sub.Material = b.Material;
        sub.Name = (b.Material >= 0 && (size_t)b.Material < materials.size())
                       ? materials[(size_t)b.Material].name : std::string();
        if (objectOfSubmesh)
            objectOfSubmesh->push_back(b.Shape >= 0 && (size_t)b.Shape < shapes.size()
                                           ? shapes[(size_t)b.Shape].name : std::string());
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
    if (out.Submeshes.size() == 1 && out.Submeshes[0].Material < 0 && !objectOfSubmesh) out.Submeshes.clear();

    // Применяем настройки импорта из сайдкара (масштаб/центрирование/нормализация)
    // ДО создания GPU-меша — модель приходит в сцену уже приведённой.
    ApplyImportSettings(out, ResolveImportSettings(path, out));

    return out;
}

bool ImportObj(const std::string& path, ImportedScene& out, std::string& err) {
    try {
        // Геометрию .obj читает LoadObjData — она же применяет .sageimport. Она
        // приходит УЖЕ с разметкой по материалам (LoadObjData группирует грани
        // по material_id), поэтому здесь остаётся разложить её обратно по узлам:
        // по узлу на материал, чтобы Flatten() собрал ту же разметку, что и у
        // остальных форматов.
        std::vector<std::string> objects;
        MeshData data = LoadObjData(path, &out.Materials, &objects);
        if (data.Empty()) {
            err = "в файле нет геометрии: " + path;
            return false;
        }

        const std::string stem = std::filesystem::path(path).stem().string();
        // Объекты файла — узлы дерева (плоского: у .obj нет родителей, и
        // вершины уже в общих координатах — мировая матрица узла единичная).
        std::unordered_map<std::string, int> groupOf;
        const std::vector<Submesh> subs = data.SubmeshesOrWhole();
        for (size_t si = 0; si < subs.size(); ++si) {
            const Submesh& sub = subs[si];
            const std::string object = si < objects.size() ? objects[si] : std::string();
            ImportedNode node;
            node.Name = !object.empty() ? object : (sub.Name.empty() ? stem : sub.Name);
            node.MaterialIndex = sub.Material;
            if (!object.empty()) {
                auto [it, fresh] = groupOf.emplace(object, (int)out.Groups.size());
                if (fresh) out.Groups.push_back(ImportedGroup{object, -1, glm::mat4(1.0f)});
                node.Group = it->second;
            }
            // Вершины части — только те, на которые она ссылается: узел обязан
            // быть самодостаточным, иначе «узел» здесь означал бы не объект, а
            // окно в чужой буфер.
            std::unordered_map<unsigned int, unsigned int> remap;
            for (unsigned int k = 0; k < sub.IndexCount; ++k) {
                const unsigned int src = data.Indices[sub.FirstIndex + k];
                auto [it, inserted] = remap.emplace(src, (unsigned int)node.Mesh.Vertices.size());
                if (inserted) node.Mesh.Vertices.push_back(data.Vertices[src]);
                node.Mesh.Indices.push_back(it->second);
            }
            out.Nodes.push_back(std::move(node));
        }
        return true;
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
}

} // namespace sage::assets
