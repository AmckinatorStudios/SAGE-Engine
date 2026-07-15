#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE       // используем свой callback на базе уже подключённого stb_image
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include <tiny_gltf.h>

#include "Model.h"
#include <tiny_obj_loader.h>
#include <stb_image.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <unordered_map>
#include "../core/Log.h"
#include <stdexcept>

// ----------------------------------------------------------------------
// Общее: декодирование картинки (используется tinygltf для встроенных/
// внешних текстур) через уже подключённый в проекте stb_image
// ----------------------------------------------------------------------
static bool GltfImageLoader(tinygltf::Image* image, const int, std::string* err, std::string*,
                             int, int, const unsigned char* bytes, int size, void*) {
    int w, h, comp;
    unsigned char* data = stbi_load_from_memory(bytes, size, &w, &h, &comp, 4); // всегда просим RGBA
    if (!data) {
        if (err) *err += "Не удалось декодировать встроенное изображение glTF\n";
        return false;
    }
    image->width = w;
    image->height = h;
    image->component = 4;
    image->bits = 8;
    image->pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
    image->image.assign(data, data + (size_t)w * h * 4);
    stbi_image_free(data);
    return true;
}

static std::string GetDirectory(const std::string& path) {
    size_t pos = path.find_last_of("/\\");
    return (pos == std::string::npos) ? "" : path.substr(0, pos);
}

// ----------------------------------------------------------------------
// OBJ + MTL
// ----------------------------------------------------------------------
std::unique_ptr<Model> Model::LoadObjInternal(const std::string& path) {
    std::string dir = GetDirectory(path);

    tinyobj::ObjReaderConfig config;
    config.mtl_search_path = dir.empty() ? "." : dir;

    tinyobj::ObjReader reader;
    if (!reader.ParseFromFile(path, config)) {
        std::string err = reader.Error().empty() ? "неизвестная ошибка" : reader.Error();
        LOG_ERROR("Model") << "Не удалось загрузить OBJ " << path << ": " << err;
        throw std::runtime_error("Не удалось загрузить OBJ " + path + ": " + err);
    }
    if (!reader.Warning().empty()) {
        LOG_WARN("Model") << "Предупреждение при загрузке " << path << ": " << reader.Warning();
    }

    const auto& attrib = reader.GetAttrib();
    const auto& shapes = reader.GetShapes();
    const auto& materials = reader.GetMaterials();

    // Группируем вершины по material_id — каждая группа станет своим SubMesh,
    // потому что у разных материалов могут быть разные текстуры.
    struct Group { std::vector<Vertex> vertices; std::vector<unsigned int> indices; };
    std::unordered_map<int, Group> groups;

    for (const auto& shape : shapes) {
        size_t indexOffset = 0;
        for (size_t f = 0; f < shape.mesh.num_face_vertices.size(); ++f) {
            int fv = shape.mesh.num_face_vertices[f];
            int materialId = shape.mesh.material_ids.empty() ? -1 : shape.mesh.material_ids[f];
            Group& group = groups[materialId];

            for (int v = 0; v < fv; ++v) {
                tinyobj::index_t idx = shape.mesh.indices[indexOffset + v];
                Vertex vertex{};
                vertex.Position = {
                    attrib.vertices[3 * idx.vertex_index + 0],
                    attrib.vertices[3 * idx.vertex_index + 1],
                    attrib.vertices[3 * idx.vertex_index + 2]
                };
                if (idx.normal_index >= 0) {
                    vertex.Normal = {
                        attrib.normals[3 * idx.normal_index + 0],
                        attrib.normals[3 * idx.normal_index + 1],
                        attrib.normals[3 * idx.normal_index + 2]
                    };
                } else {
                    vertex.Normal = {0.0f, 1.0f, 0.0f}; // нет нормалей в файле — грубый запасной вариант
                }
                if (idx.texcoord_index >= 0) {
                    vertex.TexCoords = {
                        attrib.texcoords[2 * idx.texcoord_index + 0],
                        attrib.texcoords[2 * idx.texcoord_index + 1]
                    };
                }
                group.indices.push_back(static_cast<unsigned int>(group.vertices.size()));
                group.vertices.push_back(vertex);
            }
            indexOffset += fv;
        }
    }

    auto model = std::make_unique<Model>();
    std::unordered_map<std::string, std::shared_ptr<Texture>> textureCache;

    for (auto& [materialId, group] : groups) {
        if (group.vertices.empty()) continue;

        SubMesh sub;
        sub.MeshData = std::make_shared<Mesh>(group.vertices, group.indices);
        sub.TintColor = {0.8f, 0.8f, 0.8f}; // нейтральный запасной цвет, если материала нет

        if (materialId >= 0 && materialId < (int)materials.size()) {
            const auto& mat = materials[materialId];
            if (!mat.diffuse_texname.empty()) {
                std::string texPath = dir.empty() ? mat.diffuse_texname : dir + "/" + mat.diffuse_texname;
                auto it = textureCache.find(texPath);
                if (it != textureCache.end()) {
                    sub.DiffuseTexture = it->second;
                } else {
                    try {
                        sub.DiffuseTexture = std::make_shared<Texture>(texPath, TextureFilter::Trilinear, true);
                        textureCache[texPath] = sub.DiffuseTexture;
                    } catch (const std::exception& e) {
                        LOG_WARN("Model") << "Не удалось загрузить текстуру " << texPath << ": " << e.what();
                    }
                }
                sub.TintColor = {1.0f, 1.0f, 1.0f};
            } else {
                glm::vec3 diffuse(mat.diffuse[0], mat.diffuse[1], mat.diffuse[2]);
                sub.TintColor = (glm::length(diffuse) < 0.01f) ? glm::vec3(0.8f) : diffuse;
            }
        }

        model->m_subMeshes.push_back(std::move(sub));
    }

    if (model->m_subMeshes.empty()) {
        LOG_ERROR("Model") << "OBJ файл не содержит геометрии: " << path;
        throw std::runtime_error("OBJ файл не содержит геометрии: " + path);
    }

    LOG_INFO("Model") << "Загружен OBJ " << path << " (" << model->m_subMeshes.size() << " подмешей)";
    return model;
}

// ----------------------------------------------------------------------
// GLTF / GLB
// ----------------------------------------------------------------------
namespace {

glm::mat4 NodeLocalMatrix(const tinygltf::Node& node) {
    if (node.matrix.size() == 16) {
        glm::mat4 m;
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                m[c][r] = static_cast<float>(node.matrix[c * 4 + r]);
        return m;
    }
    glm::mat4 t(1.0f), r(1.0f), s(1.0f);
    if (node.translation.size() == 3)
        t = glm::translate(glm::mat4(1.0f), glm::vec3(node.translation[0], node.translation[1], node.translation[2]));
    if (node.rotation.size() == 4) {
        glm::quat q(static_cast<float>(node.rotation[3]), static_cast<float>(node.rotation[0]),
                    static_cast<float>(node.rotation[1]), static_cast<float>(node.rotation[2])); // (w,x,y,z)
        r = glm::mat4_cast(q);
    }
    if (node.scale.size() == 3)
        s = glm::scale(glm::mat4(1.0f), glm::vec3(node.scale[0], node.scale[1], node.scale[2]));
    return t * r * s;
}

// Читает accessor из FLOAT-данных (позиции/нормали/UV) в плоский vector<float>
void ReadFloatAccessor(const tinygltf::Model& model, int accessorIndex, int components, std::vector<float>& out) {
    const auto& accessor = model.accessors[accessorIndex];
    const auto& view = model.bufferViews[accessor.bufferView];
    const auto& buffer = model.buffers[view.buffer];
    size_t stride = accessor.ByteStride(view);
    const unsigned char* base = buffer.data.data() + view.byteOffset + accessor.byteOffset;

    out.resize(accessor.count * components);
    for (size_t i = 0; i < accessor.count; ++i) {
        const float* v = reinterpret_cast<const float*>(base + i * stride);
        for (int c = 0; c < components; ++c) out[i * components + c] = v[c];
    }
}

void ReadIndices(const tinygltf::Model& model, int accessorIndex, std::vector<unsigned int>& out) {
    const auto& accessor = model.accessors[accessorIndex];
    const auto& view = model.bufferViews[accessor.bufferView];
    const auto& buffer = model.buffers[view.buffer];
    size_t stride = accessor.ByteStride(view);
    const unsigned char* base = buffer.data.data() + view.byteOffset + accessor.byteOffset;

    out.resize(accessor.count);
    for (size_t i = 0; i < accessor.count; ++i) {
        const unsigned char* p = base + i * stride;
        switch (accessor.componentType) {
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:  out[i] = *p; break;
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: out[i] = *reinterpret_cast<const uint16_t*>(p); break;
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:   out[i] = *reinterpret_cast<const uint32_t*>(p); break;
            default: out[i] = 0; break;
        }
    }
}

void ProcessPrimitive(const tinygltf::Model& gltfModel, const tinygltf::Primitive& prim,
                       const glm::mat4& worldMatrix,
                       std::unordered_map<int, std::shared_ptr<Texture>>& textureCache,
                       std::vector<SubMesh>& outSubMeshes) {
    if (prim.mode != TINYGLTF_MODE_TRIANGLES) return; // пропускаем линии/точки — редкость в игровых моделях
    auto posIt = prim.attributes.find("POSITION");
    if (posIt == prim.attributes.end()) return;

    std::vector<float> positions, normals, texcoords;
    ReadFloatAccessor(gltfModel, posIt->second, 3, positions);

    auto normIt = prim.attributes.find("NORMAL");
    bool hasNormals = normIt != prim.attributes.end();
    if (hasNormals) ReadFloatAccessor(gltfModel, normIt->second, 3, normals);

    auto uvIt = prim.attributes.find("TEXCOORD_0");
    bool hasUV = uvIt != prim.attributes.end();
    if (hasUV) ReadFloatAccessor(gltfModel, uvIt->second, 2, texcoords);

    size_t vertexCount = positions.size() / 3;
    glm::mat3 normalMatrix = glm::mat3(glm::transpose(glm::inverse(worldMatrix)));

    std::vector<Vertex> vertices(vertexCount);
    for (size_t i = 0; i < vertexCount; ++i) {
        glm::vec3 localPos(positions[i * 3], positions[i * 3 + 1], positions[i * 3 + 2]);
        vertices[i].Position = glm::vec3(worldMatrix * glm::vec4(localPos, 1.0f));

        glm::vec3 localNormal = hasNormals
            ? glm::vec3(normals[i * 3], normals[i * 3 + 1], normals[i * 3 + 2])
            : glm::vec3(0.0f, 1.0f, 0.0f);
        vertices[i].Normal = glm::normalize(normalMatrix * localNormal);

        vertices[i].TexCoords = hasUV ? glm::vec2(texcoords[i * 2], texcoords[i * 2 + 1]) : glm::vec2(0.0f);
    }

    std::vector<unsigned int> indices;
    if (prim.indices >= 0) {
        ReadIndices(gltfModel, prim.indices, indices);
    } else {
        indices.resize(vertexCount);
        for (size_t i = 0; i < vertexCount; ++i) indices[i] = static_cast<unsigned int>(i);
    }

    SubMesh sub;
    sub.MeshData = std::make_shared<Mesh>(vertices, indices);
    sub.TintColor = {1.0f, 1.0f, 1.0f};

    // Загружает (с кэшированием по индексу картинки) текстуру glTF по индексу
    // ЭЛЕМЕНТА texture[] — общая логика для обоих путей материала ниже.
    auto loadMaterialTexture = [&](int textureIndex) -> std::shared_ptr<Texture> {
        if (textureIndex < 0 || textureIndex >= (int)gltfModel.textures.size()) return nullptr;
        int imageIndex = gltfModel.textures[textureIndex].source;
        auto cacheIt = textureCache.find(imageIndex);
        if (cacheIt != textureCache.end()) return cacheIt->second;
        if (imageIndex < 0 || imageIndex >= (int)gltfModel.images.size()) return nullptr;
        const auto& img = gltfModel.images[imageIndex];
        if (img.image.empty()) return nullptr;
        auto tex = std::make_shared<Texture>(img.image.data(), img.width, img.height, TextureFilter::Trilinear, true);
        textureCache[imageIndex] = tex;
        return tex;
    };

    if (prim.material >= 0 && prim.material < (int)gltfModel.materials.size()) {
        const auto& mat = gltfModel.materials[prim.material];
        const auto& pbr = mat.pbrMetallicRoughness;
        if (pbr.baseColorFactor.size() == 4) {
            sub.TintColor = glm::vec3(pbr.baseColorFactor[0], pbr.baseColorFactor[1], pbr.baseColorFactor[2]);
        }
        if (pbr.baseColorTexture.index >= 0) {
            sub.DiffuseTexture = loadMaterialTexture(pbr.baseColorTexture.index);
        }

        // Некоторые модели (в т.ч. многие бесплатные ассеты) используют старое
        // расширение KHR_materials_pbrSpecularGlossiness вместо стандартного
        // metallic-roughness — там текстура/цвет лежат в diffuseTexture/
        // diffuseFactor. Без этого фолбэка такие модели рисуются вообще
        // без текстуры (просто плоским белым цветом).
        if (!sub.DiffuseTexture) {
            auto extIt = mat.extensions.find("KHR_materials_pbrSpecularGlossiness");
            if (extIt != mat.extensions.end()) {
                const tinygltf::Value& ext = extIt->second;
                if (ext.Has("diffuseFactor")) {
                    const tinygltf::Value& df = ext.Get("diffuseFactor");
                    if (df.IsArray() && df.ArrayLen() >= 3) {
                        sub.TintColor = glm::vec3(
                            static_cast<float>(df.Get(0).GetNumberAsDouble()),
                            static_cast<float>(df.Get(1).GetNumberAsDouble()),
                            static_cast<float>(df.Get(2).GetNumberAsDouble()));
                    }
                }
                if (ext.Has("diffuseTexture")) {
                    const tinygltf::Value& dt = ext.Get("diffuseTexture");
                    if (dt.Has("index")) {
                        sub.DiffuseTexture = loadMaterialTexture(dt.Get("index").GetNumberAsInt());
                    }
                }
            }
        }
    }

    outSubMeshes.push_back(std::move(sub));
}

void ProcessNode(const tinygltf::Model& gltfModel, int nodeIndex, const glm::mat4& parentMatrix,
                  std::unordered_map<int, std::shared_ptr<Texture>>& textureCache,
                  std::vector<SubMesh>& outSubMeshes) {
    const tinygltf::Node& node = gltfModel.nodes[nodeIndex];
    glm::mat4 worldMatrix = parentMatrix * NodeLocalMatrix(node);

    if (node.mesh >= 0 && node.mesh < (int)gltfModel.meshes.size()) {
        for (const auto& prim : gltfModel.meshes[node.mesh].primitives) {
            ProcessPrimitive(gltfModel, prim, worldMatrix, textureCache, outSubMeshes);
        }
    }
    for (int child : node.children) {
        ProcessNode(gltfModel, child, worldMatrix, textureCache, outSubMeshes);
    }
}

} // namespace

std::unique_ptr<Model> Model::LoadGltfInternal(const std::string& path, bool binary) {
    tinygltf::TinyGLTF loader;
    loader.SetImageLoader(&GltfImageLoader, nullptr);

    tinygltf::Model gltfModel;
    std::string err, warn;
    bool ok = binary
        ? loader.LoadBinaryFromFile(&gltfModel, &err, &warn, path)
        : loader.LoadASCIIFromFile(&gltfModel, &err, &warn, path);

    if (!warn.empty()) LOG_WARN("Model") << "Предупреждение glTF (" << path << "): " << warn;
    if (!ok) {
        LOG_ERROR("Model") << "Не удалось загрузить glTF " << path << ": " << err;
        throw std::runtime_error("Не удалось загрузить glTF " + path + ": " + err);
    }

    auto model = std::make_unique<Model>();
    std::unordered_map<int, std::shared_ptr<Texture>> textureCache;

    int sceneIndex = gltfModel.defaultScene >= 0 ? gltfModel.defaultScene : 0;
    if (!gltfModel.scenes.empty()) {
        for (int nodeIdx : gltfModel.scenes[sceneIndex].nodes) {
            ProcessNode(gltfModel, nodeIdx, glm::mat4(1.0f), textureCache, model->m_subMeshes);
        }
    } else {
        for (size_t i = 0; i < gltfModel.nodes.size(); ++i) {
            ProcessNode(gltfModel, (int)i, glm::mat4(1.0f), textureCache, model->m_subMeshes);
        }
    }

    if (model->m_subMeshes.empty()) {
        LOG_ERROR("Model") << "glTF файл не содержит поддерживаемой геометрии: " << path;
        throw std::runtime_error("glTF файл не содержит поддерживаемой геометрии: " + path);
    }

    LOG_INFO("Model") << "Загружен glTF " << path << " (" << model->m_subMeshes.size() << " подмешей)";
    return model;
}

// ----------------------------------------------------------------------
// Диспетчер по расширению + рендер
// ----------------------------------------------------------------------
static std::string GetExtensionLower(const std::string& path) {
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return "";
    std::string ext = path.substr(dot + 1);
    for (auto& c : ext) c = (char)tolower(c);
    return ext;
}

std::unique_ptr<Model> Model::Load(const std::string& path) {
    std::string ext = GetExtensionLower(path);
    if (ext == "obj") return LoadObjInternal(path);
    if (ext == "gltf") return LoadGltfInternal(path, false);
    if (ext == "glb") return LoadGltfInternal(path, true);
    LOG_ERROR("Model") << "Неподдерживаемый формат модели: " << path;
    throw std::runtime_error("Неподдерживаемый формат модели: " + path + " (поддерживаются .obj, .gltf, .glb)");
}

void Model::Draw(Shader& shader) const {
    for (const auto& sub : m_subMeshes) {
        if (sub.DiffuseTexture) {
            sub.DiffuseTexture->Bind(0);
            shader.SetInt("uTexture", 0);
            shader.SetInt("uUseTexture", 1);
        } else {
            shader.SetInt("uUseTexture", 0);
        }
        shader.SetVec3("uObjectColor", sub.TintColor);
        sub.MeshData->Draw();
    }
}
