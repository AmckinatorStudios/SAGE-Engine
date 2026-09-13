#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <vector>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/euler_angles.hpp>

#include "sage/assets/import/FbxTree.h"
#include "sage/assets/import/Importer.h"
#include "sage/core/Log.h"

// ---------------------------------------------------------------------------
// FBX (.fbx) — двоичный: СТАТИЧЕСКАЯ геометрия и материалы.
//
// До сих пор движок открывал .obj, .gltf/.glb, .blend и
// .bbmodel. Список выглядит достаточным ровно до первой встречи с чужой
// моделью: FBX — то, во что по умолчанию экспортируют Blender, Maya, 3ds Max,
// Mixamo, Sketchfab и любой ассет-стор. Человек, скачавший модель, получал
// «формат не поддерживается» и вывод «свою модель загрузить нельзя».
//
// МЕХАНИКА ФОРМАТА (запись, свойства, сжатые массивы, разбиение полигонов) —
// общая для всех читателей FBX и живёт в FbxTree.h. Здесь — только смысл:
// какие узлы считать мешами, откуда брать их трансформ и материал.
//
// ЧТО БЕРЁТСЯ. Геометрия: позиции, нормали, UV первого слоя, разбиение
// многоугольников на треугольники. Единицы приводятся к метрам по
// UnitScaleFactor из GlobalSettings (FBX почти всегда в сантиметрах), ось
// «вверх» — по UpAxis (Z-up разворачивается в Y-up движка). Трансформы узлов
// берутся из Model::Lcl Translation/Rotation/Scaling.
//
// СКИН И АНИМАЦИЯ СЮДА НЕ ВХОДЯТ — и это не пробел: их читает ОТДЕЛЬНЫЙ путь
// (FbxSkin.cpp), потому что и результат у него другой — не статический меш, а
// скелетная модель с костями и клипами. Здесь скиновая модель нужна как
// геометрия (обложка в панели ассетов, габариты, пикинг), и именно ею она и
// отдаётся — в позе привязки.
//
// ТЕКСТОВЫЙ FBX (старый, до 2013) не разбирается, и вместо молчания об этом
// говорится прямым текстом с указанием, что делать.
// ---------------------------------------------------------------------------

namespace sage::assets {
namespace {

namespace fs = std::filesystem;

using fbx::Doubles;
using fbx::Ints;
using fbx::Node;
using fbx::Property70;
using fbx::Property70Vec;
using fbx::Text;

// Геометрия одного узла Geometry -> MeshData. Разбор треугольников — общий
// (fbx::BuildCorners), здесь углы лишь раскладываются по вершинам и индексам.
sage::render::MeshData BuildMesh(const Node& geometry, const fbx::Units& units,
                                 const glm::mat4& nodeXform,
                                 std::vector<std::string>& warnings) {
    sage::render::MeshData mesh;
    const std::vector<fbx::MeshCorner> corners =
        fbx::BuildCorners(geometry, units, nodeXform, warnings);
    mesh.Vertices.reserve(corners.size());
    mesh.Indices.reserve(corners.size());
    for (const fbx::MeshCorner& c : corners) {
        Vertex v;
        v.Position = c.Position;
        v.Normal = c.Normal;
        v.TexCoords = c.TexCoords;
        mesh.Indices.push_back((unsigned int)mesh.Vertices.size());
        mesh.Vertices.push_back(v);
    }
    return mesh;
}

} // namespace

bool ImportFbx(const std::string& path, ImportedScene& out, std::string& err) {
    // Механика формата — общая (FbxTree.h): файл, дерево записей, единицы и
    // ось «вверх» читаются одним кодом и для статики, и для скина.
    Node root;
    if (!fbx::ReadTree(path, root, err)) return false;
    const fbx::Units units = fbx::ReadUnits(root);

    const Node* objects = root.Find("Objects");
    if (!objects) {
        err = "в файле FBX нет блока Objects";
        return false;
    }

    // --- Узлы Model: имя и ТРАНСФОРМ -----------------------------------
    //
    // Раньше отсюда бралось только имя, а положение, поворот и масштаб узла
    // игнорировались вовсе. Для файла с одним мешем это сходило с рук; модель
    // из нескольких частей — а это любая настоящая модель — приезжала кучей в
    // начале координат: 53 детали рюкзака ложились друг на друга, и весь он
    // получался размером с одну деталь.
    struct ModelInfo {
        std::string Name;
        glm::mat4 Local{1.0f};
        glm::mat4 Geometric{1.0f};
    };
    std::unordered_map<int64_t, ModelInfo> models;
    std::vector<std::string> modelNames; // порядок — запасной путь сопоставления
    for (const Node& n : objects->Children) {
        if (n.Name != "Model") continue;
        std::string name = n.Props.size() > 1 ? n.Props[1].Text : std::string();
        const size_t sep = name.find('\0');
        if (sep != std::string::npos) name = name.substr(0, sep);
        modelNames.push_back(name);

        ModelInfo info;
        info.Name = name;
        auto compose = [](const glm::vec3& t, const glm::vec3& r, const glm::vec3& s) {
            // Порядок поворота FBX по умолчанию — XYZ, то есть Rz * Ry * Rx.
            glm::mat4 m = glm::translate(glm::mat4(1.0f), t);
            m *= glm::eulerAngleZYX(glm::radians(r.z), glm::radians(r.y), glm::radians(r.x));
            return glm::scale(m, s);
        };
        info.Local = compose(Property70Vec(&n, "Lcl Translation", glm::vec3(0.0f)),
                             Property70Vec(&n, "Lcl Rotation", glm::vec3(0.0f)),
                             Property70Vec(&n, "Lcl Scaling", glm::vec3(1.0f)));
        // Геометрический трансформ применяется ТОЛЬКО к мешу узла и не
        // наследуется детьми — этим он и отличается от Lcl.
        info.Geometric = compose(Property70Vec(&n, "GeometricTranslation", glm::vec3(0.0f)),
                                 Property70Vec(&n, "GeometricRotation", glm::vec3(0.0f)),
                                 Property70Vec(&n, "GeometricScaling", glm::vec3(1.0f)));
        if (!n.Props.empty()) models[(int64_t)n.Props[0].Number] = info;
    }

    // Связи: какой объект чей ребёнок. Ими же геометрия привязывается к своей
    // модели — по порядку это угадывалось и ломалось на файлах, где Geometry и
    // Model перечислены вперемешку.
    std::unordered_map<int64_t, int64_t> parentOf;
    if (const Node* conns = root.Find("Connections")) {
        for (const Node& c : conns->Children) {
            if (c.Name != "C" || c.Props.size() < 3) continue;
            if (c.Props[0].Text != "OO") continue;
            parentOf[(int64_t)c.Props[1].Number] = (int64_t)c.Props[2].Number;
        }
    }
    // Мировая матрица модели — произведение локальных вверх по цепочке.
    auto worldOf = [&](int64_t uid) {
        glm::mat4 m(1.0f);
        int guard = 0;
        for (int64_t cur = uid; cur != 0 && guard++ < 64;) {
            auto it = models.find(cur);
            if (it == models.end()) break;
            m = it->second.Local * m;
            auto p = parentOf.find(cur);
            if (p == parentOf.end()) break;
            cur = p->second;
        }
        return m;
    };

    // --- Материалы и текстуры -------------------------------------------
    //
    // FBX держит их ОТДЕЛЬНЫМИ объектами, связанными через Connections:
    // Texture --OP("DiffuseColor")--> Material --OO--> Model. Пока эти блоки не
    // читались, материала у FBX не было вовсе: модель приезжала серой, а лежащие
    // рядом карты (albedo, normal, roughness) движок в глаза не видел — их
    // приходилось назначать руками по одной.
    struct FbxTexture {
        std::string File;      // RelativeFilename либо FileName
    };
    std::unordered_map<int64_t, FbxTexture> textures;
    for (const Node& n : objects->Children) {
        if (n.Name != "Texture" || n.Props.empty()) continue;
        FbxTexture tex;
        if (const Node* rel = n.Find("RelativeFilename")) {
            if (!rel->Props.empty()) tex.File = rel->Props[0].Text;
        }
        if (tex.File.empty()) {
            if (const Node* abs = n.Find("FileName")) {
                if (!abs->Props.empty()) tex.File = abs->Props[0].Text;
            }
        }
        // Пути в FBX почти всегда windows-овые и почти всегда чужие
        // («C:\Users\...\bag_albedo.png»): берём ИМЯ ФАЙЛА и ищем его рядом
        // с моделью — так набор, скачанный одной папкой, собирается сам.
        for (char& c : tex.File) if (c == '\\') c = '/';
        if (!tex.File.empty()) textures[(int64_t)n.Props[0].Number] = tex;
    }

    struct FbxMaterial {
        std::string Name;
        glm::vec3 Diffuse{1.0f};
        float Shininess = -1.0f;
        std::unordered_map<std::string, int64_t> Slots; // имя свойства -> uid текстуры
    };
    std::unordered_map<int64_t, FbxMaterial> materials;
    for (const Node& n : objects->Children) {
        if (n.Name != "Material" || n.Props.empty()) continue;
        FbxMaterial m;
        m.Name = n.Props.size() > 1 ? n.Props[1].Text : std::string();
        const size_t sep = m.Name.find('\0');
        if (sep != std::string::npos) m.Name = m.Name.substr(0, sep);
        m.Diffuse = Property70Vec(&n, "DiffuseColor", glm::vec3(1.0f));
        m.Shininess = (float)Property70(&n, "Shininess", -1.0);
        materials[(int64_t)n.Props[0].Number] = m;
    }

    // Связи «по свойству» (OP): текстура привязана к КОНКРЕТНОМУ слоту
    // материала, и без имени свойства нормаль неотличима от альбедо.
    std::unordered_multimap<int64_t, int64_t> materialOfModel;
    if (const Node* conns = root.Find("Connections")) {
        for (const Node& c : conns->Children) {
            if (c.Name != "C" || c.Props.size() < 3) continue;
            const int64_t child = (int64_t)c.Props[1].Number;
            const int64_t parent = (int64_t)c.Props[2].Number;
            if (c.Props[0].Text == "OP" && c.Props.size() >= 4) {
                auto mat = materials.find(parent);
                if (mat != materials.end() && textures.count(child)) {
                    mat->second.Slots[c.Props[3].Text] = child;
                }
            } else if (c.Props[0].Text == "OO") {
                if (materials.count(child)) materialOfModel.emplace(parent, child);
            }
        }
    }

    // Слот FBX -> карта движка. Имена свойств у экспортёров разнятся, поэтому
    // проверяется несколько вариантов на каждую карту.
    auto slotFile = [&](const FbxMaterial& m, std::initializer_list<const char*> names) {
        for (const char* name : names) {
            auto it = m.Slots.find(name);
            if (it == m.Slots.end()) continue;
            auto tex = textures.find(it->second);
            if (tex != textures.end() && !tex->second.File.empty()) return tex->second.File;
        }
        return std::string();
    };

    std::unordered_map<int64_t, int> materialIndexOf; // uid материала -> индекс в out.Materials
    auto materialIndex = [&](int64_t uid) {
        auto known = materialIndexOf.find(uid);
        if (known != materialIndexOf.end()) return known->second;
        auto it = materials.find(uid);
        if (it == materials.end()) return -1;
        const FbxMaterial& m = it->second;
        ImportedMaterial out_m;
        out_m.Name = m.Name;
        out_m.Albedo = m.Diffuse;
        // Блеск Phong -> шероховатость PBR. Точного перевода нет ни у кого;
        // важно лишь, чтобы отполированный металл не приезжал матовым.
        if (m.Shininess > 0.0f) {
            out_m.Roughness = std::clamp(1.0f - std::sqrt(m.Shininess / 100.0f), 0.04f, 1.0f);
        }
        out_m.AlbedoTexture = slotFile(m, {"DiffuseColor", "Maya|baseColor", "BaseColor"});
        out_m.NormalTexture = slotFile(m, {"NormalMap", "Bump", "Maya|normalCamera"});
        out_m.MetallicTexture = slotFile(m, {"Maya|metalness", "MetalnessMap", "ReflectionFactor"});
        out_m.RoughnessTexture = slotFile(m, {"Maya|specularRoughness", "ShininessExponent"});
        out_m.AOTexture = slotFile(m, {"AmbientColor", "Maya|ambientOcclusion"});
        out_m.EmissiveTexture = slotFile(m, {"EmissiveColor"});
        out.Materials.push_back(out_m);
        const int index = (int)out.Materials.size() - 1;
        materialIndexOf[uid] = index;
        return index;
    };

    size_t geometryIndex = 0;
    for (const Node& n : objects->Children) {
        if (n.Name != "Geometry") continue;
        glm::mat4 xform(1.0f);
        std::string nodeName;
        int meshMaterial = -1;
        if (!n.Props.empty()) {
            auto owner = parentOf.find((int64_t)n.Props[0].Number);
            if (owner != parentOf.end()) {
                auto mi = models.find(owner->second);
                if (mi != models.end()) {
                    xform = worldOf(owner->second) * mi->second.Geometric;
                    nodeName = mi->second.Name;
                    auto mat = materialOfModel.find(owner->second);
                    if (mat != materialOfModel.end()) meshMaterial = materialIndex(mat->second);
                }
            }
        }
        sage::render::MeshData mesh = BuildMesh(n, units, xform, out.Warnings);
        if (mesh.Empty()) {
            ++geometryIndex;
            continue;
        }
        ImportedNode node;
        node.Name = !nodeName.empty()
                        ? nodeName
                        : (geometryIndex < modelNames.size() && !modelNames[geometryIndex].empty()
                               ? modelNames[geometryIndex]
                               : fs::path(path).stem().string());
        node.Mesh = std::move(mesh);
        node.MaterialIndex = meshMaterial;
        out.Nodes.push_back(std::move(node));
        ++geometryIndex;
    }

    if (out.Nodes.empty()) {
        err = "в файле FBX не нашлось геометрии (возможно, там только кости, "
              "камеры или кривые — экспортируйте меши)";
        return false;
    }

    return true;
}

} // namespace sage::assets
