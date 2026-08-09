#include "sage/assets/import/Importer.h"

#include <algorithm>
#include <filesystem>
#include <unordered_map>

#include "sage/assets/format/MeshFormat.h"
#include "sage/core/Log.h"
#include "sage/render/ModelLoader.h"

namespace sage::assets {

namespace {

std::string LowerExt(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return ext;
}

} // namespace

size_t ImportedScene::TotalVertices() const {
    size_t n = 0;
    for (const ImportedNode& node : Nodes) n += node.Mesh.Vertices.size();
    return n;
}

size_t ImportedScene::TotalTriangles() const {
    size_t n = 0;
    for (const ImportedNode& node : Nodes) n += node.Mesh.Indices.size() / 3;
    return n;
}

sage::render::MeshData ImportedScene::Flatten() const {
    sage::render::MeshData out;

    // Порядок групп — по ПЕРВОМУ появлению материала, а не по его номеру:
    // порядок частей в файле — это порядок, в котором их видит человек в
    // Blender, и слоты материалов в инспекторе должны идти так же.
    std::vector<int> order;
    for (const ImportedNode& node : Nodes) {
        if (std::find(order.begin(), order.end(), node.MaterialIndex) == order.end())
            order.push_back(node.MaterialIndex);
    }

    for (int material : order) {
        sage::render::Submesh sub;
        sub.Material = material;
        sub.FirstIndex = (unsigned int)out.Indices.size();

        for (const ImportedNode& node : Nodes) {
            if (node.MaterialIndex != material) continue;
            // Имя подмеша — имя первого узла группы: у одноматериальной части
            // это и есть её имя из файла, а искать по номеру материала человеку
            // нечем.
            if (sub.Name.empty()) sub.Name = node.Name;

            const unsigned int base = (unsigned int)out.Vertices.size();
            // Нормали преобразуются матрицей, ОБРАТНОЙ ТРАНСПОНИРОВАННОЙ к модельной:
            // при неравномерном масштабе обычное умножение перекашивает их, и свет
            // ложится не туда. Для равномерных матриц это то же самое, поэтому
            // считаем всегда — дешевле, чем ветка с проверкой.
            const glm::mat3 normalMat = glm::transpose(glm::inverse(glm::mat3(node.Transform)));
            for (const Vertex& v : node.Mesh.Vertices) {
                Vertex t = v;
                t.Position = glm::vec3(node.Transform * glm::vec4(v.Position, 1.0f));
                t.Normal = glm::normalize(normalMat * v.Normal);
                const glm::vec3 tang = normalMat * glm::vec3(v.Tangent);
                const float l = glm::length(tang);
                t.Tangent = glm::vec4(l > 1e-12f ? tang / l : glm::vec3(1, 0, 0), v.Tangent.w);
                out.Vertices.push_back(t);
            }
            for (unsigned int i : node.Mesh.Indices) out.Indices.push_back(base + i);
        }

        sub.IndexCount = (unsigned int)out.Indices.size() - sub.FirstIndex;
        if (sub.IndexCount > 0) out.Submeshes.push_back(std::move(sub));
    }

    // Разметка из одной части и без материала не несёт информации: это ровно
    // «вся геометрия одним куском», то есть состояние по умолчанию. Оставить её
    // значило бы показывать в инспекторе слот-пустышку у каждой .obj-модели.
    if (out.Submeshes.size() == 1 && out.Submeshes[0].Material < 0) out.Submeshes.clear();
    return out;
}

ImporterRegistry& ImporterRegistry::Instance() {
    static ImporterRegistry inst;
    return inst;
}

ImporterRegistry::ImporterRegistry() { RegisterBuiltinImporters(*this); }

void ImporterRegistry::Register(const std::string& extension, const std::string& label,
                                ImportFn fn) {
    if (extension.empty() || !fn) return;
    std::string ext = extension;
    if (ext[0] != '.') ext.insert(ext.begin(), '.');
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    for (ImporterInfo& info : m_importers) {
        if (info.Extension == ext) {
            info.Label = label;
            info.Import = std::move(fn);
            return;
        }
    }
    m_importers.push_back({ext, label, std::move(fn)});
}

bool ImporterRegistry::CanImport(const std::string& extension) const {
    std::string ext = extension;
    if (!ext.empty() && ext[0] != '.') ext.insert(ext.begin(), '.');
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    for (const ImporterInfo& info : m_importers)
        if (info.Extension == ext) return true;
    return false;
}

bool ImporterRegistry::Import(const std::string& path, ImportedScene& out,
                              std::string& err) const {
    const std::string ext = LowerExt(path);
    if (ext.empty()) {
        err = "у файла нет расширения — по нему определяется формат: " + path;
        return false;
    }
    for (const ImporterInfo& info : m_importers) {
        if (info.Extension != ext) continue;
        if (!std::filesystem::exists(path)) {
            err = "файл не найден: " + path;
            return false;
        }
        if (!info.Import(path, out, err)) return false;
        for (const std::string& w : out.Warnings)
            LOG_WARN("Import") << std::filesystem::path(path).filename().string() << ": " << w;
        return true;
    }
    // Перечисляем, что УМЕЕМ: человеку с файлом .fbx нужно знать не только что
    // его не приняли, но и во что пересохранить.
    std::string known;
    for (const ImporterInfo& info : m_importers) {
        if (!known.empty()) known += ", ";
        known += info.Extension;
    }
    err = "формат " + ext + " не поддерживается. Известные: " + known;
    return false;
}

std::vector<std::string> ImporterRegistry::Extensions() const {
    std::vector<std::string> out;
    out.reserve(m_importers.size());
    for (const ImporterInfo& info : m_importers) out.push_back(info.Extension);
    return out;
}

// --- Встроенные ---------------------------------------------------------------

bool ImportObj(const std::string& path, ImportedScene& out, std::string& err) {
    try {
        // Геометрию .obj читает ModelLoader — он же применяет .sageimport. Она
        // приходит УЖЕ с разметкой по материалам (LoadObjData группирует грани
        // по material_id), поэтому здесь остаётся разложить её обратно по узлам:
        // по узлу на материал, чтобы Flatten() собрал ту же разметку, что и у
        // остальных форматов.
        sage::render::MeshData data = ModelLoader::LoadObjData(path, &out.Materials);
        if (data.Empty()) {
            err = "в файле нет геометрии: " + path;
            return false;
        }

        const std::string stem = std::filesystem::path(path).stem().string();
        for (const sage::render::Submesh& sub : data.SubmeshesOrWhole()) {
            ImportedNode node;
            node.Name = sub.Name.empty() ? stem : sub.Name;
            node.MaterialIndex = sub.Material;
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

bool ImportSageMesh(const std::string& path, ImportedScene& out, std::string& err) {
    // Свой формат тоже проходит через реестр: он такой же вход, как остальные, и
    // отдельная дверь для него означала бы, что путь импорта проверяется не весь.
    ImportedNode node;
    node.Name = std::filesystem::path(path).stem().string();
    if (!ReadMeshFile(path, node.Mesh, err)) return false;
    out.Nodes.push_back(std::move(node));
    return true;
}

void RegisterBuiltinImporters(ImporterRegistry& registry) {
    registry.Register(".obj", "Wavefront OBJ", &ImportObj);
    registry.Register(".gltf", "glTF 2.0", &ImportGltf);
    registry.Register(".glb", "glTF 2.0 (binary)", &ImportGltf);
    registry.Register(".bbmodel", "Blockbench", &ImportBlockbench);
    registry.Register(".blend", "Blender", &ImportBlend);
    // FBX — то, во что по умолчанию экспортируют Blender, Maya, 3ds Max,
    // Mixamo и любой ассет-стор. Без него «своя модель» чаще всего не
    // открывалась вовсе (см. FbxImporter.cpp).
    registry.Register(".fbx", "Autodesk FBX", &ImportFbx);
    registry.Register(".sagemesh", "SAGE mesh", &ImportSageMesh);
}

} // namespace sage::assets
