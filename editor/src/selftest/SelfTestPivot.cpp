#include "SelfTestCommon.h"

#include <glm/gtc/matrix_transform.hpp>

#include "panels/InspectorPanelDetail.h"
#include "sage/render/Mesh.h"
#include "sage/scene/SceneSerializer.h"

// ОПОРА (Transform::Pivot) В РЕДАКТОРЕ: готовые положения по габаритам меша,
// отмена и повтор правки опоры, гизмо в режиме Edit Pivot (тянет опору, объект
// стоит), обычное гизмо поворачивает вокруг опоры, ребёнок под повёрнутым
// родителем, сохранение сцены.
bool EditorLayer::SelfTestPivot() {
    bool ok = true;
    auto fail = [&](const std::string& why) {
        LOG_ERROR("Editor") << "SELFTEST: pivot: " << why;
        ok = false;
    };
    auto same = [](const glm::mat4& a, const glm::mat4& b) {
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                if (std::abs(a[c][r] - b[c][r]) > 1e-4f) return false;
        return true;
    };
    auto near3 = [](const glm::vec3& a, const glm::vec3& b) { return glm::length(a - b) < 1e-4f; };

    GameObject box = CreateCubeEntity("Pivot Probe");
    if (!box.Valid()) {
        fail("куб не создан");
        return ok;
    }
    Transform& tr = box.GetTransform();
    tr.Position = {2, 1, 0};
    tr.Rotation = {0, 30, 0};
    tr.Scale = {2, 1, 1};
    const glm::mat4 start = m_scene->WorldMatrix(box.Entity());

    // Готовое положение — по габаритам меша, объект на месте.
    const auto* mr = m_scene->Registry().try_get<MeshRendererComponent>(box.Entity());
    if (!mr || !mr->MeshPtr) {
        fail("у куба нет меша — нечем считать габариты");
    } else {
        const glm::vec3 lo = mr->MeshPtr->BoundsMin(), hi = mr->MeshPtr->BoundsMax();
        const glm::vec3 bottom = sage::editor::inspectordetail::PivotPreset(lo, hi, {0, -1, 0});
        PushUndoSnapshot();
        tr.SetPivotKeepPlace(bottom);
        if (!near3(tr.Pivot, glm::vec3((lo.x + hi.x) * 0.5f, lo.y, (lo.z + hi.z) * 0.5f)))
            fail("«Низ, центр» не на нижней грани габаритов");
        if (!same(start, m_scene->WorldMatrix(box.Entity()))) fail("выбор положения сдвинул объект");
        const glm::vec3 corner = sage::editor::inspectordetail::PivotPreset(lo, hi, {1, 1, -1});
        if (!near3(corner, glm::vec3(hi.x, hi.y, lo.z))) fail("угол «верх, справа, сзади» не в углу габаритов");
        // Отмена возвращает прежнюю опору, повтор — новую.
        Undo();
        box = m_scene->FindByName("Pivot Probe");
        if (!box.Valid() || box.GetTransform().Pivot != glm::vec3(0.0f)) fail("отмена не вернула опору");
        Redo();
        box = m_scene->FindByName("Pivot Probe");
        if (!box.Valid() || !near3(box.GetTransform().Pivot, bottom)) fail("повтор не вернул опору");
    }
    if (!box.Valid()) return ok;

    // Edit Pivot: гизмо стоит в опоре, его перенос двигает только опору.
    {
        Transform& t = box.GetTransform();
        const glm::mat4 world = m_scene->WorldMatrix(box.Entity());
        glm::mat4 gizmo = ViewportPanel::GizmoFrame(world, t);
        if (!near3(glm::vec3(gizmo[3]), glm::vec3(world * glm::vec4(t.Pivot, 1.0f)))) fail("гизмо стоит не в опоре");
        gizmo[3] += glm::vec4(0.0f, 0.75f, 0.0f, 0.0f);
        m_viewport.ApplyGizmo(*this, box, gizmo, true);
        if (!same(world, m_scene->WorldMatrix(box.Entity()))) fail("перенос опоры гизмо сдвинул объект");
        const glm::vec3 pivotWorld = glm::vec3(m_scene->WorldMatrix(box.Entity()) * glm::vec4(t.Pivot, 1.0f));
        if (!near3(pivotWorld, glm::vec3(gizmo[3]))) fail("опора не пришла туда, куда её дотащили");

        // Обычное гизмо: поворот вокруг опоры держит опору на месте.
        const glm::mat4 frame = ViewportPanel::GizmoFrame(m_scene->WorldMatrix(box.Entity()), t);
        const glm::mat4 turned = frame * glm::rotate(glm::mat4(1.0f), glm::radians(40.0f), glm::vec3(0, 1, 0));
        m_viewport.ApplyGizmo(*this, box, turned, false);
        const glm::vec3 after = glm::vec3(m_scene->WorldMatrix(box.Entity()) * glm::vec4(t.Pivot, 1.0f));
        if (!near3(after, pivotWorld)) fail("поворот гизмо ушёл с опоры");
        // Масштаб гизмо — тоже от опоры.
        const glm::mat4 frame2 = ViewportPanel::GizmoFrame(m_scene->WorldMatrix(box.Entity()), t);
        m_viewport.ApplyGizmo(*this, box, frame2 * glm::scale(glm::mat4(1.0f), glm::vec3(1.5f)), false);
        const glm::vec3 after2 = glm::vec3(m_scene->WorldMatrix(box.Entity()) * glm::vec4(t.Pivot, 1.0f));
        if (!near3(after2, pivotWorld)) fail("масштаб гизмо ушёл с опоры");
    }

    // Ребёнок под повёрнутым родителем: перенос опоры гизмо не двигает его.
    GameObject child = CreateCubeEntity("Pivot Child");
    if (child.Valid()) {
        m_scene->SetParent(child.Entity(), box.Entity());
        child.GetTransform().Position = {0.5f, 1, 0};
        const glm::mat4 world = m_scene->WorldMatrix(child.Entity());
        glm::mat4 gizmo = ViewportPanel::GizmoFrame(world, child.GetTransform());
        gizmo[3] += glm::vec4(0.3f, -0.2f, 0.4f, 0.0f);
        m_viewport.ApplyGizmo(*this, child, gizmo, true);
        if (!same(world, m_scene->WorldMatrix(child.Entity()))) fail("опора ребёнка сдвинула ребёнка");
        const glm::vec3 pw = glm::vec3(m_scene->WorldMatrix(child.Entity()) * glm::vec4(child.GetTransform().Pivot, 1.0f));
        if (!near3(pw, glm::vec3(gizmo[3]))) fail("опора ребёнка не там, куда её дотащили");
    } else {
        fail("ребёнок не создан");
    }

    // Сохранение сцены: опора и положение переживают перечитывание.
    {
        const glm::mat4 world = m_scene->WorldMatrix(box.Entity());
        const glm::vec3 pivot = box.GetTransform().Pivot;
        std::unique_ptr<Scene> back = SceneSerializer::LoadFromString(SceneSerializer::SaveToString(*m_scene));
        GameObject again = back ? back->FindByName("Pivot Probe") : GameObject();
        if (!again.Valid()) fail("сцена с опорой не перечиталась");
        else {
            if (!near3(again.GetTransform().Pivot, pivot)) fail("опора не сохранилась в сцене");
            if (!same(world, back->WorldMatrix(again.Entity()))) fail("после перечитывания объект стоит иначе");
        }
    }

    if (child.Valid()) m_scene->RemoveObject(child.Id());
    m_scene->RemoveObject(box.Id());
    m_tools.PivotEdit = false;
    if (ok) LOG_INFO("Editor") << "PIVOT: OK — положения, отмена, гизмо опоры, поворот и масштаб вокруг неё, иерархия, сцена";
    return ok;
}
