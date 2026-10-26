#include "SelfTestCommon.h"

#include "../gizmo/ColliderEdit.h"
#include "../gizmo/Handles.h"

// ============================================================================
//  РУЧКИ КОЛЛАЙДЕРА В ЖИВОМ ВЬЮПОРТЕ: настоящая мышь тянет грань коробки
// ============================================================================
//
// Модульный тест проверяет математику ручек; здесь — что её видно и за неё
// можно взяться: режим C, ручка +X стоит там, где грань, протяжка мышью её
// двигает, противоположная грань стоит, а одна отмена возвращает всё целиком.
// События мыши — в очередь ImGui, как у проверки VIEWPORT_INPUT.
void EditorLayer::TickColliderHandleProbe() {
    if (m_colliderProbeStep < 0) return;
    if (!std::getenv("SAGE_EDITOR_SELFTEST")) return;
    // После остальных живых проверок: им нужен обычный гизмо и свободная мышь.
    if (m_probeStep >= 0 || m_cursorProbeStep >= 0 || m_luaSelfTestStep >= 0 || m_play.Active()) return;
    ImGuiIO& io = ImGui::GetIO();
    if (m_colliderProbeStep > 1) io.AddMousePosEvent(m_colliderProbePos.x, m_colliderProbePos.y);
    if (ImGuiWindow* w = ImGui::FindWindowByID(ImHashStr("Viewport")))
        if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) ImGui::FocusWindow(w);
    if (m_colliderProbeWait > 0) { --m_colliderProbeWait; return; }

    auto done = [&](bool ok, const std::string& why) {
        if (ok) LOG_INFO("Editor") << "COLLIDER_HANDLES: OK — " << why;
        else LOG_ERROR("Editor") << "COLLIDER_HANDLES: FAIL — " << why;
        m_tools.ColliderEdit = false;
        if (m_scene && m_scene->Get(m_colliderProbeId).Valid()) m_scene->RemoveObject(m_colliderProbeId);
        m_selection.Clear();
        m_colliderProbeStep = -1;
    };
    GameObject box = m_scene ? m_scene->Get(m_colliderProbeId) : GameObject();
    // Ручка — ровно та, что вьюпорт нарисовал в прошлом кадре, и ось — та,
    // протяжка вдоль которой на экране длиннее (камера может смотреть ровно
    // вдоль одной из осей, и тогда ход по ней на экране — точка).
    const int axis = m_colliderProbeAxis;
    auto handleScreen = [&](ImVec2& out, ImVec2& along) {
        const int axis = m_colliderProbeAxis;   // уже выбранная в этом кадре
        const auto& ch = m_viewport.ColliderHandlesState();
        const sage::editor::handles::View& v = ch.LastView();
        for (const sage::editor::handles::Point& p : ch.LastPoints()) {
            if (p.Id != axis * 2 + 1) continue;
            const glm::mat4 world = m_scene->WorldMatrix(box.Entity());
            ImVec2 a, b;
            if (!sage::editor::handles::ToScreen(v, p.World, a) ||
                !sage::editor::handles::ToScreen(v, p.World + glm::normalize(glm::vec3(world[axis])), b))
                return false;
            out = a;
            const float len = std::hypot(b.x - a.x, b.y - a.y);
            if (len < 1.0f) return false;
            along = ImVec2((b.x - a.x) / len, (b.y - a.y) / len);
            return true;
        }
        return false;
    };

    switch (m_colliderProbeStep) {
        case 0: {
            if (!m_scene) return;
            SetWorkspace(EditorWorkspace::Scene);
            GameObject o = CreatePrimitiveEntity("ColliderProbe", MeshRef::Type::Cube);
            o.GetTransform().Position = {0.0f, 0.5f, 0.0f};
            o.GetTransform().Scale = {2.0f, 1.0f, 1.0f};
            m_scene->Registry().emplace_or_replace<ColliderComponent>(o.Entity());
            m_colliderProbeId = o.Id();
            m_selection.SetPrimary(o.Id());
            m_tools.ColliderEdit = true;
            FocusSelected();
            m_colliderProbeWait = 40;   // камера долетает до объекта
            ++m_colliderProbeStep;
            return;
        }
        case 1: {
            if (!box.Valid()) return done(false, "объект проверки пропал");
            // Камера долетела? Вид не меняется десять кадров подряд.
            if (m_view != m_colliderProbeView) {
                m_colliderProbeView = m_view;
                m_colliderProbeStill = 0;
                return;
            }
            if (++m_colliderProbeStill < 10) return;
            // Ось, протяжка вдоль которой на экране длиннее (X или Z).
            float bestLen = 0.0f;
            {
                const auto& ch = m_viewport.ColliderHandlesState();
                const glm::mat4 world = m_scene->WorldMatrix(box.Entity());
                for (int a : {0, 2})
                    for (const sage::editor::handles::Point& p : ch.LastPoints()) {
                        if (p.Id != a * 2 + 1) continue;
                        ImVec2 p0, p1;
                        if (sage::editor::handles::ToScreen(ch.LastView(), p.World, p0) &&
                            sage::editor::handles::ToScreen(ch.LastView(),
                                                            p.World + glm::normalize(glm::vec3(world[a])), p1)) {
                            const float len = std::hypot(p1.x - p0.x, p1.y - p0.y);
                            if (len > bestLen) { bestLen = len; m_colliderProbeBest = a; }
                        }
                    }
            }
            if (bestLen < 8.0f) return;   // ручки ещё не нарисованы
            m_colliderProbeAxis = m_colliderProbeBest;
            ImVec2 at, dir;
            if (!handleScreen(at, dir)) return done(false, "ручка за камерой");
            m_colliderProbeDir = dir;
            m_colliderProbePos = at;
            m_colliderProbeStart = at;
            io.AddMousePosEvent(at.x, at.y);   // навести: ручка подсвечивается
            m_colliderProbeWait = 2;
            ++m_colliderProbeStep;
            return;
        }
        case 2:
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            ++m_colliderProbeStep;
            return;
        case 3: case 4: case 5: case 6: case 7: case 8:
            // Протяжка наружу по экранной оси X, по 12 пикселей за кадр.
            m_colliderProbePos.x += m_colliderProbeDir.x * 12.0f;
            m_colliderProbePos.y += m_colliderProbeDir.y * 12.0f;
            ++m_colliderProbeStep;
            return;
        case 9:
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            m_colliderProbeWait = 2;
            ++m_colliderProbeStep;
            return;
        case 10: {
            if (!box.Valid()) return done(false, "объект проверки пропал");
            const ColliderComponent col = m_scene->Registry().get<ColliderComponent>(box.Entity());
            const glm::vec3 scale = box.GetTransform().Scale;
            const int a = axis;
            const bool grew = col.HalfExtents[a] > 0.55f;
            const bool oppositeStays = std::abs(col.Center[a] - col.HalfExtents[a] + 0.5f) < 1e-3f;
            const bool objectUntouched = scale == glm::vec3(2.0f, 1.0f, 1.0f);
            const bool othersUntouched = std::abs(col.HalfExtents.y - 0.5f) < 1e-6f &&
                                         std::abs(col.HalfExtents[2 - a] - 0.5f) < 1e-6f;
            Undo();
            const ColliderComponent* back = m_scene->Registry().try_get<ColliderComponent>(
                m_scene->Get(m_colliderProbeId).Entity());
            const bool undone = back && std::abs(back->HalfExtents[a] - 0.5f) < 1e-4f &&
                                std::abs(back->Center[a]) < 1e-4f;
            char buf[320];
            ImVec2 now(0, 0), dir;
            handleScreen(now, dir);
            std::snprintf(buf, sizeof(buf),
                          "грань +%c протянута мышью: половина %.3f, центр %.3f (ручка была %.0f,%.0f, мышь "
                          "ушла в %.0f,%.0f, ImGui видит %.0f,%.0f, ручка теперь %.0f,%.0f)",
                          "XYZ"[a], col.HalfExtents[a], col.Center[a], m_colliderProbeStart.x, m_colliderProbeStart.y,
                          m_colliderProbePos.x, m_colliderProbePos.y, io.MousePos.x, io.MousePos.y, now.x, now.y);
            if (grew && oppositeStays && objectUntouched && othersUntouched && undone)
                return done(true, std::string(buf) + "; противоположная грань стоит, масштаб объекта не тронут, отмена вернула форму");
            return done(false, std::string(buf) + (grew ? "" : "; грань не сдвинулась") +
                                   (oppositeStays ? "" : "; противоположная грань уехала") +
                                   (objectUntouched ? "" : "; изменился масштаб объекта") +
                                   (othersUntouched ? "" : "; изменились другие оси") +
                                   (undone ? "" : "; отмена не вернула форму"));
        }
        default: return;
    }
}
