// Гизмо объекта: то, что делается после ImGuizmo::Manipulate. Вынесено из
// ViewportPanel.cpp — тот файл отвечает за кадр вьюпорта целиком, а это
// одна операция: «гизмо сдвинулось — перенести это в Transform».
#include "ViewportPanel.h"

#include <glm/gtc/matrix_transform.hpp>

#include "EditorHost.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "sage/scene/ScaleLock.h"
#include "sage/render/Camera.h"

void ViewportPanel::ResetCamera(Camera& camera) {
    camera.Position = {6.5f, 5.0f, 6.5f};
    camera.SetAngles(-135.0f, -28.0f);
}

namespace {
// Мировая -> локальная: убираем вклад родителя.
glm::mat4 ToLocal(Scene& scene, entt::entity e, const glm::mat4& world) {
    const entt::entity p = scene.ParentOf(e);
    return p != entt::null ? glm::inverse(scene.WorldMatrix(p)) * world : world;
}
} // namespace

// Гизмо стоит В ОПОРЕ (матрица объекта, сдвинутая на Pivot): поворот и
// масштаб ImGuizmo делает вокруг начала своей матрицы, и только так они идут
// вокруг опоры. Матрица объекта получается обратно сдвигом на −Pivot.
glm::mat4 ViewportPanel::GizmoFrame(const glm::mat4& objectWorld, const Transform& tr) {
    return objectWorld * glm::translate(glm::mat4(1.0f), tr.Pivot);
}

void ViewportPanel::ApplyGizmo(EditorHost& host, GameObject selected, const glm::mat4& gizmo, bool pivotEdit) {
    Scene& scene = host.CurrentScene();
    Transform& tr = selected.GetTransform();
    const glm::mat4 objectWorld = scene.WorldMatrix(selected.Entity());

    if (pivotEdit) {
        // Правка опоры: гизмо тянут, объект стоит. Новая опора — точка гизмо в
        // координатах объекта; SetPivotKeepPlace держит мировую матрицу.
        const glm::vec3 local = glm::vec3(glm::inverse(objectWorld) * glm::vec4(glm::vec3(gizmo[3]), 1.0f));
        tr.SetPivotKeepPlace(local);
        return;
    }

    const glm::mat4 world = gizmo * glm::translate(glm::mat4(1.0f), -tr.Pivot);
    const glm::vec3 scaleBefore = tr.Scale;
    tr.SetFromMatrix(ToLocal(scene, selected.Entity(), world));
    tr.Scale = sage::scene::AllowedScale(scene.Registry(), selected.Entity(), scaleBefore, tr.Scale);
    // ВО ВРЕМЯ ИГРЫ ПОЛОЖЕНИЕ ВЕДЁТ ФИЗИКА, и правку Transform она затирает на
    // следующем же шаге: объект возвращался на место, будто мышь ничего не
    // делала. Переставляем и само тело — тогда ящик можно подвинуть под игрока
    // прямо на ходу, ради чего это и нужно.
    host.SyncBodyToTransform(selected);

    // Мультивыделение: та же мировая дельта — на остальные выбранные.
    if (host.Selection().All().size() <= 1) return;
    const glm::mat4 delta = world * glm::inverse(m_dragStartPrimary);
    for (auto& [id, startWorld] : m_dragStartWorlds) {
        if (id == selected.Id()) continue;
        GameObject o = scene.Get(id);
        if (!o.Valid() || scene.IsFolder(o.Entity())) continue;
        // Пропускаем потомков других выбранных: их несёт двигающийся родитель,
        // иначе смещение применилось бы дважды.
        bool ancestorSelected = false;
        for (entt::entity a = scene.ParentOf(o.Entity()); a != entt::null; a = scene.ParentOf(a)) {
            if (host.Selection().Contains(scene.Registry().get<IdComponent>(a).Id)) {
                ancestorSelected = true;
                break;
            }
        }
        if (ancestorSelected) continue;
        // В группе масштабируют все, кому можно: камера, попавшая в набор с
        // ящиками, едет вместе с ними, но не растёт.
        Transform& ot = o.GetTransform();
        const glm::vec3 otherBefore = ot.Scale;
        ot.SetFromMatrix(ToLocal(scene, o.Entity(), delta * startWorld));
        ot.Scale = sage::scene::AllowedScale(scene.Registry(), o.Entity(), otherBefore, ot.Scale);
        host.SyncBodyToTransform(o);
    }
}
