#include "ModelLoader.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <stdexcept>

#include "sage/assets/Pack.h"
#include "sage/assets/import/Importer.h"
#include "sage/assets/import/MeshNormalize.h"
#include "sage/core/Log.h"
#include "sage/render/MeshData.h"

// Настройки .sageimport и разбор .obj — в слое ресурсов
// (sage/assets/import/ImportSettings.cpp, ObjImporter.cpp); здесь — только
// то, что строит из их данных Mesh.

namespace ModelLoader {

std::shared_ptr<Mesh> LoadObj(const std::string& path) {
    sage::render::MeshData d = LoadObjData(path);
    return std::make_shared<Mesh>(d.Vertices, d.Indices, d.Submeshes);
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
    ApplyImportSettings(d, ResolveImportSettings(path, d));
    return d;
}

sage::render::MeshData LoadMeshData(const std::string& path) {
    // Отсутствующий файл отличаем от битого ЗДЕСЬ: ниже оба выглядят как
    // «парсер не смог», а человеку, у которого «модель не грузится», нужно
    // знать, опечатался он в пути или у него испорченный экспорт.
    if (!sage::assets::vfs::Exists(path)) {
        throw std::runtime_error("Файл модели не найден: " + path);
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
            ApplyImportSettings(data, ResolveImportSettings(path, data));
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

bool LoadModelTree(const std::string& path, sage::assets::ImportedScene& scene, glm::mat4& rootMatrix,
                   std::string& err) {
    // ДЕРЕВО ОДНО НА ФАЙЛ, а частей у модели десятки: каждая грузится своим
    // запросом, и разбирать ради каждой весь файл заново значило бы платить
    // N разборов за одну модель. Помним последний разобранный файл (по пути и
    // времени правки) — части одной модели грузятся подряд.
    struct Cached {
        std::string Key;
        sage::assets::ImportedScene Scene;
        glm::mat4 Root{1.0f};
    };
    static std::mutex mx;
    static Cached cache;
    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(path, ec);
    const std::string key =
        path + "|" + std::to_string(ec ? 0 : (long long)stamp.time_since_epoch().count());
    {
        std::lock_guard<std::mutex> lk(mx);
        if (cache.Key == key) {
            scene = cache.Scene;
            rootMatrix = cache.Root;
            return true;
        }
    }
    sage::assets::ImportedScene fresh;
    if (!sage::assets::ImporterRegistry::Instance().Import(path, fresh, err)) return false;
    glm::mat4 root(1.0f);
    // .obj приходит из LoadObjData уже с настройками импорта — второй раз их
    // не применяем.
    if (ExtensionLower(path) != "obj") {
        const sage::render::MeshData whole = fresh.Flatten();
        glm::vec3 lo(std::numeric_limits<float>::max()), hi(std::numeric_limits<float>::lowest());
        for (const Vertex& v : whole.Vertices) {
            lo = glm::min(lo, v.Position);
            hi = glm::max(hi, v.Position);
        }
        root = ImportMatrix(ResolveImportSettings(path, lo, hi), lo, hi);
    }
    {
        std::lock_guard<std::mutex> lk(mx);
        cache.Key = key;
        cache.Scene = fresh;
        cache.Root = root;
    }
    scene = std::move(fresh);
    rootMatrix = root;
    return true;
}

sage::render::MeshData LoadMeshPartData(const std::string& path, int group) {
    if (!sage::assets::vfs::Exists(path)) throw std::runtime_error("Файл модели не найден: " + path);
    sage::assets::ImportedScene scene;
    glm::mat4 root(1.0f);
    std::string err;
    if (!LoadModelTree(path, scene, root, err)) throw std::runtime_error(err);
    if (group < 0 || group >= (int)scene.Groups.size())
        throw std::runtime_error("в модели нет части №" + std::to_string(group) + ": " + path +
                                 " (её переэкспортировали с другим составом?)");
    sage::render::MeshData d = scene.FlattenGroup(group);
    // Настройки, меняющие ГЕОМЕТРИЮ части (развёртка, обход, нормали), — ей;
    // размер, поворот и место — корню иерархии (см. LoadModelTree).
    if (ExtensionLower(path) != "obj") {
        ImportSettings s = LoadImportSettings(path);
        s.Scale = 1.0f;
        s.Recenter = false;
        s.NormalizeSize = false;
        s.Rotation = glm::vec3(0.0f);
        s.Offset = glm::vec3(0.0f);
        ApplyImportSettings(d, s);
    }
    return d;
}

std::shared_ptr<Mesh> LoadMeshPart(const std::string& path, int group, bool keepCpuData) {
    sage::render::MeshData d = LoadMeshPartData(path, group);
    // Пустой узел (группа без своей геометрии) — законный: объекту нечего
    // рисовать, но место в дереве он держит.
    if (d.Empty()) return nullptr;
    return std::make_shared<Mesh>(d.Vertices, d.Indices, d.Submeshes, keepCpuData);
}

}
