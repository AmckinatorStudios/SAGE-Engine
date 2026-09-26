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

#include "sage/assets/import/FbxTree.h"
#include "sage/assets/import/Importer.h"
#include "sage/core/Log.h"

// ---------------------------------------------------------------------------
// FBX (.fbx) — двоичный: СТАТИЧЕСКАЯ геометрия и материалы.
//
// До сих пор движок открывал .obj, .gltf/.glb, .blend и
// .bbmodel. Список выглядит достаточным ровно до первой встречи с чужой
// моделью: FBX — то, во что по умолчанию экспортируют 3D-редакторы,
// сервисы риггинга, магазины моделей и любой ассет-стор. Человек, скачавший модель, получал
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

// Углы одного материала -> MeshData. Разбор треугольников — общий
// (fbx::BuildCorners), здесь углы лишь раскладываются по вершинам и индексам.
sage::render::MeshData BuildMesh(const std::vector<fbx::MeshCorner>& corners, int material) {
    sage::render::MeshData mesh;
    for (const fbx::MeshCorner& c : corners) {
        if (c.Material != material) continue;
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
        // Полный трансформ FBX (предповорот, опоры, порядок осей) — общий со
        // скелетным разбором. Здесь стоял свой, урезанный до T*R*S, и детали
        // моделей из Maya/3ds Max, у которых разворот лежит в PreRotation и
        // опорах, разъезжались и поворачивались как попало.
        info.Local = fbx::ReadTransform(n).Matrix();
        // Геометрический трансформ применяется ТОЛЬКО к мешу узла и не
        // наследуется детьми — этим он и отличается от Lcl.
        info.Geometric = fbx::GeometricMatrix(n);
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
    //
    // Материалы модели — СПИСКОМ В ПОРЯДКЕ СВЯЗЕЙ: номер материала грани
    // (LayerElementMaterial) — это номер именно в нём. Здесь была хэш-таблица,
    // из которой брался один, «первый попавшийся» материал на всю геометрию.
    std::unordered_map<int64_t, std::vector<int64_t>> materialsOfModel;
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
                if (materials.count(child)) materialsOfModel[parent].push_back(child);
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
        if (m.Shininess >= 0.0f) {
            out_m.Roughness = std::clamp(1.0f - std::sqrt(m.Shininess / 100.0f), 0.04f, 1.0f);
        }
        out_m.AlbedoTexture = slotFile(m, {"DiffuseColor", "Maya|baseColor", "BaseColor"});
        out_m.NormalTexture = slotFile(m, {"NormalMap", "Bump", "Maya|normalCamera"});
        out_m.MetallicTexture = slotFile(m, {"Maya|metalness", "MetalnessMap", "ReflectionFactor"});
        out_m.RoughnessTexture = slotFile(m, {"Maya|specularRoughness", "ShininessExponent"});
        out_m.AOTexture = slotFile(m, {"AmbientColor", "Maya|ambientOcclusion"});
        out_m.EmissiveTexture = slotFile(m, {"EmissiveColor"});
        // Карта прозрачности — вырез по альфе: листва, трава, решётки. 3D-редактор
        // кладёт её в TransparencyFactor, Maya и 3ds Max — в TransparentColor;
        // файл обычно тот же, что у альбедо (альфа-канал картинки). Без этого
        // карточки листьев приезжали сплошными квадратами и так же квадратами
        // отбрасывали тень.
        if (!slotFile(m, {"TransparencyFactor", "TransparentColor"}).empty()) {
            out_m.AlphaMode = 1;
            out_m.AlphaCutoff = 0.5f;
            // Карточка листа — одна плоскость, и видна она с обеих сторон.
            out_m.DoubleSided = true;
        }
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
        const std::vector<int64_t>* modelMaterials = nullptr;
        if (!n.Props.empty()) {
            auto owner = parentOf.find((int64_t)n.Props[0].Number);
            if (owner != parentOf.end()) {
                auto mi = models.find(owner->second);
                if (mi != models.end()) {
                    xform = worldOf(owner->second) * mi->second.Geometric;
                    nodeName = mi->second.Name;
                    auto mats = materialsOfModel.find(owner->second);
                    if (mats != materialsOfModel.end()) modelMaterials = &mats->second;
                }
            }
        }
        const std::vector<fbx::MeshCorner> corners =
            fbx::BuildCorners(n, units, xform, out.Warnings);
        if (corners.empty()) {
            ++geometryIndex;
            continue;
        }
        const std::string baseName =
            !nodeName.empty()
                ? nodeName
                : (geometryIndex < modelNames.size() && !modelNames[geometryIndex].empty()
                       ? modelNames[geometryIndex]
                       : fs::path(path).stem().string());

        // ПО УЗЛУ НА МАТЕРИАЛ. Один меш FBX часто покрашен несколькими
        // материалами по граням (дерево: кора и листва в одной геометрии).
        // Раньше вся геометрия получала один материал — и листва рисовалась
        // корой, сплошными непрозрачными квадратами.
        std::vector<int> used;
        for (const fbx::MeshCorner& c : corners)
            if (std::find(used.begin(), used.end(), c.Material) == used.end())
                used.push_back(c.Material);
        for (int local : used) {
            ImportedNode node;
            node.Name = used.size() > 1 ? baseName + "#" + std::to_string(local) : baseName;
            node.Mesh = BuildMesh(corners, local);
            if (modelMaterials && !modelMaterials->empty()) {
                const size_t k = local >= 0 && (size_t)local < modelMaterials->size()
                                     ? (size_t)local : 0;
                node.MaterialIndex = materialIndex((*modelMaterials)[k]);
            }
            out.Nodes.push_back(std::move(node));
        }
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
