#include "ModelMaterialImport.h"

#include <filesystem>
#include <system_error>

#include "Project.h"
#include "sage/assets/AssetDatabase.h"
#include "sage/core/Log.h"
#include "sage/render/Material.h"
#include "sage/anim/ClipFile.h"
#include "sage/render/ModelMaterial.h"
#include "sage/render/SkinnedModel.h"
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

    // Одна часть (или разметки нет вовсе) — старый однослотовый случай: модель
    // получает один материал ОБЪЕКТА, и слоты ей не нужны.
    const bool singleMaterial = submeshes.size() <= 1;
    if (singleMaterial && !mr.MaterialPath.empty()) return result; // выбор человека главнее

    const ModelLoader::ExtractedMaterialSet set = ModelLoader::ExtractMaterials(modelPath);
    if (!set.Warnings.empty()) result.FirstWarning = set.Warnings.front();
    result.FileMaterials = (int)set.Materials.size();
    result.Parts = (int)submeshes.size();
    // Нет материалов в файле — и не надо: белая болванка это честный результат
    // «в модели материалов нет», а не поломка. Но сказать об этом надо: молчание
    // здесь неотличимо от «импорт не сработал», а это разные вещи.
    if (!set.Found()) {
        LOG_WARN("Материалы") << "В модели нет материалов: " << mr.Ref.path
                              << " (частей " << submeshes.size() << ")"
                              << (result.FirstWarning.empty()
                                      ? std::string()
                                      : " — " + result.FirstWarning);
        return result;
    }

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
        // Берётся материал, на который ССЫЛАЕТСЯ эта часть, а не первый в
        // файле: в .glb с четырьмя материалами единственный примитив вполне
        // может пользоваться четвёртым, и «первый» покрасил бы его чужим.
        // Разметки нет — материал в файле один, и его индекс ноль.
        const int index = (submeshes.size() == 1 && submeshes[0].Material >= 0)
                              ? submeshes[0].Material : 0;
        const std::string ref = fileFor(index);
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
    int kept = 0, noMaterial = 0, notWritten = 0;
    for (size_t i = 0; i < submeshes.size(); ++i) {
        if (!mr.Slots[i].Path.empty()) { ++kept; continue; }  // выбор человека главнее импорта
        // Часть без материала в файле — законно (её красит материал объекта), а
        // вот часть С материалом, для которой не вышло имя файла, — отказ, и
        // молчать о нём нельзя: снаружи оба случая выглядят одинаково пустым
        // слотом.
        if (submeshes[i].Material < 0) { ++noMaterial; continue; }
        const std::string ref = fileFor(submeshes[i].Material);
        if (ref.empty()) { ++notWritten; continue; }
        mr.Slots[i].Path = ref;
        mr.Slots[i].Ptr = ResourceManager::Instance().GetMaterial(ref);
        ++result.Assigned;
    }

    // ИТОГ — ОДНОЙ СТРОКОЙ И ВСЕГДА. Раньше импорт молчал целиком: и когда
    // разложил тридцать семь материалов, и когда не разложил ни одного. Поэтому
    // на вопрос «почему модель белая» ответа в логе не было вовсе — там про неё
    // не было ни строчки.
    // КАКИЕ КАРТЫ ПРИЕХАЛИ — поимённо. «Загружается только albedo» — это первое,
    // что замечают, и до сих пор проверить это можно было лишь глазами по
    // слотам: набор, в котором лежат одни diffuse-текстуры, и движок, потерявший
    // остальные карты, выглядят совершенно одинаково.
    int withAlbedo = 0, withNormal = 0, withMetallic = 0, withRoughness = 0, withAO = 0,
        withEmissive = 0;
    for (const ModelLoader::ExtractedMaterial& m : set.Materials) {
        if (!m.AlbedoMap.empty()) ++withAlbedo;
        if (!m.NormalMap.empty()) ++withNormal;
        if (!m.MetallicMap.empty()) ++withMetallic;
        if (!m.RoughnessMap.empty()) ++withRoughness;
        if (!m.AOMap.empty()) ++withAO;
        if (!m.EmissiveMap.empty()) ++withEmissive;
    }
    std::string maps;
    auto note = [&maps](const char* name, int n) {
        if (n == 0) return;
        if (!maps.empty()) maps += ", ";
        maps += name;
        maps += " " + std::to_string(n);
    };
    note("albedo", withAlbedo);
    note("normal", withNormal);
    note("metallic", withMetallic);
    note("roughness", withRoughness);
    note("AO", withAO);
    note("emissive", withEmissive);
    LOG_INFO("Материалы") << "Карты модели " << mr.Ref.path << ": "
                          << (maps.empty() ? std::string("НИ ОДНОЙ — в файле только цвета") : maps);

    LOG_INFO("Материалы") << "Модель " << mr.Ref.path << ": частей " << submeshes.size()
                          << ", материалов в файле " << set.Materials.size()
                          << ", назначено " << result.Assigned
                          << (kept ? ", своих оставлено " + std::to_string(kept) : "")
                          << (noMaterial ? ", без материала в файле " + std::to_string(noMaterial)
                                         : "")
                          << (notWritten ? ", НЕ ЗАПИСАНО " + std::to_string(notWritten) : "")
                          << (result.Created ? ", создано файлов " + std::to_string(result.Created)
                                             : "");
    if (notWritten > 0 && !result.FirstWarning.empty())
        LOG_ERROR("Материалы") << "Причина: " << result.FirstWarning;
    return result;
}

namespace {

// Клип по умолчанию — ПОЗА ПОКОЯ.
//
// Персонажа ставят в сцену стоящим, а не в прыжке и не в момент смерти: из
// восемнадцати клипов «Jumpscare» и «Shutdown» правильным первым впечатлением
// не будет ни один. Ищем по имени — «idle» пишут почти все, в любом регистре и
// с любым префиксом экспортёра («SpringBonnie_LegacyFit--Idle»). Не нашли —
// берём первый: он всё равно лучше неподвижной позы привязки, по которой не
// видно даже, что скелет вообще работает.
int PreferredClipIndex(const std::vector<sage::anim::AnimationClip>& clips) {
    for (size_t i = 0; i < clips.size(); ++i) {
        std::string lower;
        lower.reserve(clips[i].Name.size());
        for (unsigned char c : clips[i].Name) lower.push_back((char)std::tolower(c));
        if (lower.find("idle") != std::string::npos) return (int)i;
    }
    return clips.empty() ? -1 : 0;
}

} // namespace

ModelMaterialImportResult& SetupModelAnimation(const Project& project, entt::registry& registry,
                                               entt::entity entity, const std::string& modelRef,
                                               ModelMaterialImportResult& result) {
    if (modelRef.empty() || !registry.valid(entity)) return result;

    // Скелет берём через кэш: ту же модель сейчас же попросит и сцена.
    std::shared_ptr<sage::render::SkinnedModel> model =
        ResourceManager::Instance().GetSkinnedModel(modelRef);
    if (!model || model->GetSkeleton().Count() == 0) {
        // Декорация без костей — это НЕ ошибка и не повод навешивать Animation:
        // компонент, которому нечего анимировать, только мешал бы в инспекторе.
        return result;
    }

    result.Skeleton = true;
    result.Bones = model->GetSkeleton().Count();

    // Клипы — файлами рядом с моделью (как и материалы). Делается здесь, а не
    // в SetEntityMesh: ссылки на записанные файлы нужны тут же, чтобы назначить
    // клип сущности, а не оставить его на диске.
    const ClipImportResult clips = ImportModelClips(project, modelRef);
    result.Clips = clips.Found;

    const bool had = registry.all_of<AnimationComponent>(entity);
    AnimationComponent& am = registry.get_or_emplace<AnimationComponent>(entity);
    result.AnimationAdded = !had;

    // Выбор человека не перебиваем: назначенный клип — это его работа.
    if (am.ClipPath.empty() && !clips.Refs.empty()) {
        const int index = PreferredClipIndex(model->Clips());
        if (index >= 0 && index < (int)clips.Refs.size()) am.ClipPath = clips.Refs[index];
    }
    result.Clip = am.ClipPath;

    LOG_INFO("Анимация") << "Модель со скелетом " << modelRef << ": костей " << result.Bones
                         << ", клипов " << result.Clips
                         << (result.AnimationAdded ? ", добавлен компонент Animation"
                                                   : ", компонент Animation уже был")
                         << (result.Clip.empty() ? std::string(" (клипов в файле нет — скелет "
                                                               "готов к позам из скрипта)")
                                                 : ", клип " + result.Clip);
    return result;
}

namespace {

ModelMaterialImportResult SetEntityMeshImpl(const Project& project, entt::registry& registry,
                                            entt::entity entity, MeshRendererComponent& mr,
                                            MeshRef::Type type, const std::string& path,
                                            std::shared_ptr<Mesh> mesh) {
    const bool sameModel = mr.Ref.type == type && mr.Ref.path == path;
    mr.Ref.type = type;
    mr.Ref.path = path;
    mr.MeshPtr = std::move(mesh);
    // Слоты — это материалы частей ЭТОЙ модели. Другая модель — другие части, и
    // старые слоты покрасили бы их наугад.
    if (!sameModel) mr.Slots.clear();

    if (type != MeshRef::Type::Model || path.empty()) return {};

    ModelMaterialImportResult result;
    // СКЕЛЕТ И КЛИПЫ — ДО проверки на загруженный меш, а не после. Инспектор
    // ставит модель с mesh == nullptr (грузит следующим шагом), и у персонажа,
    // назначенного слотом инспектора, статического меша не будет НИКОГДА: его
    // рисует скелетный проход. Стой эта настройка ниже, ровно у этого пути
    // персонаж так и оставался бы статуей.
    SetupModelAnimation(project, registry, entity, path, result);

    // Материалы — только когда меш УЖЕ загружен: по нему берётся разметка
    // частей. Модель без меша получит их вместе с загрузкой.
    if (!mr.MeshPtr) return result;

    const ModelMaterialImportResult materials = ImportModelMaterials(project, mr);
    result.Created = materials.Created;
    result.Assigned = materials.Assigned;
    result.AnyMaps = materials.AnyMaps;
    result.FirstWarning = materials.FirstWarning;
    result.FileMaterials = materials.FileMaterials;
    result.Parts = materials.Parts;
    return result;
}

} // namespace

ModelMaterialImportResult SetEntityMesh(const Project& project, entt::registry& registry,
                                        entt::entity entity, MeshRef::Type type,
                                        const std::string& path, std::shared_ptr<Mesh> mesh) {
    MeshRendererComponent& mr = registry.get_or_emplace<MeshRendererComponent>(entity);
    return SetEntityMeshImpl(project, registry, entity, mr, type, path, std::move(mesh));
}

// ============================================================================
//  Клипы анимации в отдельные файлы
// ============================================================================

ClipImportResult ImportModelClips(const Project& project, const std::string& modelRef) {
    ClipImportResult result;
    if (modelRef.empty()) return result;

    const std::string modelPath = sage::AssetDatabase::Instance().LocatePath(modelRef);
    std::error_code ec;
    if (!fs::exists(modelPath, ec)) {
        result.FirstWarning = "файл модели не найден: " + modelPath;
        LOG_WARN("Анимация") << result.FirstWarning;
        return result;
    }

    // Скелетная модель — через кэш: она же нужна и сцене, грузить второй раз
    // незачем.
    std::shared_ptr<sage::render::SkinnedModel> model =
        ResourceManager::Instance().GetSkinnedModel(modelRef);
    if (!model) {
        // Не ошибка импорта: у модели может не быть скелета вовсе (декорация).
        // Причину GetSkinnedModel уже написал в лог.
        LOG_INFO("Анимация") << "В модели нет скелета: " << modelRef;
        return result;
    }

    const std::vector<sage::anim::AnimationClip>& clips = model->Clips();
    result.Found = (int)clips.size();
    if (clips.empty()) {
        LOG_INFO("Анимация") << "В модели нет клипов: " << modelRef;
        return result;
    }

    const fs::path dir = fs::path(modelPath).parent_path();
    const std::string stem = fs::path(modelPath).stem().string();
    result.Refs.reserve(clips.size());

    for (const sage::anim::AnimationClip& clip : clips) {
        const fs::path file = dir / sage::anim::ClipFileName(stem, clip.Name);
        result.Refs.push_back(project.AssetRef(file));
        if (fs::exists(file, ec)) continue;     // правку руками не стираем
        try {
            sage::anim::SaveClip(sage::anim::ToAsset(clip, model->GetSkeleton()), file.string());
            sage::AssetDatabase::Instance().Register(file.string(), "animation");
            ++result.Created;
        } catch (const std::exception& e) {
            if (result.FirstWarning.empty()) result.FirstWarning = e.what();
            LOG_ERROR("Анимация") << "Клип не записан (" << clip.Name << "): " << e.what();
        }
    }

    LOG_INFO("Анимация") << "Клипы модели " << modelRef << ": в файле " << result.Found
                         << ", записано " << result.Created
                         << (result.Created == 0 && result.Found > 0 ? " (уже лежали рядом)" : "");
    return result;
}
