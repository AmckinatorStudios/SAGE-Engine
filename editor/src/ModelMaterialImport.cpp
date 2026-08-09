#include "ModelMaterialImport.h"

#include <filesystem>
#include <system_error>

#include "Project.h"
#include "sage/assets/AssetDatabase.h"
#include "sage/core/Log.h"
#include "sage/render/Material.h"
#include "sage/render/ModelMaterial.h"
#include "sage/render/ResourceManager.h"
#include "sage/scene/Components.h"

namespace fs = std::filesystem;

namespace {

// Имя файла материала. У одноматериальной модели — <модель>.sagemat, как было
// всегда: менять имена уже созданным материалам ради единообразия значило бы
// сломать ссылки в существующих сценах. У многоматериальной к имени модели
// добавляется имя материала — иначе четырнадцать материалов писались бы в один
// файл, и остался бы последний.
fs::path MaterialFileFor(const fs::path& modelPath, const ModelLoader::ExtractedMaterial& material,
                         size_t index, size_t total) {
    if (total <= 1) return fs::path(modelPath).replace_extension(".sagemat");

    std::string name;
    for (unsigned char c : material.Name) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                        (c >= 'a' && c <= 'z') || c == '-' || c == '_' || c >= 0x80;
        name.push_back(ok ? (char)c : '_');
    }
    if (name.empty()) name = "mat" + std::to_string(index);
    return modelPath.parent_path() / (modelPath.stem().string() + "_" + name + ".sagemat");
}

// Записывает .sagemat, если его ещё нет. Возвращает путь к файлу (существующему
// или новому) и отмечает, создавали ли его.
fs::path EnsureMaterialFile(const Project& project, const fs::path& path,
                            const ModelLoader::ExtractedMaterial& ex, bool& created,
                            std::string& firstWarning) {
    std::error_code ec;
    if (fs::exists(path, ec)) return path;

    Material mat;
    mat.Albedo = ex.Albedo;
    mat.Emissive = ex.Emissive;
    mat.EmissiveStrength = ex.EmissiveStrength;
    mat.Metallic = ex.Metallic;
    mat.Roughness = ex.Roughness;
    mat.Opacity = ex.Opacity;
    // Пути карт — относительно проекта: материал переживёт сборку игры и переезд
    // проекта (см. Project::AssetRef).
    mat.TexturePath = project.AssetRef(ex.AlbedoMap);
    mat.NormalMapPath = project.AssetRef(ex.NormalMap);
    mat.MetallicMapPath = project.AssetRef(ex.MetallicMap);
    mat.RoughnessMapPath = project.AssetRef(ex.RoughnessMap);
    mat.AOMapPath = project.AssetRef(ex.AOMap);
    mat.EmissiveMap = project.AssetRef(ex.EmissiveMap);

    try {
        mat.SaveToFile(path.string());
    } catch (const std::exception& e) {
        LOG_ERROR("Editor") << "материал модели не сохранён: " << e.what();
        if (firstWarning.empty()) firstWarning = e.what();
        return {};
    }
    sage::AssetDatabase::Instance().Register(path.string(), "material");
    created = true;
    return path;
}

} // namespace

ModelMaterialImportResult ImportModelMaterials(const Project& project, MeshRendererComponent& mr) {
    ModelMaterialImportResult result;
    if (mr.Ref.path.empty()) return result;

    // Путь к самому файлу модели: в Ref он относительный (см. Project::AssetRef),
    // а читать надо настоящий файл на диске.
    const std::string modelPath = sage::AssetDatabase::Instance().LocatePath(mr.Ref.path);
    if (modelPath.empty()) return result;

    // Разметка меша — источник числа слотов. Её нет только у процедурных
    // примитивов и форматов без материалов; там и импортировать нечего сверх
    // одного материала объекта.
    const std::vector<sage::render::Submesh> submeshes =
        mr.MeshPtr ? mr.MeshPtr->Submeshes() : std::vector<sage::render::Submesh>{};

    // Материала объекта нет и разметки нет — старый однослотовый случай: модель
    // получает один материал, и слоты ей не нужны.
    const bool singleMaterial = submeshes.size() <= 1;
    if (singleMaterial && !mr.MaterialPath.empty()) return result; // выбор человека главнее

    const ModelLoader::ExtractedMaterialSet set = ModelLoader::ExtractMaterials(modelPath);
    if (!set.Warnings.empty()) result.FirstWarning = set.Warnings.front();
    // Нет материалов в файле — и не надо: белая болванка это честный результат
    // «в модели материалов нет», а не поломка.
    if (!set.Found()) return result;

    // Файлы материалов создаются ЛЕНИВО и по требованию разметки: материал, на
    // который не ссылается ни одна часть модели, в проект не попадает — иначе
    // рядом с моделью оседали бы .sagemat, которых никто не рисует.
    std::vector<std::string> files(set.Materials.size());
    std::vector<bool> resolved(set.Materials.size(), false);
    auto fileFor = [&](int index) -> std::string {
        if (index < 0 || (size_t)index >= set.Materials.size()) return {};
        if (resolved[(size_t)index]) return files[(size_t)index];
        resolved[(size_t)index] = true;
        bool created = false;
        const fs::path path =
            MaterialFileFor(fs::path(modelPath), set.Materials[(size_t)index], (size_t)index,
                            set.Materials.size());
        const fs::path written =
            EnsureMaterialFile(project, path, set.Materials[(size_t)index], created,
                               result.FirstWarning);
        if (created) ++result.Created;
        if (set.Materials[(size_t)index].HasAnyMap()) result.AnyMaps = true;
        files[(size_t)index] = written.empty() ? std::string() : project.AssetRef(written);
        return files[(size_t)index];
    };

    if (singleMaterial) {
        const std::string ref = fileFor(0);
        if (ref.empty()) return result;
        mr.MaterialPath = ref;
        mr.MaterialPtr = ResourceManager::Instance().GetMaterial(ref);
        result.Assigned = 1;
        return result;
    }

    // Многоматериальная модель: слот на КАЖДУЮ часть меша, по индексу материала
    // из разметки. Части без материала в файле остаются с пустым слотом — их
    // красит материал объекта (см. MaterialForSubmesh).
    mr.Slots.resize(submeshes.size());
    for (size_t i = 0; i < submeshes.size(); ++i) {
        if (!mr.Slots[i].Path.empty()) continue;   // выбор человека главнее импорта
        const std::string ref = fileFor(submeshes[i].Material);
        if (ref.empty()) continue;
        mr.Slots[i].Path = ref;
        mr.Slots[i].Ptr = ResourceManager::Instance().GetMaterial(ref);
        ++result.Assigned;
    }
    return result;
}
