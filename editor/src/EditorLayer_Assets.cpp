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
constexpr float kToolbarHeight = 34.0f;

} // namespace


void EditorLayer::HandleDroppedFiles() {
    if (m_droppedFiles.empty()) return;
    std::vector<std::string> dropped;
    dropped.swap(m_droppedFiles);

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
            if (!m_project.Loaded()) {
                SetStatusMessage(T("Open a project first — there is nowhere to bring the file"));
                continue;
            }
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
            if (m_project.Loaded() && m_project.AssetRef(path) == path.generic_string())
                SetStatusMessage(T("This scene is not from this project — assets may not be found"));
            LoadSceneFromFile(path);
            continue;
        }

        if (!m_project.Loaded()) {
            SetStatusMessage(T("Open a project first — there is nowhere to bring the file"));
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
    if (!fs::exists(full, ec) && m_project.Loaded()) full = m_project.Dir() / path;
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
        MeshRendererComponent& mr = obj.Renderer();
        mr.MaterialPath = ref;
        mr.MaterialPtr = ResourceManager::Instance().GetMaterial(ref);
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
        MeshRendererComponent& mr = obj.Renderer();
        SetEntityMesh(mr, MeshRef::Type::Model, ref, std::move(mesh));
        SetStatusMessage(T("Mesh replaced: ") + asset.filename().string());
        return true;
    }
    // Префаб на сущность НЕ применяется: он сам себе поддерево, и «применить» его
    // к чужой сущности значило бы её заменить. Ставится он в сцену — броском во
    // вьюпорт или в список.
    SetStatusMessage(T("This file cannot be assigned to an object"));
    return false;
}

// Материал модели — сразу при её появлении в сцене.
//
// Раньше это делал ТОЛЬКО инспектор, когда модель назначали через его слот. Три
// других пути — перетаскивание в список сцены, во вьюпорт и на объект — материал
// не назначали вовсе, и модель с текстурами вставала белой болванкой. Один и тот
// же ассет выглядел по-разному в зависимости от того, каким жестом его принесли,
// и «текстуры не работают» было честным выводом из увиденного.
void EditorLayer::AssignModelMaterial(MeshRendererComponent& mr) {
    ImportModelMaterials(m_project, mr);
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
        MeshRendererComponent& mr = obj.Renderer();
        SetEntityMesh(mr, MeshRef::Type::Model, ref, std::move(mesh));
        AssignModelMaterial(mr);
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
    if (!isModel && !isPrefab && !isMaterial) return false;

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

    if (isMaterial) {
        // Материал ложится на то, НА ЧТО его уронили. В пустоту ронять его
        // бессмысленно — там нечего красить, и создавать ради этого объект было
        // бы сюрпризом.
        if (bestEntity == entt::null) {
            SetStatusMessage(T("Nowhere to drop the material — no object under the cursor"));
            return true;
        }
        PushUndoSnapshot();
        MeshRendererComponent& mr = m_scene->Registry().get<MeshRendererComponent>(bestEntity);
        mr.MaterialPath = ref;
        mr.MaterialPtr = ResourceManager::Instance().GetMaterial(ref);
        const int id = m_scene->Registry().get<IdComponent>(bestEntity).Id;
        SetSelectedId(id);
        m_selection = {id};
        SetStatusMessage(T("Material assigned: ") + asset.filename().string());
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
        MeshRendererComponent& mr = obj.Renderer();
        SetEntityMesh(mr, MeshRef::Type::Model, ref, ResourceManager::Instance().GetModel(ref));
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
