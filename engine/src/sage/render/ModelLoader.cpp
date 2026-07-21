#include "ModelLoader.h"
#include "sage/render/MeshData.h"
#define TINYOBJLOADER_IMPLEMENTATION_ALREADY_IN_LIB
#include <tiny_obj_loader.h>
#include <ufbx.h>
#include <algorithm>
#include <cctype>
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
    return std::make_shared<Mesh>(d.Vertices, d.Indices);
}

sage::render::MeshData LoadObjData(const std::string& path) {
    tinyobj::ObjReaderConfig config;
    tinyobj::ObjReader reader;

    if (!reader.ParseFromFile(path, config)) {
        std::string err = reader.Error().empty() ? "неизвестная ошибка" : reader.Error();
        throw std::runtime_error("Не удалось загрузить модель " + path + ": " + err);
    }

    const auto& attrib = reader.GetAttrib();
    const auto& shapes = reader.GetShapes();

    std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;

    // Границы массивов атрибутов — индексы из битого/вредоносного .obj обязаны
    // проверяться, иначе чтение за границей буфера (crash/UB на крафтовом файле).
    const size_t vertexCount = attrib.vertices.size() / 3;
    const size_t normalCount = attrib.normals.size() / 3;
    const size_t texCount = attrib.texcoords.size() / 2;

    for (const auto& shape : shapes) {
        for (const auto& idx : shape.mesh.indices) {
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
            vertices.push_back(v);
            indices.push_back(static_cast<unsigned int>(indices.size()));
        }
    }

    // Применяем настройки импорта из сайдкара (масштаб/центрирование/нормализация)
    // ДО создания GPU-меша — модель приходит в сцену уже приведённой.
    ApplyImportSettings(vertices, LoadImportSettings(path));

    return sage::render::MeshData{std::move(vertices), std::move(indices)};
}

void ParseFbxGeometry(const std::string& path,
                      std::vector<Vertex>& vertices,
                      std::vector<unsigned int>& indices) {
    ufbx_load_opts opts = {};
    opts.target_axes = ufbx_axes_right_handed_y_up;
    opts.target_unit_meters = 1.0f;
    opts.generate_missing_normals = true;

    ufbx_error error;
    ufbx_scene* scene = ufbx_load_file(path.c_str(), &opts, &error);
    if (!scene) {
        char buf[512];
        ufbx_format_error(buf, sizeof(buf), &error);
        throw std::runtime_error("Не удалось загрузить FBX " + path + ": " + buf);
    }
    struct SceneGuard { ufbx_scene* s; ~SceneGuard() { if (s) ufbx_free_scene(s); } } guard{scene};

    vertices.clear();
    indices.clear();
    std::vector<uint32_t> tri;

    for (size_t ni = 0; ni < scene->nodes.count; ++ni) {
        const ufbx_node* node = scene->nodes.data[ni];
        if (!node->mesh) continue;
        const ufbx_mesh* mesh = node->mesh;
        const ufbx_matrix& g2w = node->geometry_to_world;
        tri.resize(mesh->max_face_triangles * 3);
        for (size_t fi = 0; fi < mesh->faces.count; ++fi) {
            ufbx_face face = mesh->faces.data[fi];
            size_t numTris = ufbx_triangulate_face(tri.data(), tri.size(), mesh, face);
            for (size_t t = 0; t < numTris * 3; ++t) {
                uint32_t corner = tri[t];
                ufbx_vec3 p = ufbx_get_vertex_vec3(&mesh->vertex_position, corner);
                ufbx_vec3 pw = ufbx_transform_position(&g2w, p);
                Vertex v{};
                v.Position = {(float)pw.x, (float)pw.y, (float)pw.z};
                if (mesh->vertex_normal.exists) {
                    ufbx_vec3 n = ufbx_get_vertex_vec3(&mesh->vertex_normal, corner);
                    ufbx_vec3 nw = ufbx_transform_direction(&g2w, n);
                    glm::vec3 gn((float)nw.x, (float)nw.y, (float)nw.z);
                    float len = glm::length(gn);
                    v.Normal = len > 1e-6f ? gn / len : glm::vec3(0, 1, 0);
                }
                if (mesh->vertex_uv.exists) {
                    ufbx_vec2 uv = ufbx_get_vertex_vec2(&mesh->vertex_uv, corner);
                    v.TexCoords = {(float)uv.x, (float)uv.y};
                }
                indices.push_back((unsigned int)vertices.size());
                vertices.push_back(v);
            }
        }
    }
    if (vertices.empty())
        throw std::runtime_error("FBX не содержит геометрии: " + path);
}

std::shared_ptr<Mesh> LoadFbx(const std::string& path) {
    std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;
    ParseFbxGeometry(path, vertices, indices);
    // Настройки импорта (масштаб/центрирование/нормализация) — ДО GPU-меша.
    ApplyImportSettings(vertices, LoadImportSettings(path));
    return std::make_shared<Mesh>(vertices, indices);
}

std::shared_ptr<Mesh> Load(const std::string& path) {
    std::string ext;
    size_t dot = path.find_last_of('.');
    if (dot != std::string::npos) {
        ext = path.substr(dot + 1);
        for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
    }
    if (ext == "fbx") return LoadFbx(path);
    return LoadObj(path);
}

}
