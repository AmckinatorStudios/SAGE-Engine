// ---------------------------------------------------------------------------
// EditorLayer — правка сцены: отмена, выделение, объекты.
//
// Отмена и повтор, выделение, создание и удаление объектов, префабы,
// выравнивание и подгонка камеры. Связывает их одно: каждое такое действие
// МЕНЯЕТ ДОКУМЕНТ и потому обязано попасть в историю отмены. Держать их рядом
// — единственный способ не забыть про снимок в новом действии.
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


void EditorLayer::PushUndoSnapshot() {
    if (InPlayMode()) return; // правки в Play эфемерны — Stop их и так откатит
    constexpr size_t kMaxUndoEntries = 100;
    if (m_undoStack.size() >= kMaxUndoEntries) {
        m_undoStack.erase(m_undoStack.begin());
    }
    m_undoStack.push_back(SceneSerializer::SaveToString(*m_scene));
    m_redoStack.clear(); // новая мутация обрывает redo-ветку
    m_sceneDirty = true;
    UpdateWindowTitle();
}

void EditorLayer::CapturePendingSnapshot() {
    if (InPlayMode()) return;
    m_pendingEditSnapshot = SceneSerializer::SaveToString(*m_scene);
}

void EditorLayer::CommitPendingSnapshot() {
    if (InPlayMode() || m_pendingEditSnapshot.empty()) return;
    constexpr size_t kMaxUndoEntries = 100;
    if (m_undoStack.size() >= kMaxUndoEntries) m_undoStack.erase(m_undoStack.begin());
    m_undoStack.push_back(m_pendingEditSnapshot);
    m_pendingEditSnapshot.clear();
    m_redoStack.clear();
    m_sceneDirty = true;
    UpdateWindowTitle();
}

// Одна запись undo на всё перетаскивание DragFloat/набор текста: состояние
// «до» запоминается на активации виджета, в стек уходит на завершении правки.
void EditorLayer::TrackLastImGuiItem() {
    if (InPlayMode()) return;
    if (ImGui::IsItemActivated()) CapturePendingSnapshot();
    if (ImGui::IsItemDeactivatedAfterEdit()) CommitPendingSnapshot();
}

void EditorLayer::Undo() {
    if (InPlayMode() || m_undoStack.empty()) return;
    m_redoStack.push_back(SceneSerializer::SaveToString(*m_scene));
    if (RestoreSceneFromString(m_undoStack.back())) {
        m_undoStack.pop_back();
        m_sceneDirty = true;
        UpdateWindowTitle();
    } else {
        m_redoStack.pop_back(); // откат не удался — не ломаем историю
    }
}

void EditorLayer::Redo() {
    if (InPlayMode() || m_redoStack.empty()) return;
    m_undoStack.push_back(SceneSerializer::SaveToString(*m_scene));
    if (RestoreSceneFromString(m_redoStack.back())) {
        m_redoStack.pop_back();
        m_sceneDirty = true;
        UpdateWindowTitle();
    } else {
        m_undoStack.pop_back();
    }
}

// ============================================================================
//  Сущности
// ============================================================================


// Готовый элемент интерфейса по имени пресета. Значения подобраны так, чтобы
// созданный элемент был СРАЗУ ВИДЕН и сразу делал то, что обещает названием:
// кнопка ловит мышь, полоса заполнена наполовину, поле ввода имеет подсказку.
// Ноль в размере или прозрачный цвет по умолчанию означали бы, что человек
// создал элемент и не увидел ничего.
GameObject EditorLayer::CreateUIEntity(const std::string& preset) {
    GameObject obj = m_scene->CreateObject(preset);
    entt::registry& reg = m_scene->Registry();

    // Новый элемент становится ДОЧЕРНИМ к выделенному элементу интерфейса.
    //
    // Интерфейс собирается из вложенных прямоугольников: панель, а в ней
    // надпись, кнопка и полоса. Раньше каждый созданный элемент вставал в
    // корень, то есть отсчитывался от края ЭКРАНА, и собрать панель означало
    // создать элементы, а потом перетащить каждый в иерархии на панель, помня,
    // что до этого они лежали не там. Самый частый шаг верстки требовал
    // отдельного ручного действия — и именно это ощущается как «неудобно
    // прикреплять».
    if (m_selectedId >= 0) {
        GameObject sel = m_scene->Get(m_selectedId);
        if (sel.Valid() && sage::ui::IsElement(reg, sel.Entity())) {
            m_scene->SetParent(obj.Entity(), sel.Entity());
        }
    }

    // Что именно значит «кнопка» или «полоса», знает ДВИЖОК (sage/ui/UIPresets.h).
    // Раньше это знание жило только здесь, и получить кнопку можно было лишь
    // мышью в редакторе: скрипт, собирающий интерфейс на лету, повторял те же
    // семь присваиваний у себя.
    sage::ui::ApplyPreset(reg, obj.Entity(), preset);
    // Новый элемент появляется в центре родителя: у края экрана его легко не
    // заметить и решить, что «ничего не создалось».
    sage::ui::Transform& xf = reg.get<sage::ui::Transform>(obj.Entity());
    xf.Anchor = UIAnchor::Center;
    xf.Offset = glm::vec2(0.0f, 0.0f);

    return obj;
}

GameObject EditorLayer::CreateCubeEntity(const std::string& name) {
    return CreatePrimitiveEntity(name, MeshRef::Type::Cube);
}

GameObject EditorLayer::CreatePrimitiveEntity(const std::string& name, MeshRef::Type type) {
    GameObject obj = m_scene->CreateObject(name);
    MeshRendererComponent& mr = obj.Renderer();
    mr.Ref = MeshRef{type, ""};
    mr.MeshPtr = ResourceManager::Instance().GetPrimitive(type);
    return obj;
}

namespace {
// Копирует компонент T с сущности src на copy, если он есть. Дубликат должен
// нести ВСЕ движковые компоненты — раньше копировались только Script/Camera, и
// дубликат света/физического тела/эмиттера молча терял суть оригинала.
template <typename T>
void CopyComponentIfPresent(GameObject& src, GameObject& copy) {
    if (const T* c = src.Registry()->try_get<T>(src.Entity())) {
        copy.Registry()->emplace_or_replace<T>(copy.Entity(), *c);
    }
}
} // namespace

namespace {
// Копирование сущностей и поддеревьев переехало в движок (sage/scene/Prefab.h):
// ровно то же самое нужно игре, а жило оно здесь, в безымянном пространстве
// имён редактора, и потому было недоступно никому, кроме него. Здесь остались
// короткие псевдонимы, чтобы не править два десятка мест вызова.
using sage::scene::CopyAllComponents;
using sage::scene::CopySubtree;
} // namespace

// Копирует одну сущность (без детей) со всеми компонентами; сдвиг, чтобы копия
// не сливалась с оригиналом. Возвращает копию.
GameObject EditorLayer::DuplicateEntity(GameObject src) {
    GameObject copy = m_scene->CreateObject(src.Name() + " Copy");
    CopyAllComponents(src, copy);
    copy.GetTransform().Position.x += 0.5f;
    return copy;
}

void EditorLayer::DuplicateSelected() {
    if (m_selection.empty()) return;
    PushUndoSnapshot();
    std::vector<int> copies;
    for (int id : m_selection) {
        GameObject src = m_scene->Get(id);
        if (!src.Valid()) continue;
        entt::entity parent = m_scene->ParentOf(src.Entity()); // копия остаётся у того же родителя
        GameObject copy = DuplicateEntity(src);
        if (parent != entt::null) m_scene->SetParent(copy.Entity(), parent);
        copies.push_back(copy.Id());
    }
    m_selection = copies;
    m_selectedId = copies.empty() ? -1 : copies.back();
}

namespace {
// Сколько сущностей под этой в иерархии. Нужно ровно для одного: сказать
// человеку в вопросе, СКОЛЬКО он на самом деле удаляет.
int CountDescendants(Scene& scene, entt::entity e) {
    const HierarchyComponent* h = scene.Registry().try_get<HierarchyComponent>(e);
    if (!h) return 0;
    int n = 0;
    for (entt::entity kid : h->Children) {
        if (!scene.Registry().valid(kid)) continue;
        n += 1 + CountDescendants(scene, kid);
    }
    return n;
}
} // namespace

void EditorLayer::DeleteSelected() {
    int count = 0;
    std::string firstName;
    for (int id : m_selection) {
        GameObject o = m_scene->Get(id);
        if (!o.Valid()) continue;
        if (count == 0) firstName = o.Name();
        ++count;
    }
    if (count == 0) return;

    // Спрашиваем — и считаем ПОДДЕРЕВО, а не только выделенное: удаление
    // родителя уносит детей, и человек, выделивший одну строку в иерархии,
    // сплошь и рядом не помнит, сколько под ней.
    int withChildren = 0;
    for (int id : m_selection) {
        GameObject o = m_scene->Get(id);
        if (!o.Valid()) continue;
        withChildren += 1 + CountDescendants(*m_scene, o.Entity());
    }

    std::string message;
    if (count == 1) {
        message = T("Delete \u00ab") + firstName + "»?";
        if (withChildren > 1)
            message += T("\nTogether with its children that is ") + std::to_string(withChildren) + T(" entities.");
    } else {
        message = T("Delete the selected objects (") + std::to_string(count) + ")?";
        if (withChildren > count)
            message += T("\nTogether with its children that is ") + std::to_string(withChildren) + T(" entities.");
    }
    message += T("\nCtrl+Z undoes this.");

    m_confirm.Ask("delete-entity", T("Deleting an object"), message, [this]() {
        PushUndoSnapshot();
        for (int id : m_selection)
            if (m_scene->Get(id).Valid()) m_scene->RemoveObject(id); // удаляет и поддерево
        SetSelectedId(-1);
        m_selection.clear();
    });
}

void EditorLayer::SetSelectedId(int id) {
    m_selectedId = id;
    m_selection.clear();
    if (id != -1) m_selection.push_back(id);
}

bool EditorLayer::IsSelected(int id) const {
    return std::find(m_selection.begin(), m_selection.end(), id) != m_selection.end();
}

void EditorLayer::ToggleSelection(int id) {
    if (id == -1) return;
    auto it = std::find(m_selection.begin(), m_selection.end(), id);
    if (it != m_selection.end()) {
        m_selection.erase(it);
        m_selectedId = m_selection.empty() ? -1 : m_selection.back();
    } else {
        m_selection.push_back(id);
        m_selectedId = id; // добавленная становится первичной
    }
}

// ============================================================================
//  Префабы — переиспользуемые сущности-поддеревья (.sageprefab). Формат —
//  та же JSON-сериализация, что у сцен: префаб = мини-сцена с одним корнем.
// ============================================================================
bool EditorLayer::SaveSelectedAsPrefab(const fs::path& path, std::string& err) {
    GameObject root = m_scene->Get(m_selectedId);
    if (!root.Valid()) { err = T("nothing selected"); return false; }
    if (!sage::scene::SavePrefab(*m_scene, root.Entity(), path.string(), err)) return false;
    SetStatusMessage(T("Prefab saved: ") + path.filename().string());
    return true;
}

int EditorLayer::InstantiatePrefab(const fs::path& path) {
    PushUndoSnapshot();
    const int rootId = sage::scene::InstantiatePrefab(*m_scene, path.string());
    if (rootId != -1) SetSelectedId(rootId);
    return rootId;
}

// ============================================================================
//  Инструменты над выделением
// ============================================================================

float EditorLayer::SnapStepForCurrentOp() {
    switch ((ImGuizmo::OPERATION)m_gizmoOp) {
        case ImGuizmo::ROTATE: return m_snapRotate;
        case ImGuizmo::SCALE:  return m_snapScale;
        default:               return m_snapMove;
    }
}

namespace {

// Мировой AABB одной сущности: восемь углов локальной коробки через мировую
// матрицу. Не «центр ± радиус»: при повороте коробка перестаёт быть выровненной
// по осям, и охватывающая её мировая коробка строится только по углам.
bool EntityWorldBounds(Scene& scene, entt::entity e, glm::vec3& lo, glm::vec3& hi) {
    const MeshRendererComponent* mr = scene.Registry().try_get<MeshRendererComponent>(e);
    if (!mr || !mr->MeshPtr) return false;
    const glm::vec3 bmin = mr->MeshPtr->BoundsMin();
    const glm::vec3 bmax = mr->MeshPtr->BoundsMax();
    const glm::mat4 world = scene.WorldMatrix(e);
    bool any = false;
    for (int c = 0; c < 8; ++c) {
        const glm::vec3 corner((c & 1) ? bmax.x : bmin.x, (c & 2) ? bmax.y : bmin.y,
                               (c & 4) ? bmax.z : bmin.z);
        const glm::vec3 w = glm::vec3(world * glm::vec4(corner, 1.0f));
        lo = any ? glm::min(lo, w) : w;
        hi = any ? glm::max(hi, w) : w;
        any = true;
    }
    return any;
}

} // namespace

bool EditorLayer::SelectionBounds(glm::vec3& outMin, glm::vec3& outMax) {
    bool any = false;
    for (int id : m_selection) {
        GameObject o = m_scene->Get(id);
        if (!o.Valid()) continue;
        glm::vec3 lo, hi;
        if (!EntityWorldBounds(*m_scene, o.Entity(), lo, hi)) continue;
        outMin = any ? glm::min(outMin, lo) : lo;
        outMax = any ? glm::max(outMax, hi) : hi;
        any = true;
    }
    return any;
}

void EditorLayer::FocusSelected() {
    glm::vec3 lo, hi;
    glm::vec3 target;
    float radius = 1.0f;
    if (SelectionBounds(lo, hi)) {
        target = (lo + hi) * 0.5f;
        radius = std::max(glm::length(hi - lo) * 0.5f, 0.1f);
    } else {
        // Выделено что-то без геометрии (свет, камера, пустышка) — подводим
        // камеру к его позиции: маркер всё равно нарисован, и добраться до него
        // человек хочет ровно так же.
        GameObject o = SelectedObject();
        if (!o.Valid()) return;
        target = glm::vec3(m_scene->WorldMatrix(o.Entity())[3]);
    }

    // Расстояние — из вертикального угла обзора, чтобы объект занял кадр
    // примерно на 70%: впритык он упирался бы в края, а «с запасом» съедало бы
    // смысл операции.
    const float fov = glm::radians(std::max(m_camera.Fov, 10.0f));
    const float dist = std::max(radius / std::tan(fov * 0.5f) / 0.7f, radius + 0.5f);
    m_camera.Position = target - m_camera.Front * dist;
}

void EditorLayer::DropSelectedToSurface() {
    if (m_selection.empty()) return;
    PushUndoSnapshot();

    int moved = 0;
    for (int id : m_selection) {
        GameObject o = m_scene->Get(id);
        if (!o.Valid()) continue;
        glm::vec3 lo, hi;
        if (!EntityWorldBounds(*m_scene, o.Entity(), lo, hi)) continue;

        // Луч вниз из центра НИЖНЕЙ грани: из центра объекта он сначала прошёл
        // бы сквозь него самого, а из угла — промахнулся бы мимо опоры.
        const glm::vec3 bottom((lo.x + hi.x) * 0.5f, lo.y, (lo.z + hi.z) * 0.5f);
        const glm::vec3 ro = bottom + glm::vec3(0.0f, 0.001f, 0.0f);
        const glm::vec3 rd(0.0f, -1.0f, 0.0f);

        float bestT = 1e30f;
        bool found = false;
        auto view = m_scene->Registry().view<IdComponent, MeshRendererComponent>();
        for (auto e : view) {
            // Себя и других выделенных пропускаем: они едут вместе с этим, и
            // опираться на них значило бы ставить объект сам на себя.
            if (IsSelected(view.get<IdComponent>(e).Id)) continue;
            Mesh* mesh = view.get<MeshRendererComponent>(e).MeshPtr.get();
            if (!mesh) continue;
            const glm::mat4 inv = glm::inverse(m_scene->WorldMatrix(e));
            const glm::vec3 lro = glm::vec3(inv * glm::vec4(ro, 1.0f));
            const glm::vec3 lrd = glm::vec3(inv * glm::vec4(rd, 0.0f));
            sage::render::RayHit hit = sage::render::RayMesh(*mesh, lro, lrd);
            if (hit.Hit && hit.Distance < bestT) {
                bestT = hit.Distance;
                found = true;
            }
        }
        if (!found) continue;

        // Двигаем на дельту в МИРЕ, а потом переводим в локальные координаты:
        // у сущности с родителем прибавление к Transform.Position означало бы
        // смещение в системе родителя, то есть не туда.
        const float dropBy = bestT;
        Transform& tr = o.GetTransform();
        const entt::entity parent = m_scene->ParentOf(o.Entity());
        if (parent == entt::null) {
            tr.Position.y -= dropBy;
        } else {
            glm::mat4 world = m_scene->WorldMatrix(o.Entity());
            world[3].y -= dropBy;
            const glm::mat4 local = glm::inverse(m_scene->WorldMatrix(parent)) * world;
            tr.Position = glm::vec3(local[3]);
        }
        ++moved;
    }
    SetStatusMessage(moved ? (T("Dropped onto the surface: ") + std::to_string(moved))
                           : T("There is no surface under the selection"));
}

void EditorLayer::AlignSelection(int axis) {
    if (m_selection.size() < 2 || axis < 0 || axis > 2) return;
    GameObject primary = SelectedObject();
    if (!primary.Valid()) return;
    PushUndoSnapshot();

    // Эталон — первичная сущность (та, вокруг которой стоит гизмо): выравнивать
    // «по среднему» бессмысленно, человек всегда равняет ПО ЧЕМУ-ТО.
    const float target = m_scene->WorldMatrix(primary.Entity())[3][axis];
    for (int id : m_selection) {
        if (id == primary.Id()) continue;
        GameObject o = m_scene->Get(id);
        if (!o.Valid()) continue;
        glm::mat4 world = m_scene->WorldMatrix(o.Entity());
        world[3][axis] = target;
        const entt::entity parent = m_scene->ParentOf(o.Entity());
        const glm::mat4 local =
            (parent == entt::null) ? world : glm::inverse(m_scene->WorldMatrix(parent)) * world;
        o.GetTransform().Position = glm::vec3(local[3]);
    }
    SetStatusMessage(std::string(T("Aligned along axis ")) + "XYZ"[axis]);
}

bool EditorLayer::HasPrimaryCamera() {
    auto view = m_scene->Registry().view<CameraComponent, Transform>();
    for (auto e : view) {
        if (view.get<CameraComponent>(e).Primary) return true;
    }
    return false;
}
