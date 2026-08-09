// Реализация tinygltf развёрнута в render/Model.cpp — здесь только объявления
// (второе разворачивание дало бы дублирующиеся символы на линковке).
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include <tiny_gltf.h>

#include "sage/assets/import/Importer.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <stb_image.h>

// ---------------------------------------------------------------------------
// glTF/GLB -> ImportedScene: УЗЛЫ, а не один слипшийся меш.
//
// ЧТО БЫЛО. ImportGltf сводился к ImportObj: вся геометрия файла сливалась в
// один буфер без границ и без материалов. Для движка это означало, что модель,
// в которой лежат четырнадцать материалов, приезжает как одна безымянная
// болванка — не потому что материалы не прочитались, а потому что от них не
// осталось СТРУКТУРЫ, к которой их можно было бы привязать. Иерархия сцены
// внутри файла (узлы, их имена и трансформы) терялась там же.
//
// ЧТО СТАЛО. Один ImportedNode на КАЖДЫЙ примитив каждого узла: своё имя, своя
// мировая матрица, свой индекс материала. Дальше с этим работают все, кому
// нужен разбор по частям:
//   • ImportedScene::Flatten() склеивает узлы в один меш, но СОХРАНЯЕТ границы
//     (Submesh) и материал каждой части — так многоматериальная модель едет в
//     сущность сцены целиком и красится по частям;
//   • панель импорта и будущий импорт сцен получают настоящее дерево объектов,
//     а не один узел «модель».
//
// ПОЧЕМУ МАТРИЦА УЗЛА НЕ ЗАПЕКАЕТСЯ ЗДЕСЬ. Вершины остаются в локальных
// координатах примитива, а положение узла лежит в ImportedNode::Transform.
// Запечь их сразу значило бы навсегда потерять, где кончается один объект и
// начинается другой, — то есть повторить ровно ту потерю, ради которой этот
// файл и написан. Flatten() запекает их сам, когда меш и правда нужен один.
// ---------------------------------------------------------------------------

namespace sage::assets {
namespace {

namespace fs = std::filesystem;

// Раскодировать картинку tinygltf сам не умеет (собран с TINYGLTF_NO_STB_IMAGE),
// и без колбэка ЛЮБОЙ файл с текстурами разбирается с ошибкой — то есть модель
// не грузится вовсе. Геометрия при этом картинки не использует, но разбор
// файла общий.
bool DecodeImage(tinygltf::Image* image, const int, std::string* err, std::string*, int, int,
                 const unsigned char* bytes, int size, void*) {
    int w = 0, h = 0, comp = 0;
    unsigned char* data = stbi_load_from_memory(bytes, size, &w, &h, &comp, 4);
    if (!data) {
        if (err) *err += "не удалось раскодировать изображение glTF\n";
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

glm::mat4 NodeLocalMatrix(const tinygltf::Node& node) {
    if (node.matrix.size() == 16) {
        glm::mat4 m(1.0f);
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r) m[c][r] = (float)node.matrix[c * 4 + r];
        return m;
    }
    glm::mat4 t(1.0f), r(1.0f), s(1.0f);
    if (node.translation.size() == 3) {
        t = glm::translate(glm::mat4(1.0f), glm::vec3((float)node.translation[0],
                                                      (float)node.translation[1],
                                                      (float)node.translation[2]));
    }
    if (node.rotation.size() == 4) {
        // glTF хранит кватернион как (x,y,z,w), конструктор glm ждёт (w,x,y,z).
        const glm::quat q((float)node.rotation[3], (float)node.rotation[0],
                          (float)node.rotation[1], (float)node.rotation[2]);
        r = glm::mat4_cast(q);
    }
    if (node.scale.size() == 3) {
        s = glm::scale(glm::mat4(1.0f),
                       glm::vec3((float)node.scale[0], (float)node.scale[1], (float)node.scale[2]));
    }
    return t * r * s;
}

// --- Чтение аксессоров С ПРОВЕРКОЙ ГРАНИЦ -----------------------------------
//
// Проверки здесь не перестраховка. Индексы аксессоров, смещения и шаги в glTF —
// это числа из файла, и файл может быть битым или собранным нарочно. Чтение по
// ним без проверки — это чтение за границей буфера в процессе редактора,
// который люди запускают на моделях, скачанных из интернета.
struct AccessorView {
    const unsigned char* Base = nullptr;
    size_t Count = 0;
    size_t Stride = 0;
    int ComponentType = 0;
    bool Valid = false;
};

AccessorView OpenAccessor(const tinygltf::Model& gltf, int accessorIndex, size_t elementBytes) {
    AccessorView v;
    if (accessorIndex < 0 || accessorIndex >= (int)gltf.accessors.size()) return v;
    const tinygltf::Accessor& acc = gltf.accessors[(size_t)accessorIndex];
    if (acc.bufferView < 0 || acc.bufferView >= (int)gltf.bufferViews.size()) return v;
    const tinygltf::BufferView& view = gltf.bufferViews[(size_t)acc.bufferView];
    if (view.buffer < 0 || view.buffer >= (int)gltf.buffers.size()) return v;
    const std::vector<unsigned char>& data = gltf.buffers[(size_t)view.buffer].data;

    const size_t stride = acc.ByteStride(view) > 0 ? (size_t)acc.ByteStride(view) : elementBytes;
    const size_t offset = view.byteOffset + acc.byteOffset;
    if (offset > data.size() || acc.count == 0) return v;
    // Последний элемент обязан целиком помещаться в буфер.
    const size_t span = stride * (acc.count - 1) + elementBytes;
    if (span > data.size() - offset) return v;

    v.Base = data.data() + offset;
    v.Count = acc.count;
    v.Stride = stride;
    v.ComponentType = acc.componentType;
    v.Valid = true;
    return v;
}

// FLOAT-аксессор (позиции, нормали, UV, касательные) в плоский массив.
bool ReadFloats(const tinygltf::Model& gltf, int accessorIndex, int components,
                std::vector<float>& out) {
    const AccessorView v = OpenAccessor(gltf, accessorIndex, sizeof(float) * (size_t)components);
    if (!v.Valid || v.ComponentType != TINYGLTF_COMPONENT_TYPE_FLOAT) return false;
    out.resize(v.Count * (size_t)components);
    for (size_t i = 0; i < v.Count; ++i) {
        float tmp[4];
        std::memcpy(tmp, v.Base + i * v.Stride, sizeof(float) * (size_t)components);
        for (int c = 0; c < components; ++c) out[i * (size_t)components + (size_t)c] = tmp[c];
    }
    return true;
}

bool ReadIndices(const tinygltf::Model& gltf, int accessorIndex, size_t vertexCount,
                 std::vector<unsigned int>& out) {
    size_t elem = 0;
    if (accessorIndex >= 0 && accessorIndex < (int)gltf.accessors.size()) {
        switch (gltf.accessors[(size_t)accessorIndex].componentType) {
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:  elem = 1; break;
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: elem = 2; break;
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:   elem = 4; break;
            default: return false;
        }
    }
    const AccessorView v = OpenAccessor(gltf, accessorIndex, elem);
    if (!v.Valid) return false;
    out.resize(v.Count);
    for (size_t i = 0; i < v.Count; ++i) {
        const unsigned char* p = v.Base + i * v.Stride;
        unsigned int idx = 0;
        switch (v.ComponentType) {
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: idx = *p; break;
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
                uint16_t s = 0; std::memcpy(&s, p, sizeof(s)); idx = s; break;
            }
            default: {
                uint32_t s = 0; std::memcpy(&s, p, sizeof(s)); idx = s; break;
            }
        }
        // Индекс за пределами вершин примитива — битый файл. Схлопываем в 0:
        // треугольник выродится, но чтения за границей не будет.
        out[i] = idx < vertexCount ? idx : 0u;
    }
    return true;
}

// --- Материалы ---------------------------------------------------------------

// Внешняя картинка (у .gltf они лежат файлами рядом) — путём относительно
// модели. Встроенная (.glb, data:) — пусто: файла на диске нет, и выкладывать
// его отсюда нечем и незачем. Тем занят ModelLoader::ExtractMaterials, который
// пишет .sagemat.
std::string ImageUri(const tinygltf::Model& gltf, int textureIndex) {
    if (textureIndex < 0 || textureIndex >= (int)gltf.textures.size()) return {};
    const int image = gltf.textures[(size_t)textureIndex].source;
    if (image < 0 || image >= (int)gltf.images.size()) return {};
    const std::string& uri = gltf.images[(size_t)image].uri;
    if (uri.empty() || uri.rfind("data:", 0) == 0) return {};
    return uri;
}

void ReadMaterials(const tinygltf::Model& gltf, ImportedScene& out) {
    // ПОРЯДОК ВАЖЕН: индекс материала в Submesh — это индекс в этом списке, и
    // он же индекс в model.materials. Пропустить или переставить хоть один
    // значит покрасить части модели чужими материалами.
    out.Materials.reserve(gltf.materials.size());
    for (const tinygltf::Material& m : gltf.materials) {
        ImportedMaterial im;
        im.Name = m.name;
        const tinygltf::PbrMetallicRoughness& pbr = m.pbrMetallicRoughness;
        if (pbr.baseColorFactor.size() >= 4) {
            im.Albedo = glm::vec3((float)pbr.baseColorFactor[0], (float)pbr.baseColorFactor[1],
                                  (float)pbr.baseColorFactor[2]);
            im.Opacity = (float)pbr.baseColorFactor[3];
        }
        im.Metallic = (float)pbr.metallicFactor;
        im.Roughness = (float)pbr.roughnessFactor;
        if (m.emissiveFactor.size() >= 3) {
            im.Emissive = glm::vec3((float)m.emissiveFactor[0], (float)m.emissiveFactor[1],
                                    (float)m.emissiveFactor[2]);
        }
        im.AlbedoTexture = ImageUri(gltf, pbr.baseColorTexture.index);
        im.NormalTexture = ImageUri(gltf, m.normalTexture.index);
        im.EmissiveTexture = ImageUri(gltf, m.emissiveTexture.index);
        // metallic/roughness лежат в РАЗНЫХ каналах одной текстуры, а AO — в
        // своей (часто той же). Здесь пишется только ссылка на файл; кто из
        // каналов чей, разбирает ExtractMaterials, когда пишет .sagemat.
        im.MetallicTexture = ImageUri(gltf, pbr.metallicRoughnessTexture.index);
        im.RoughnessTexture = im.MetallicTexture;
        im.AOTexture = ImageUri(gltf, m.occlusionTexture.index);
        out.Materials.push_back(std::move(im));
    }
}

// --- Обход узлов -------------------------------------------------------------

std::string PrimitiveName(const tinygltf::Model& gltf, const tinygltf::Node& node, int meshIndex,
                          size_t primIndex, size_t primCount) {
    std::string name = node.name;
    if (name.empty() && meshIndex >= 0 && meshIndex < (int)gltf.meshes.size())
        name = gltf.meshes[(size_t)meshIndex].name;
    if (name.empty()) name = "node";
    // Номер части дописываем, только если частей больше одной: у обычного
    // односоставного узла имя должно совпадать с именем из Blender, иначе
    // сопоставить слот материала с тем, что видно в файле, не получится.
    if (primCount > 1) name += "#" + std::to_string(primIndex);
    return name;
}

void CollectPrimitive(const tinygltf::Model& gltf, const tinygltf::Primitive& prim,
                      const glm::mat4& world, std::string name, ImportedScene& out) {
    if (prim.mode != TINYGLTF_MODE_TRIANGLES) return; // линии и точки геометрией не считаем
    auto posIt = prim.attributes.find("POSITION");
    if (posIt == prim.attributes.end()) return;

    std::vector<float> positions;
    if (!ReadFloats(gltf, posIt->second, 3, positions) || positions.empty()) {
        out.Warnings.push_back("часть «" + name + "» пропущена: позиции вершин не читаются");
        return;
    }
    const size_t count = positions.size() / 3;

    std::vector<float> normals, uvs;
    auto normIt = prim.attributes.find("NORMAL");
    const bool hasNormals =
        normIt != prim.attributes.end() && ReadFloats(gltf, normIt->second, 3, normals) &&
        normals.size() >= count * 3;
    auto uvIt = prim.attributes.find("TEXCOORD_0");
    const bool hasUV = uvIt != prim.attributes.end() && ReadFloats(gltf, uvIt->second, 2, uvs) &&
                       uvs.size() >= count * 2;

    ImportedNode node;
    node.Name = std::move(name);
    node.Transform = world;
    node.MaterialIndex = (prim.material >= 0 && prim.material < (int)gltf.materials.size())
                             ? prim.material : -1;
    node.Mesh.Vertices.resize(count);
    for (size_t i = 0; i < count; ++i) {
        Vertex& v = node.Mesh.Vertices[i];
        v.Position = glm::vec3(positions[i * 3], positions[i * 3 + 1], positions[i * 3 + 2]);
        v.Normal = hasNormals ? glm::vec3(normals[i * 3], normals[i * 3 + 1], normals[i * 3 + 2])
                              : glm::vec3(0.0f, 1.0f, 0.0f);
        v.TexCoords = hasUV ? glm::vec2(uvs[i * 2], uvs[i * 2 + 1]) : glm::vec2(0.0f);
    }

    if (prim.indices >= 0) {
        if (!ReadIndices(gltf, prim.indices, count, node.Mesh.Indices)) {
            out.Warnings.push_back("часть «" + node.Name + "» пропущена: индексы не читаются");
            return;
        }
    } else {
        node.Mesh.Indices.resize(count);
        for (size_t i = 0; i < count; ++i) node.Mesh.Indices[i] = (unsigned int)i;
    }
    if (node.Mesh.Indices.size() < 3) return;

    out.Nodes.push_back(std::move(node));
}

void CollectNode(const tinygltf::Model& gltf, int nodeIndex, const glm::mat4& parent,
                 std::vector<bool>& visited, ImportedScene& out) {
    if (nodeIndex < 0 || nodeIndex >= (int)gltf.nodes.size()) return;
    // Узел, встреченный дважды, — это либо кривой файл, либо цикл в дереве.
    // Без этой отметки цикл означал бы бесконечную рекурсию на загрузке модели.
    if (visited[(size_t)nodeIndex]) return;
    visited[(size_t)nodeIndex] = true;

    const tinygltf::Node& node = gltf.nodes[(size_t)nodeIndex];
    const glm::mat4 world = parent * NodeLocalMatrix(node);

    if (node.mesh >= 0 && node.mesh < (int)gltf.meshes.size()) {
        const tinygltf::Mesh& mesh = gltf.meshes[(size_t)node.mesh];
        for (size_t p = 0; p < mesh.primitives.size(); ++p) {
            CollectPrimitive(gltf, mesh.primitives[p], world,
                             PrimitiveName(gltf, node, node.mesh, p, mesh.primitives.size()), out);
        }
    }
    for (int child : node.children) CollectNode(gltf, child, world, visited, out);
}

} // namespace

bool ImportGltf(const std::string& path, ImportedScene& out, std::string& err) {
    const std::string ext = fs::path(path).extension().string();
    const bool binary = ext == ".glb" || ext == ".GLB";

    tinygltf::TinyGLTF loader;
    loader.SetImageLoader(&DecodeImage, nullptr);
    tinygltf::Model gltf;
    std::string parseErr, warn;
    const bool ok = binary ? loader.LoadBinaryFromFile(&gltf, &parseErr, &warn, path)
                           : loader.LoadASCIIFromFile(&gltf, &parseErr, &warn, path);
    if (!ok) {
        err = "не удалось разобрать glTF " + path + ": " +
              (parseErr.empty() ? std::string("неизвестная ошибка") : parseErr);
        return false;
    }
    if (!warn.empty()) out.Warnings.push_back(warn);

    ReadMaterials(gltf, out);

    std::vector<bool> visited(gltf.nodes.size(), false);
    if (!gltf.scenes.empty()) {
        const size_t sceneIndex =
            (gltf.defaultScene >= 0 && gltf.defaultScene < (int)gltf.scenes.size())
                ? (size_t)gltf.defaultScene : 0;
        for (int root : gltf.scenes[sceneIndex].nodes)
            CollectNode(gltf, root, glm::mat4(1.0f), visited, out);
    }
    // Узлы, не попавшие ни в одну сцену (или файл вовсе без сцен): экспортёры
    // такое делают, и терять из-за этого геометрию нельзя — «модель пустая»
    // выглядит как поломка движка, а не как особенность файла.
    for (size_t i = 0; i < gltf.nodes.size(); ++i) {
        if (visited[i]) continue;
        const tinygltf::Node& n = gltf.nodes[i];
        if (n.mesh < 0) continue;   // пустышки и кости сами по себе не геометрия
        CollectNode(gltf, (int)i, glm::mat4(1.0f), visited, out);
    }

    if (out.Nodes.empty()) {
        err = "в glTF нет треугольной геометрии: " + path;
        return false;
    }
    return true;
}

} // namespace sage::assets
