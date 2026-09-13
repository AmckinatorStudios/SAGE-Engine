// ---------------------------------------------------------------------------
// EditorLayer — ассеты и попадание мышью во вьюпорт.
//
// Перетащенный в окно файл, ассет, брошенный во вьюпорт, назначение материала
// и выбор объекта под курсором. Вместе они здесь не случайно: и то и другое —
// это ЛУЧ ИЗ ЭКРАНА В СЦЕНУ. Бросок ассета должен попасть туда же, куда попал
// бы щелчок, и разъехаться этим двум путям нельзя.
//
// Часть класса EditorLayer: объявления методов остались в EditorLayer.h, здесь
// только тела. Разбит он ровно потому, что дорос до двух с половиной тысяч
// строк, в которых рядом лежали сборка игры, отмена правки и раскладка окон —
// три области, у которых нет ничего общего, кроме имени класса.
// ---------------------------------------------------------------------------
#include "EditorLayer.h"
#include "sage/assets/Pack.h"

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <fstream>

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/euler_angles.hpp>

#include "imgui.h"
#include "imgui_internal.h" // DockBuilder API (создание раскладки по умолчанию)
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "ImGuizmo.h"

#include "EditorTheme.h"
#include "EditorIcons.h"
#include "ModelMaterialImport.h"
#include "AssetSlot.h"
#include "TextureSet.h"
#include "sage/render/DebugView.h"
#include "sage/core/Application.h"
#include "sage/core/Paths.h"
#include "sage/render/ModelMaterial.h"
#include "sage/assets/AssetDatabase.h"
#include "sage/core/Systems.h"
#include "sage/core/Version.h"
#include "sage/core/CrashHandler.h"
#include "sage/render/MeshRaycast.h"
#include "sage/render/ResourceManager.h"
#include "sage/render/Screenshot.h"
#include "sage/render/LightingUpload.h"
#include "sage/anim/AnimationSystem.h"
#include "sage/ecs/LightSystem.h"
#include "sage/ecs/RenderSystem.h"
#include "sage/render/ParticlePresets.h"
#include "sage/gi/GI.h"
#include "sage/scene/Components.h"
#include "sage/ui/UI.h"
#include "sage/ui/UIDemos.h"
#include "sage/ui/UIPresets.h"
#include "sage/ui/UISceneSystem.h"
#include "sage/scene/Prefab.h"
#include "sage/scene/SceneSerializer.h"
#include "AssetExt.h"
#include "Localization.h"

namespace fs = std::filesystem;

namespace {

// Пересечение луча с произвольным AABB [bmin, bmax] в локальном пространстве
// объекта (slab-тест). Возвращает t входа (>=0) или отрицательное при промахе.
float RayBox(const glm::vec3& ro, const glm::vec3& rd, const glm::vec3& bmin, const glm::vec3& bmax) {
    glm::vec3 inv = 1.0f / rd; // IEEE inf при нулевой компоненте — slab-тест это переживает
    glm::vec3 t0 = (bmin - ro) * inv;
    glm::vec3 t1 = (bmax - ro) * inv;
    glm::vec3 tmin = glm::min(t0, t1), tmax = glm::max(t0, t1);
    float tNear = std::max({tmin.x, tmin.y, tmin.z});
    float tFar  = std::min({tmax.x, tmax.y, tmax.z});
    if (tNear > tFar || tFar < 0.0f) return -1.0f;
    return tNear >= 0.0f ? tNear : tFar;
}

// Луч vs единичный куб [-0.5,0.5]^3 — маркеры невидимых сущностей (камера/свет).
float RayUnitCube(const glm::vec3& ro, const glm::vec3& rd) {
    return RayBox(ro, rd, glm::vec3(-0.5f), glm::vec3(0.5f));
}

constexpr float kStatusBarHeight = 26.0f;

} // namespace


void EditorLayer::HandleDroppedFiles() {
    if (m_droppedFiles.empty()) return;
    std::vector<std::string> dropped;
    dropped.swap(m_droppedFiles);

    // ПОКА НА ЭКРАНЕ СТАРТОВОЕ ОКНО, брошенный файл принадлежит ему: там
    // перетаскивание означает «добавь этот проект в список», а не «внеси файл
    // в ассеты». Без этой ветки папка, брошенная на стартовое окно, молча
    // копировалась бы в ассеты того проекта, который откроют следующим.
    if (!m_project.Loaded() || m_launcherRequested) {
        m_launcher.AcceptDroppedFiles(dropped, m_projects);
        return;
    }

    int imported = 0;
    std::string lastError;
    for (const std::string& raw : dropped) {
        const fs::path path(raw);
        std::error_code ec;

        // Папка вносится ЦЕЛИКОМ, вместе с содержимым.
        //
        // Раньше редактор отвечал на неё отказом «папку перетащить нельзя —
        // бросьте файлы». Со стороны это выглядело как «ничего не происходит»:
        // строку состояния внизу окна при перетаскивании никто не читает. А
        // именно папкой приходит любой скачанный набор — модель со своими
        // текстурами, набор интерфейса, тайлсет. Раскладывать его по файлам
        // вручную, чтобы движок согласился их принять, — работа .
        if (fs::is_directory(path, ec)) {
            const fs::path dest = m_assetsCwd / path.filename();
            std::error_code copyEc;
            fs::copy(path, dest, fs::copy_options::recursive, copyEc);
            if (copyEc) {
                lastError = copyEc.message();
                LOG_ERROR("Editor") << "Перетаскивание папки: " << copyEc.message();
                continue;
            }
            // База ассетов должна узнать о новых файлах сразу: иначе ссылки на
            // них считаются битыми до следующего открытия проекта.
            sage::AssetDatabase::Instance().ScanProject(m_project.Dir().string());
            ++imported;
            m_assets.Select(dest);
            LOG_INFO("Editor") << "Внесена папка: " << dest.string();
            continue;
        }

        const std::string ext = sage::editor::ToLowerExt(path);
        if (ext == ".sageproj") {
            std::string err;
            if (!OpenProject(path.string(), err)) SetStatusMessage(T("Failed to open the project: ") + err);
            continue;
        }
        if (ext == ".sage") {
            // Сцена ОТКРЫВАЕТСЯ. Если она из другого проекта, ссылки внутри неё
            // указывают в тот проект, и об этом честнее сказать сразу.
            if (m_project.AssetRef(path) == path.generic_string())
                SetStatusMessage(T("This scene is not from this project — assets may not be found"));
            LoadSceneFromFile(path);
            continue;
        }

        const AssetsPanel::ImportReport rep = AssetsPanel::ImportAsset(path, m_assetsCwd);
        if (!rep.Ok) {
            lastError = rep.Error;
            continue;
        }
        ++imported;
        m_assets.Select(rep.Created);
        for (const std::string& missing : rep.Missing)
            LOG_WARN("Editor") << "Перетаскивание: спутник не найден — " << missing;
    }

    if (imported == 1) SetStatusMessage(T("Brought into the project: ") + m_assets.Selected().filename().string());
    else if (imported > 1) SetStatusMessage(T("Files brought into the project: ") + std::to_string(imported));
    else if (!lastError.empty()) SetStatusMessage(T("Import failed: ") + lastError);
}

// «Показать в Assets»: перейти в папку файла, выделить его и открыть панель.
//
// Путь в компоненте относительный (см. Project::AssetRef), а текущая папка
// процесса — не обязательно корень проекта, поэтому сначала превращаем ссылку в
// настоящий путь. Панель открываем принудительно: команда «покажи, где лежит»,
// после которой ничего не появилось (панель была закрыта), выглядит как
// поломка.
void EditorLayer::ShowAssetInPanel(const fs::path& path) {
    if (path.empty()) return;
    fs::path full = path;
    std::error_code ec;
    if (!fs::exists(full, ec)) full = m_project.Dir() / path;
    if (!fs::exists(full, ec)) {
        SetStatusMessage(T("File not found: ") + path.string());
        return;
    }
    m_showAssets = true;
    m_assetsCwd = full.parent_path();
    m_assets.Select(full);
}

// ============================================================================
//  Пикинг из вьюпорта
// ============================================================================

void EditorLayer::PickAtViewport(float u, float v, bool additive) {
    PickAtViewportWith(m_view, m_proj, u, v, additive);
}

bool EditorLayer::ApplyAssetToEntity(int entityId, const fs::path& asset) {
    GameObject obj = m_scene->Get(entityId);
    if (!obj.Valid()) return false;

    const std::string ext = sage::editor::ToLowerExt(asset);
    const std::string ref = m_project.AssetRef(asset);

    if (ext == ".sagemat") {
        PushUndoSnapshot();
        // AssignMaterial, а не три присваивания: он же возвращает в нейтраль
        // поправки экземпляра. Красный материал на зелёном кубе иначе даёт
        // бурое пятно — цвет объекта множится на albedo материала.
        // get_or_emplace, а не Renderer(): бросить материал можно и на ПУСТОЙ
        // объект — у него меша нет. Бросок означает «пусть выглядит так», и
        // отказывать на этом месте было бы странно; компонент заводится сам.
        AssignMaterial(m_scene->Registry().get_or_emplace<MeshRendererComponent>(obj.Entity()),
                       ref, ResourceManager::Instance().GetMaterial(ref));
        SetStatusMessage(T("Material assigned: ") + asset.filename().string());
        return true;
    }
    if (ext == ".lua") {
        PushUndoSnapshot();
        m_scene->Registry().emplace_or_replace<ScriptComponent>(obj.Entity(), ScriptComponent{ref});
        SetStatusMessage(T("Script assigned: ") + asset.filename().string());
        return true;
    }
    if (ModelLoader::IsSupportedModel(ext)) {
        std::shared_ptr<Mesh> mesh = ResourceManager::Instance().GetModel(ref);
        if (!mesh) {
            SetStatusMessage(T("The model failed to load: ") + asset.filename().string() +
                             T(" — details in Console"));
            return true;
        }
        PushUndoSnapshot();
        // Компонент обязан быть до настройки модели — и у пустого объекта его
        // нет, поэтому заводим, а не читаем.
        m_scene->Registry().get_or_emplace<MeshRendererComponent>(obj.Entity());
        ReportModelMaterials(SetEntityMesh(m_project, m_scene->Registry(), obj.Entity(),
                                           MeshRef::Type::Model, ref, std::move(mesh)));
        SetStatusMessage(T("Mesh replaced: ") + asset.filename().string());
        return true;
    }
    // КАРТИНКА — ЭТО НАБОР КАРТ, А НЕ ОДНА ТЕКСТУРА.
    //
    // Текстуры скачивают наборами: рядом лежат albedo, normal, roughness,
    // metallic, ao. Раньше бросок картинки на объект отвечал «этот файл нельзя
    // назначить объекту», и единственным путём было создать материал руками и
    // разложить пять карт по слотам, зная, какой файл в какой слот. Понятно, до
    // какого слота доходили: до albedo. Отсюда и вывод, который и звучал, —
    // «работает только albedo, остальные карты игнорируются».
    if (assetslot::Accepts(assetslot::Kind::Texture, asset)) {
        PushUndoSnapshot();
        std::string status;
        const std::string matRef = MaterialFromTextureSet(asset, status);
        if (matRef.empty()) {
            SetStatusMessage(status);
            return true;
        }
        AssignMaterial(m_scene->Registry().get_or_emplace<MeshRendererComponent>(obj.Entity()),
                       matRef, ResourceManager::Instance().GetMaterial(matRef));
        SetStatusMessage(status);
        return true;
    }

    // Префаб на сущность НЕ применяется: он сам себе поддерево, и «применить» его
    // к чужой сущности значило бы её заменить. Ставится он в сцену — броском во
    // вьюпорт или в список.
    SetStatusMessage(T("This file cannot be assigned to an object"));
    return false;
}

// Материал из набора карт, лежащих рядом с этой картинкой. Возвращает ссылку
// проекта на .sagemat (пусто — не получилось), status — строка для человека.
std::string EditorLayer::MaterialFromTextureSet(const fs::path& texture, std::string& status) {
    const sage::editor::TextureSet set = sage::editor::FindTextureSet(texture);

    Material material;
    material.TexturePath = m_project.AssetRef(set.Albedo);
    material.NormalMapPath = m_project.AssetRef(set.Normal);
    material.MetallicMapPath = m_project.AssetRef(set.Metallic);
    material.RoughnessMapPath = m_project.AssetRef(set.Roughness);
    material.AOMapPath = m_project.AssetRef(set.AO);
    material.EmissiveMap = m_project.AssetRef(set.Emissive);
    // Карта задана — значит вид определяет она, а множитель её лишь домножает.
    // Фактор 0 при наличии карты обнулил бы её целиком, и «металл приехал без
    // металличности» выглядело бы как потеря текстуры (та же логика, что при
    // импорте материалов модели).
    if (!material.MetallicMapPath.empty()) material.Metallic = 1.0f;
    if (!material.RoughnessMapPath.empty()) material.Roughness = 1.0f;
    if (!material.EmissiveMap.empty()) material.Emissive = glm::vec3(1.0f);

    // Файл материала — РЯДОМ С КАРТАМИ и по общему корню имени: набор
    // «brick-wall_*» даёт «brick-wall.sagemat», и найти его потом можно там же,
    // где лежат сами карты.
    fs::path file = texture.parent_path() / (set.Base + ".sagemat");
    std::error_code ec;
    if (fs::exists(file, ec)) {
        // Уже есть — не перезаписываем: в нём могли поправить цвет, зеркальность
        // или подставить свою карту, и бросок текстуры не повод стирать эту
        // работу.
        const std::string ref = m_project.AssetRef(file);
        status = T("Material from the set: ") + file.filename().string() + T(" (already existed)");
        return ref;
    }
    try {
        material.SaveToFile(file.string());
    } catch (const std::exception& e) {
        LOG_ERROR("Материалы") << "Материал из набора не записан: " << e.what();
        status = T("Material from the set NOT written — details in Console");
        return {};
    }
    sage::AssetDatabase::Instance().Register(file.string(), "material");

    LOG_INFO("Материалы") << "Материал из набора " << set.Base << ": карт " << set.Found()
                          << (set.Normal.empty() ? "" : ", нормаль")
                          << (set.Metallic.empty() ? "" : ", металличность")
                          << (set.Roughness.empty() ? "" : ", шероховатость")
                          << (set.AO.empty() ? "" : ", затенение")
                          << (set.Emissive.empty() ? "" : ", свечение");
    if (set.NormalIsDirectX) {
        LOG_WARN("Материалы") << "Карта нормалей в формате DirectX (зелёный канал перевёрнут) — "
                              << "движок ждёт OpenGL; рельеф может читаться наизнанку";
    }
    for (const std::string& skipped : set.Unused) {
        LOG_INFO("Материалы") << "Карта из набора не применяется движком: " << skipped;
    }

    status = T("Material from the set: ") + std::to_string(set.Found()) + T(" maps");
    return m_project.AssetRef(file);
}

// Материал модели — сразу при её появлении в сцене.
//
// Раньше это делал ТОЛЬКО инспектор, когда модель назначали через его слот. Три
// других пути — перетаскивание в список сцены, во вьюпорт и на объект — материал
// не назначали вовсе, и модель с текстурами вставала белой болванкой. Один и тот
// же ассет выглядел по-разному в зависимости от того, каким жестом его принесли,
// и «текстуры не работают» было честным выводом из увиденного.
void EditorLayer::ReportModelMaterials(const ModelMaterialImportResult& r) {
    // РЕЗУЛЬТАТ НЕ ВЫБРАСЫВАЕМ. Раньше он именно выбрасывался, и модель,
    // приехавшая белой, не оставляла после себя ни строки состояния, ни записи
    // в логе — «не работает» приходилось проверять глазами по кадру.
    if (r.Assigned > 0) {
        SetStatusMessage(T("Model materials assigned: ") + std::to_string(r.Assigned));
    } else if (r.Parts > 0 && r.FileMaterials == 0) {
        SetStatusMessage(T("The model carries no materials — it stays white"));
    } else if (r.Parts > 0) {
        SetStatusMessage(T("Model materials NOT assigned — details in Console"));
    }
}

bool EditorLayer::AddAssetToScene(const fs::path& asset) {
    const std::string ext = sage::editor::ToLowerExt(asset);
    const std::string ref = m_project.AssetRef(asset);

    int newId = -1;
    if (ext == ".sageprefab") {
        PushUndoSnapshot();
        newId = sage::scene::InstantiatePrefab(*m_scene, ref);
        if (newId < 0) {
            SetStatusMessage(T("The prefab could not be placed: ") + asset.filename().string());
            return true;
        }
    } else if (ModelLoader::IsSupportedModel(ext)) {
        std::shared_ptr<Mesh> mesh = ResourceManager::Instance().GetModel(ref);
        if (!mesh) {
            SetStatusMessage(T("The model failed to load: ") + asset.filename().string() +
                             T(" — details in Console"));
            return true;
        }
        PushUndoSnapshot();
        GameObject obj = m_scene->CreateObject(asset.stem().string());
        ReportModelMaterials(SetEntityMesh(m_project, m_scene->Registry(), obj.Entity(),
                                           MeshRef::Type::Model, ref, std::move(mesh)));
        newId = obj.Id();
    } else {
        return false;
    }

    SetSelectedId(newId);
    m_selection = {newId};
    m_sceneDirty = true;
    UpdateWindowTitle();
    SetStatusMessage(T("Added to the scene: ") + asset.filename().string());
    return true;
}

bool EditorLayer::DropAssetAtViewport(const glm::mat4& view, const glm::mat4& proj, float u,
                                      float v, const fs::path& asset) {
    const std::string ext = sage::editor::ToLowerExt(asset);
    const std::string ref = m_project.AssetRef(asset);

    const bool isModel = ModelLoader::IsSupportedModel(ext);
    const bool isPrefab = ext == ".sageprefab";
    const bool isMaterial = ext == ".sagemat";
    // Текстура — тот же жест, что и материал: её роняют НА ПОВЕРХНОСТЬ, которую
    // хотят покрасить. Разница только в том, что материала ещё нет — он
    // собирается из набора карт, лежащих рядом (см. MaterialFromTextureSet).
    const bool isTexture = assetslot::Accepts(assetslot::Kind::Texture, asset);
    if (!isModel && !isPrefab && !isMaterial && !isTexture) return false;

    // Луч через точку, где отпустили кнопку. Та же математика, что у выбора
    // мышью (см. PickAtViewportWith), и это важно: место, куда встанет объект,
    // обязано совпадать с тем, по чему бы кликнули.
    const glm::vec2 ndc(u * 2.0f - 1.0f, 1.0f - v * 2.0f);
    const glm::mat4 invVP = glm::inverse(proj * view);
    glm::vec4 p0 = invVP * glm::vec4(ndc, -1.0f, 1.0f);
    glm::vec4 p1 = invVP * glm::vec4(ndc, 1.0f, 1.0f);
    const glm::vec3 ro = glm::vec3(p0) / p0.w;
    const glm::vec3 rd = glm::normalize(glm::vec3(p1) / p1.w - ro);

    // Ближайшая поверхность под курсором: и точка постановки, и объект, которому
    // достанется материал.
    float bestT = 1e30f;
    entt::entity bestEntity = entt::null;
    auto meshes = m_scene->Registry().view<IdComponent, MeshRendererComponent>();
    for (auto e : meshes) {
        Mesh* mesh = meshes.get<MeshRendererComponent>(e).MeshPtr.get();
        if (!mesh) continue;
        const glm::mat4 inv = glm::inverse(m_scene->WorldMatrix(e));
        const glm::vec3 lro = glm::vec3(inv * glm::vec4(ro, 1.0f));
        const glm::vec3 lrd = glm::vec3(inv * glm::vec4(rd, 0.0f));
        const sage::render::RayHit hit = sage::render::RayMesh(*mesh, lro, lrd);
        if (hit.Hit && hit.Distance < bestT) {
            bestT = hit.Distance;
            bestEntity = e;
        }
    }

    if (isMaterial || isTexture) {
        // Материал ложится на то, НА ЧТО его уронили. В пустоту ронять его
        // бессмысленно — там нечего красить, и создавать ради этого объект было
        // бы сюрпризом.
        if (bestEntity == entt::null) {
            SetStatusMessage(T("Nowhere to drop the material — no object under the cursor"));
            return true;
        }
        PushUndoSnapshot();
        std::string status = T("Material assigned: ") + asset.filename().string();
        std::string useRef = ref;
        if (isTexture) {
            useRef = MaterialFromTextureSet(asset, status);
            if (useRef.empty()) {
                SetStatusMessage(status);
                return true;
            }
        }
        MeshRendererComponent& mr = m_scene->Registry().get<MeshRendererComponent>(bestEntity);
        AssignMaterial(mr, useRef, ResourceManager::Instance().GetMaterial(useRef));
        const int id = m_scene->Registry().get<IdComponent>(bestEntity).Id;
        SetSelectedId(id);
        m_selection = {id};
        SetStatusMessage(status);
        return true;
    }

    // Точка постановки: поверхность под курсором, а если её нет — точка на луче
    // в паре метров от камеры. Ронять в бесконечность нельзя, а «в начало
    // координат» означало бы, что объект исчез из виду.
    const bool onSurface = bestEntity != entt::null;
    const glm::vec3 point = onSurface ? (ro + rd * bestT) : (ro + rd * 8.0f);

    PushUndoSnapshot();
    int newId = -1;
    if (isPrefab) {
        newId = sage::scene::InstantiatePrefabAt(*m_scene, ref, point);
        if (newId < 0) {
            SetStatusMessage(T("The prefab could not be placed: ") + asset.filename().string());
            return true;
        }
    } else {
        GameObject obj = m_scene->CreateObject(asset.stem().string());
        ReportModelMaterials(SetEntityMesh(m_project, m_scene->Registry(), obj.Entity(),
                                           MeshRef::Type::Model, ref,
                                           ResourceManager::Instance().GetModel(ref)));
        // Ссылка берётся ПОСЛЕ вызова: он мог завести сущности компоненты
        // (Animation у модели со скелетом), а всякая вставка в ECS вправе
        // переселить хранилище — и ссылка, взятая раньше, стала бы чужой.
        MeshRendererComponent& mr = obj.Renderer();
        if (!mr.MeshPtr) {
            m_scene->RemoveObject(obj.Id());
            SetStatusMessage(T("The model failed to load: ") + asset.filename().string() +
                             T(" — details in Console"));
            return true;
        }
        obj.GetTransform().Position = point;
        newId = obj.Id();

        // Ставим НА поверхность, а не центром в точку попадания: иначе половина
        // модели уходит под пол, и первое, что приходится делать после
        // перетаскивания, — поднимать её вручную.
        if (onSurface) {
            const glm::vec3 bmin = mr.MeshPtr->BoundsMin();
            obj.GetTransform().Position.y -= bmin.y * obj.GetTransform().Scale.y;
        }
    }

    SetSelectedId(newId);
    m_selection = {newId};
    m_sceneDirty = true;
    UpdateWindowTitle();
    SetStatusMessage(T("Placed in the scene: ") + asset.filename().string());
    return true;
}

// --- Рамка выделения во вьюпорте -------------------------------------------
//
// ПО ЭКРАННОЙ КОРОБКЕ, А НЕ ПО ЛУЧУ. Обводя рамкой, человек смотрит на
// картинку: «всё, что я обвёл». Проверять лучом каждый пиксель рамки — это
// сотни тысяч трассировок на один жест, и результат всё равно был бы другим:
// объект, видимый краем за чужой спиной, в рамку попадает, а в лучи — нет.
// Поэтому каждый объект проецируется в экран целиком (восемь углов его
// коробки) и сравнивается своим прямоугольником с прямоугольником рамки.
void EditorLayer::SelectInViewportRect(const glm::mat4& view, const glm::mat4& proj, float u0,
                                       float v0, float u1, float v1, bool additive) {
    const glm::vec2 rectMin(std::min(u0, u1), std::min(v0, v1));
    const glm::vec2 rectMax(std::max(u0, u1), std::max(v0, v1));
    const glm::mat4 vp = proj * view;

    // Экранный прямоугольник набора точек. Возвращает false, если объект
    // целиком ЗА камерой: точка с w <= 0 проецируется зеркально, и без этой
    // проверки предметы за спиной попадали бы в рамку перед лицом.
    auto screenBox = [&](const std::vector<glm::vec3>& pts, glm::vec2& mn, glm::vec2& mx) {
        bool any = false;
        for (const glm::vec3& p : pts) {
            const glm::vec4 clip = vp * glm::vec4(p, 1.0f);
            if (clip.w <= 1e-6f) continue;
            const glm::vec2 uv((clip.x / clip.w) * 0.5f + 0.5f, 0.5f - (clip.y / clip.w) * 0.5f);
            if (!any) { mn = mx = uv; any = true; continue; }
            mn = glm::min(mn, uv);
            mx = glm::max(mx, uv);
        }
        return any;
    };
    auto overlaps = [&](const glm::vec2& mn, const glm::vec2& mx) {
        return !(mx.x < rectMin.x || mn.x > rectMax.x || mx.y < rectMin.y || mn.y > rectMax.y);
    };

    std::vector<int> picked;
    auto meshes = m_scene->Registry().view<IdComponent, MeshRendererComponent>();
    for (auto e : meshes) {
        Mesh* mesh = meshes.get<MeshRendererComponent>(e).MeshPtr.get();
        if (!mesh) continue;
        const glm::mat4 world = m_scene->WorldMatrix(e);
        const glm::vec3 lo = mesh->BoundsMin();
        const glm::vec3 hi = mesh->BoundsMax();
        std::vector<glm::vec3> corners;
        corners.reserve(8);
        for (int i = 0; i < 8; ++i) {
            const glm::vec3 local((i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y,
                                  (i & 4) ? hi.z : lo.z);
            corners.push_back(glm::vec3(world * glm::vec4(local, 1.0f)));
        }
        glm::vec2 mn, mx;
        if (screenBox(corners, mn, mx) && overlaps(mn, mx))
            picked.push_back(meshes.get<IdComponent>(e).Id);
    }

    // Свет и камера — теми же маркерами, что и по клику: меша у них нет, но на
    // экране они нарисованы, и не попасть в рамку, которая их обводит, было бы
    // странно.
    auto marker = [&](entt::entity e, int id) {
        const glm::vec3 pos(m_scene->WorldMatrix(e)[3]);
        glm::vec2 mn, mx;
        if (screenBox({pos}, mn, mx) && overlaps(mn, mx)) picked.push_back(id);
    };
    auto camMarkers = m_scene->Registry().view<CameraComponent, Transform, IdComponent>();
    for (auto e : camMarkers) marker(e, camMarkers.get<IdComponent>(e).Id);
    auto lightMarkers = m_scene->Registry().view<LightComponent, Transform, IdComponent>();
    for (auto e : lightMarkers) marker(e, lightMarkers.get<IdComponent>(e).Id);

    SetSelection(picked, additive);
}

void EditorLayer::PickAtViewportWith(const glm::mat4& view, const glm::mat4& proj, float u, float v,
                                     bool additive) {
    // Луч из камеры через пиксель вьюпорта: unprojection ближней/дальней точек NDC.
    // Для ортогональной проекции это работает ровно так же: обе точки уходят в
    // одну сторону, просто луч получается параллельным, а не расходящимся.
    glm::vec2 ndc(u * 2.0f - 1.0f, 1.0f - v * 2.0f);
    glm::mat4 invVP = glm::inverse(proj * view);
    glm::vec4 p0 = invVP * glm::vec4(ndc, -1.0f, 1.0f);
    glm::vec4 p1 = invVP * glm::vec4(ndc, 1.0f, 1.0f);
    glm::vec3 ro = glm::vec3(p0) / p0.w;
    glm::vec3 rd = glm::normalize(glm::vec3(p1) / p1.w - ro);

    int bestId = -1;
    float bestDist = 1e30f;
    bool bestExact = false;
    auto meshes = m_scene->Registry().view<IdComponent, MeshRendererComponent>();
    for (auto e : meshes) {
        Mesh* mesh = meshes.get<MeshRendererComponent>(e).MeshPtr.get();
        if (!mesh) continue;
        // МИРОВАЯ матрица (учёт иерархии родителей): раньше бралась локальная —
        // дочерние сущности выделялись по неверной позиции.
        glm::mat4 inv = glm::inverse(m_scene->WorldMatrix(e));
        glm::vec3 lro = glm::vec3(inv * glm::vec4(ro, 1.0f));
        glm::vec3 lrd = glm::vec3(inv * glm::vec4(rd, 0.0f)); // без нормализации: t остаётся в масштабе мира

        sage::render::RayHit hit = sage::render::RayMesh(*mesh, lro, lrd);
        if (!hit.Hit) continue;

        // Точное попадание (по треугольнику) бьёт приблизительное (по коробке)
        // ДАЖЕ ЕСЛИ ОНО ДАЛЬШЕ. Иначе объект без копии геометрии перехватывал бы
        // выбор у соседа просто потому, что его коробка начинается раньше, —
        // а именно так пол и перекрывал всё, что на нём стоит.
        const bool better = (hit.Exact && !bestExact) ||
                            (hit.Exact == bestExact && hit.Distance < bestDist);
        if (!better) continue;
        bestDist = hit.Distance;
        bestExact = hit.Exact;
        bestId = meshes.get<IdComponent>(e).Id;
    }

    // Невидимые сущности (камера/свет) кликабельны по маленькому боксу вокруг
    // их позиции — иначе их гизмо не выбрать (меша нет).
    auto pickMarker = [&](entt::entity e, int id, const glm::vec3& pos) {
        glm::mat4 boxInv = glm::inverse(glm::translate(glm::mat4(1.0f), pos) *
                                        glm::scale(glm::mat4(1.0f), glm::vec3(0.6f)));
        glm::vec3 lro = glm::vec3(boxInv * glm::vec4(ro, 1.0f));
        glm::vec3 lrd = glm::vec3(boxInv * glm::vec4(rd, 0.0f));
        float t = RayUnitCube(lro, lrd);
        // Маркер считается ТОЧНЫМ попаданием: у света и камеры нет геометрии,
        // этот кубик и есть их единственное видимое тело, и промахнуться по
        // нему нельзя — он ровно там, где нарисован.
        if (t < 0.0f) return;
        const bool better = !bestExact || t < bestDist;
        if (!better) return;
        bestDist = t;
        bestExact = true;
        bestId = id;
    };
    auto camMarkers = m_scene->Registry().view<CameraComponent, Transform, IdComponent>();
    for (auto e : camMarkers)
        pickMarker(e, camMarkers.get<IdComponent>(e).Id, glm::vec3(m_scene->WorldMatrix(e)[3]));
    auto lightMarkers = m_scene->Registry().view<LightComponent, Transform, IdComponent>();
    for (auto e : lightMarkers)
        pickMarker(e, lightMarkers.get<IdComponent>(e).Id, glm::vec3(m_scene->WorldMatrix(e)[3]));

    // Ctrl-клик (additive): добавить/убрать попадание из набора (клик по пустоте
    // ничего не меняет). Обычный клик: одиночный выбор (мимо всех — снять).
    if (additive) {
        if (bestId != -1) ToggleSelection(bestId);
    } else {
        SetSelectedId(bestId);
    }
}
