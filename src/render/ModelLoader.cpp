#include "ModelLoader.h"
#define TINYOBJLOADER_IMPLEMENTATION_ALREADY_IN_LIB
#include <tiny_obj_loader.h>
#include <stdexcept>
#include <unordered_map>

namespace ModelLoader {

MeshData LoadObjData(const std::string& path) {
    tinyobj::ObjReaderConfig config;
    tinyobj::ObjReader reader;

    if (!reader.ParseFromFile(path, config)) {
        std::string err = reader.Error().empty() ? "неизвестная ошибка" : reader.Error();
        throw std::runtime_error("Не удалось загрузить модель " + path + ": " + err);
    }

    const auto& attrib = reader.GetAttrib();
    const auto& shapes = reader.GetShapes();

    MeshData data;
    for (const auto& shape : shapes) {
        for (const auto& idx : shape.mesh.indices) {
            Vertex v{};
            v.Position = {
                attrib.vertices[3 * idx.vertex_index + 0],
                attrib.vertices[3 * idx.vertex_index + 1],
                attrib.vertices[3 * idx.vertex_index + 2]
            };
            if (idx.normal_index >= 0) {
                v.Normal = {
                    attrib.normals[3 * idx.normal_index + 0],
                    attrib.normals[3 * idx.normal_index + 1],
                    attrib.normals[3 * idx.normal_index + 2]
                };
            }
            if (idx.texcoord_index >= 0) {
                v.TexCoords = {
                    attrib.texcoords[2 * idx.texcoord_index + 0],
                    attrib.texcoords[2 * idx.texcoord_index + 1]
                };
            }
            data.Vertices.push_back(v);
            data.Indices.push_back(static_cast<unsigned int>(data.Indices.size()));
        }
    }
    return data;
}

std::shared_ptr<Mesh> LoadObj(const std::string& path) {
    return std::make_shared<Mesh>(LoadObjData(path)); // CPU-парсинг + GL-загрузка
}

}
