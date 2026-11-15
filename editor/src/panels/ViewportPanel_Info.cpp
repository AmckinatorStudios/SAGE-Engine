// ---------------------------------------------------------------------------
// ИНФОРМАЦИОННЫЙ СЛОЙ ВЬЮПОРТА — несколько строк поверх кадра в левом нижнем
// углу: какой это вид, что выделено, где камера, во что обходится кадр.
//
// ПАССИВНЫЙ. Только рисунок в списке отрисовки окна: ни одного элемента ImGui,
// поэтому наведение, клики, выбор рамкой, гизмо и полёт камеры его не видят.
//
// НЕ НАКРЫВАЕТ СОСЕДЕЙ. Место — левый нижний угол (подпись вида стояла там и
// раньше); мешает превью камеры, строка инструментов или гизмо ориентации —
// блок поднимается над ними, а не помещается — не рисуется вовсе.
// Предупреждения рендера и подсказка «нет света» от него не зависят: они
// обходят занятое им место (см. ViewportPanel::Draw).
//
// Что показывать — привычка человека (EditorPrefs «viewport.overlay.*»), а не
// свойство проекта или сцены. Цифры — из того же места, что строка состояния
// (EditorHost::SceneStats), камера — из самой камеры и OrthoView.
// ---------------------------------------------------------------------------
#include "ViewportPanel.h"

#include <algorithm>

#include "imgui.h"

#include "EditorHost.h"
#include "EditorTheme.h"
#include "ViewGizmo.h"
#include "../ui/UI.h"

namespace vptools = sage::editor::vptools;

vptools::OverlayPrefs& ViewportPanel::OverlayPrefsRef() {
    if (!m_overlayLoaded) {
        m_overlay.Load();
        m_overlayLoaded = true;
    }
    return m_overlay;
}

vptools::Box ViewportPanel::DrawInfoOverlay(EditorHost& host, int slot, ViewKind kind, ImVec2 pos, ImVec2 cell,
                                            int viewCount) {
    const int s = slot & 3;
    m_infoRect[s] = {};
    vptools::OverlayInput in;
    in.ViewName = ViewKindName(kind);
    in.Slot = slot;
    in.SlotCount = viewCount;
    in.Active = slot == m_activeSlot;
    in.Perspective = kind == ViewKind::Perspective;
    const Camera& cam = host.EditorCamera();
    in.CameraPos = cam.Position;
    in.Fov = cam.Fov;
    in.OrthoHeight = m_ortho[s].Height;
    in.SelectedCount = (int)host.Selection().All().size();
    if (const GameObject sel = host.SelectedObject(); sel.Valid()) in.SelectedName = sel.Name();
    const EditorHost::SceneStatsInfo st = host.SceneStats();
    in.Fps = st.Fps;
    in.MemoryGb = st.MemoryGb;
    in.DrawCalls = st.DrawCalls;
    in.Triangles = st.Triangles;
    in.Objects = st.Objects;
    m_infoLines[s] = vptools::OverlayLines(OverlayPrefsRef(), in);
    const std::vector<std::string>& lines = m_infoLines[s];
    if (lines.empty()) return {};

    const Sage::UI::Style& ui = Sage::UI::Get();
    const float padX = ui.SpacingSM, padY = ui.SpacingXS, lineGap = std::floor(ui.SpacingXS * 0.5f);
    const float lh = ImGui::GetTextLineHeight();
    float w = 0.0f;
    for (const std::string& l : lines) w = std::max(w, ImGui::CalcTextSize(l.c_str()).x);
    w = std::floor(w + padX * 2.0f);
    const float h = std::floor((float)lines.size() * lh + (float)(lines.size() - 1) * lineGap + padY * 2.0f);

    // Что обходить: строка инструментов, превью камеры, гизмо ориентации.
    std::vector<vptools::Box> avoid;
    auto add = [&](ImVec2 a, ImVec2 b) {
        if (b.x > a.x && b.y > a.y) avoid.push_back({a.x, a.y, b.x, b.y});
    };
    add(m_toolsMin, m_toolsMax);
    add(m_previewMin, m_previewMax);
    if (in.Active) {
        const float g = sage::editor::viewgizmo::Extent();
        add(ImVec2(pos.x + cell.x - g, pos.y), ImVec2(pos.x + cell.x, pos.y + g));
    }
    const vptools::Box cellBox{pos.x, pos.y, pos.x + cell.x, pos.y + cell.y};
    const vptools::Box b = vptools::PlaceBottomLeft(cellBox, w, h, ui.SpacingSM, avoid);
    if (b.Empty()) return {};

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(b.X0, b.Y0), ImVec2(b.X1, b.Y1), EditorTheme::Color32Alpha(EditorTheme::Role::Bg, 0.55f),
                      EditorTheme::KitMetrics().Rounding);
    float y = b.Y0 + padY;
    for (size_t i = 0; i < lines.size(); ++i) {
        // Первая строка активного вида — ярче: по ней находят, где работают гизмо.
        const bool strong = i == 0 && in.Active && OverlayPrefsRef().View;
        dl->AddText(ImVec2(b.X0 + padX, y),
                    EditorTheme::Color32(strong ? EditorTheme::Role::Text : EditorTheme::Role::TextDim),
                    lines[i].c_str());
        y += lh + lineGap;
    }
    m_infoRect[s] = b;
    return b;
}
