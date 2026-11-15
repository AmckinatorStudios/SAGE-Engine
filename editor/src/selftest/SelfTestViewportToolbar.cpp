#include "SelfTestCommon.h"

// ============================================================================
//  СТРОКА ИНСТРУМЕНТОВ И СЛОЙ ИНФОРМАЦИИ ВЬЮПОРТА — в живом кадре
// ============================================================================
//
// Модульные тесты проверяют правила (перенос, строки слоя, размещение); здесь —
// что их видно в настоящем вьюпорте:
//   * поле шагов привязки есть, только пока привязка включена: G включает —
//     поле в следующем кадре на месте, G выключает — пропадает сразу; шаги
//     при этом не теряются;
//   * кнопка правки коллайдера доступна у коробки-коллайдера и погашена у
//     объекта без коллайдера;
//   * щелчок по кнопке строки работает и НЕ выбирает объект под строкой;
//   * строка не выходит за край вида;
//   * слой информации выключается одним переключателем, не пересекается со
//     строкой инструментов и есть у каждого из четырёх видов.
// События мыши и клавиш — в очередь ImGui, как у VIEWPORT_INPUT.
void EditorLayer::TickViewportToolbarProbe() {
    if (m_toolbarProbeStep < 0) return;
    if (!std::getenv("SAGE_EDITOR_SELFTEST")) return;
    // После остальных живых проверок: им нужна свободная мышь и прежний вид.
    if (m_probeStep >= 0 || m_cursorProbeStep >= 0 || m_luaSelfTestStep >= 0 || m_colliderProbeStep >= 0 ||
        m_play.Active() || !m_scene)
        return;
    ImGuiIO& io = ImGui::GetIO();
    if (ImGuiWindow* w = ImGui::FindWindowByID(ImHashStr("Viewport")))
        if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) ImGui::FocusWindow(w);
    if (m_toolbarProbeWait > 0) { --m_toolbarProbeWait; return; }

    const ImVec2 vmin = m_viewport.ViewMin(), vmax = m_viewport.ViewMax();
    const ImVec2 center((vmin.x + vmax.x) * 0.5f, (vmin.y + vmax.y) * 0.65f);
    auto done = [&](bool ok, const std::string& why) {
        if (ok) LOG_INFO("Editor") << "VIEWPORT_TOOLBAR: OK — " << why;
        else LOG_ERROR("Editor") << "VIEWPORT_TOOLBAR: FAIL — " << why;
        m_tools.Snap = m_toolbarProbeSnap;
        m_tools.SaveSnap();
        m_tools.GizmoOp = m_toolbarProbeOp;
        m_viewport.SetViewCountForTest(1);
        m_viewport.Overlay().Enabled = true;
        for (int id : {m_toolbarProbeBox, m_toolbarProbePlain})
            if (m_scene->Get(id).Valid()) m_scene->RemoveObject(id);
        m_selection.Clear();
        m_toolbarProbeStep = -1;
    };
    auto fail = [&](const std::string& why) { done(false, why); };

    switch (m_toolbarProbeStep) {
        case 0: {
            SetWorkspace(EditorWorkspace::Scene);
            m_toolbarProbeSnap = m_tools.Snap;
            m_toolbarProbeOp = m_tools.GizmoOp;
            m_toolbarProbeMove = m_tools.SnapMove;
            m_tools.Snap = false;
            GameObject box = CreatePrimitiveEntity("ToolbarProbeBox", MeshRef::Type::Cube);
            m_scene->Registry().emplace_or_replace<ColliderComponent>(box.Entity());
            m_toolbarProbeBox = box.Id();
            m_toolbarProbePlain = CreatePrimitiveEntity("ToolbarProbePlain", MeshRef::Type::Sphere).Id();
            m_selection.SetPrimary(m_toolbarProbeBox);
            io.AddMousePosEvent(center.x, center.y);
            m_toolbarProbeWait = 3;
            ++m_toolbarProbeStep;
            return;
        }
        case 1:
            if (m_viewport.SnapFieldShown()) return fail("привязка выключена, а поле шагов нарисовано");
            if (!m_viewport.ColliderButtonEnabled()) return fail("у коробки-коллайдера кнопка правки погашена");
            if (m_viewport.ToolsMax().x > vmax.x + 0.5f) return fail("строка инструментов вылезла за край вида");
            io.AddKeyEvent(ImGuiKey_G, true);   // G — включить
            ++m_toolbarProbeStep;
            return;
        case 2:
            io.AddKeyEvent(ImGuiKey_G, false);
            m_toolbarProbeWait = 2;
            ++m_toolbarProbeStep;
            return;
        case 3:
            if (!m_tools.Snap) return fail("G не включила привязку");
            if (!m_viewport.SnapFieldShown()) return fail("привязка включена, а поля шагов нет");
            io.AddKeyEvent(ImGuiKey_G, true);   // G — выключить
            ++m_toolbarProbeStep;
            return;
        case 4:
            io.AddKeyEvent(ImGuiKey_G, false);
            ++m_toolbarProbeStep;
            return;
        case 5:
            if (m_tools.Snap) return fail("G не выключила привязку");
            if (m_viewport.SnapFieldShown()) return fail("привязка выключена, а поле шагов осталось");
            if (m_tools.SnapMove != m_toolbarProbeMove) return fail("выключение привязки тронуло шаги");
            m_selection.SetPrimary(m_toolbarProbePlain);
            m_toolbarProbeWait = 2;
            ++m_toolbarProbeStep;
            return;
        case 6: {
            if (m_viewport.ColliderButtonEnabled()) return fail("у объекта без коллайдера кнопка правки доступна");
            // Щелчок по первой кнопке строки (выбор): работает, а объект под
            // строкой не выбирается.
            m_selection.Clear();
            m_tools.GizmoOp = (int)ImGuizmo::TRANSLATE;
            const float h = ImGui::GetFrameHeight();
            m_toolbarProbeClick = ImVec2(m_viewport.ToolsMin().x + h * 0.5f, m_viewport.ToolsMin().y + h * 0.5f);
            io.AddMousePosEvent(m_toolbarProbeClick.x, m_toolbarProbeClick.y);
            m_toolbarProbeWait = 1;
            ++m_toolbarProbeStep;
            return;
        }
        case 7:
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            ++m_toolbarProbeStep;
            return;
        case 8:
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            m_toolbarProbeWait = 2;
            ++m_toolbarProbeStep;
            return;
        case 9: {
            if (m_tools.GizmoOp != EditorHost::kGizmoSelectOnly) return fail("щелчок по кнопке «Выбор» не сработал");
            if (!m_selection.Empty()) return fail("щелчок по строке инструментов выбрал объект под ней");
            // Слой информации: есть, не на строке инструментов, выключается.
            const auto& info = m_viewport.InfoRect(0);
            const sage::editor::vptools::Box tools{m_viewport.ToolsMin().x, m_viewport.ToolsMin().y,
                                                   m_viewport.ToolsMax().x, m_viewport.ToolsMax().y};
            if (m_viewport.InfoLines(0).empty() || info.Empty()) return fail("слоя информации нет при настройках по умолчанию");
            if (info.Overlaps(tools)) return fail("слой информации лёг на строку инструментов");
            m_viewport.Overlay().Enabled = false;
            m_toolbarProbeWait = 2;
            ++m_toolbarProbeStep;
            return;
        }
        case 10:
            if (!m_viewport.InfoLines(0).empty() || !m_viewport.InfoRect(0).Empty())
                return fail("слой выключен, а строки остались");
            m_viewport.Overlay().Enabled = true;
            m_viewport.SetViewCountForTest(4);
            m_toolbarProbeWait = 3;
            ++m_toolbarProbeStep;
            return;
        case 11: {
            // Четыре вида: у каждого — свой слой с номером вида, без пересечений.
            for (int i = 0; i < 4; ++i) {
                const auto& lines = m_viewport.InfoLines(i);
                if (lines.empty() || lines[0].find('4') == std::string::npos)
                    return fail("у вида " + std::to_string(i + 1) + " из четырёх нет подписи");
                for (int k = 0; k < i; ++k)
                    if (m_viewport.InfoRect(i).Overlaps(m_viewport.InfoRect(k)))
                        return fail("слои информации двух видов пересеклись");
            }
            return done(true, "поле шагов есть только при включённой привязке (G), шаги сохраняются; кнопка "
                              "коллайдера доступна только у формы с ручками; щелчок по строке не выбирает объект; "
                              "слой информации выключается и есть у каждого из четырёх видов");
        }
        default: return;
    }
}
