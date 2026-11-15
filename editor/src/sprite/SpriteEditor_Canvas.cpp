#include "SpriteEditor.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "imgui.h"

#include "../AssetSlot.h"
#include "../EditorHost.h"
#include "../EditorIcons.h"
#include "../EditorTheme.h"
#include "../Localization.h"
#include "SpriteEditorDetail.h"
#include "sage/render/Texture.h"
#include "../InspectorControls.h"

using sage::editor::T;
namespace sp = sage::editor::sprite;
using Method = sage::ui::SpriteSlicing::Mode;

namespace {

ImVec2 HandlePos(ImVec2 p0, ImVec2 p1, int h) {
    const float mx = (p0.x + p1.x) * 0.5f, my = (p0.y + p1.y) * 0.5f;
    switch (h) {
    case sp::HTopLeft: return p0;
    case sp::HTop: return {mx, p0.y};
    case sp::HTopRight: return {p1.x, p0.y};
    case sp::HRight: return {p1.x, my};
    case sp::HBottomRight: return p1;
    case sp::HBottom: return {mx, p1.y};
    case sp::HBottomLeft: return {p0.x, p1.y};
    default: return {p0.x, my};
    }
}

ImGuiMouseCursor HandleCursor(int h) {
    switch (h) {
    case sp::HTopLeft:
    case sp::HBottomRight: return ImGuiMouseCursor_ResizeNWSE;
    case sp::HTopRight:
    case sp::HBottomLeft: return ImGuiMouseCursor_ResizeNESW;
    case sp::HTop:
    case sp::HBottom: return ImGuiMouseCursor_ResizeNS;
    default: return ImGuiMouseCursor_ResizeEW;
    }
}

// Подпись над рамкой: имя куска на тёмной подложке, чтобы читалось на любой
// картинке.
void NameTag(ImDrawList* dl, ImVec2 at, const char* text, ImU32 color) {
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const ImVec2 a(at.x, at.y - ts.y - 4.0f), b(at.x + ts.x + 8.0f, at.y);
    dl->AddRectFilled(a, b, IM_COL32(20, 20, 24, 210), 3.0f);
    dl->AddText(ImVec2(a.x + 4.0f, a.y + 2.0f), color, text);
}

} // namespace

ImVec2 SpriteEditor::ToScreen(glm::vec2 px) const {
    const float ox = m_canvasA.x + m_canvasSize.x * 0.5f + m_pan.x - m_contentW * m_zoom * 0.5f;
    const float oy = m_canvasA.y + m_canvasSize.y * 0.5f + m_pan.y - m_contentH * m_zoom * 0.5f;
    return {ox + px.x * m_zoom, oy + px.y * m_zoom};
}

glm::vec2 SpriteEditor::ToPixel(ImVec2 s) const {
    const ImVec2 o = ToScreen(glm::vec2(0.0f));
    return {(s.x - o.x) / std::max(1e-4f, m_zoom), (s.y - o.y) / std::max(1e-4f, m_zoom)};
}

void SpriteEditor::UpdateView(ImVec2 a, ImVec2 size, float contentW, float contentH) {
    m_canvasA = a;
    m_canvasSize = size;
    m_contentW = contentW;
    m_contentH = contentH;
    // Пока вид не трогали руками, он следует за размером холста: окно
    // разложилось, инспектор спрятали — картинка снова вписана.
    if (!m_userView && (std::fabs(size.x - m_fitFor.x) > 0.5f || std::fabs(size.y - m_fitFor.y) > 0.5f)) m_zoom = 0.0f;
    if (m_zoom <= 0.0f && contentW > 0.0f && contentH > 0.0f) {
        m_fitFor = glm::vec2(size.x, size.y);
        // Вписать: картинка целиком с полем вокруг.
        m_zoom = std::clamp(std::min(size.x * 0.88f / contentW, size.y * 0.88f / contentH), 0.05f, 64.0f);
        m_pan = glm::vec2(0.0f);
    }
}

// --- Окно холста -----------------------------------------------------------

void SpriteEditor::DrawCanvasWindow(EditorHost& host) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const bool shown = ImGui::Begin(T("Canvas###SpriteEditorCanvas"), nullptr,
                                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                        ImGuiWindowFlags_NoCollapse);
    ImGui::PopStyleVar();
    if (!shown) {
        ImGui::End();
        return;
    }
    const float barH = ImGui::GetFrameHeight() + 6.0f;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const ImVec2 size(std::max(50.0f, avail.x), std::max(50.0f, avail.y - barH));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, ImVec2(a.x + size.x, a.y + size.y), EditorTheme::Color32(EditorTheme::Role::Bg));
    if (!m_tex || !m_pixels) {
        ImGui::BeginChild("##sp_empty", size, false, ImGuiWindowFlags_NoBackground);
        DrawEmptyState(host);
        ImGui::EndChild();
    } else {
        dl->PushClipRect(a, ImVec2(a.x + size.x, a.y + size.y), true);
        // Панель инструментов подаётся ПЕРВОЙ (наведение достаётся первому
        // поданному элементу — кнопки поверх холста нажимаются, а не тянут
        // картинку), а рисуется в верхнем слое — поверх картинки.
        dl->ChannelsSplit(2);
        dl->ChannelsSetCurrent(1);
        DrawCanvasToolbar(a, size);
        dl->ChannelsSetCurrent(0);
        ImGui::SetCursorScreenPos(a);
        ImGui::InvisibleButton("##sp_canvas", size,
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                                   ImGuiButtonFlags_MouseButtonMiddle);
        if (m_mode == Mode::Sheet) {
            UpdateView(a, size, (float)m_w, (float)m_h);
            HandleSheetMouse(a, size);
            DrawSheetCanvas(a, size);
        } else if (m_nineResult) {
            DrawNineResult(a, size);
        } else {
            DrawNineCanvas(a, size);
        }
        dl->ChannelsMerge();
        dl->PopClipRect();
    }
    ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + size.y));
    DrawStatusBar();
    ImGui::End();
}

void SpriteEditor::DrawEmptyState(EditorHost& host) {
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float cardW = std::min(420.0f, avail.x - 32.0f);
    const float cardH = 210.0f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 a(p0.x + (avail.x - cardW) * 0.5f, p0.y + std::max(16.0f, (avail.y - cardH) * 0.45f));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, ImVec2(a.x + cardW, a.y + cardH), EditorTheme::Color32(EditorTheme::Role::Surface), 10.0f);
    dl->AddRect(a, ImVec2(a.x + cardW, a.y + cardH), EditorTheme::Color32(EditorTheme::Role::Line), 10.0f);
    EditorIcons::Overlay(dl, a.x + cardW * 0.5f - 20.0f, a.y + 22.0f, 40.0f, "texture");
    const char* title = T("Open a texture to start");
    const char* hint = T("Pick a PNG below or drag one here from Assets. Its sprites are kept next to it.");
    const ImVec2 ts = ImGui::CalcTextSize(title);
    dl->AddText(ImVec2(a.x + (cardW - ts.x) * 0.5f, a.y + 74.0f), EditorTheme::Color32(EditorTheme::Role::Text), title);
    ImGui::SetCursorScreenPos(ImVec2(a.x + 24.0f, a.y + 100.0f));
    // Перенос — в координатах окна, а не экрана.
    ImGui::PushTextWrapPos(a.x + cardW - 24.0f - ImGui::GetWindowPos().x);
    ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(EditorTheme::Role::TextDim));
    ImGui::TextWrapped("%s", hint);
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    ImGui::SetCursorScreenPos(ImVec2(a.x + 24.0f, a.y + cardH - 66.0f));
    // Слот занимает всю ширину своего окна — даём ему окно шириной карточки.
    ImGui::BeginChild("##sp_open_slot", ImVec2(cardW - 48.0f, 56.0f), false,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
    const assetslot::Result r = assetslot::Draw(host, "##sp_open", assetslot::Kind::Texture, m_path, nullptr,
                                                T("Drop a texture here"));
    ImGui::EndChild();
    if (r.Changed) Load(r.Path);
}

// --- Панель инструментов холста ---------------------------------------------

void SpriteEditor::DrawCanvasToolbar(ImVec2 a, ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float btn = ImGui::GetFrameHeight();
    const ImGuiStyle& st = ImGui::GetStyle();
    // Слева — инструменты (только в режиме листа и только нужные сейчас),
    // справа — вид: вписать, 100%, мельче, крупнее, сетка пикселей.
    if (m_mode == Mode::Sheet) {
        struct ToolDef { Tool tool; const char* icon; const char* tip; bool show; };
        const bool manual = m_doc.Sheet.Slicing.Method == Method::Manual;
        const ToolDef tools[] = {
            {Tool::Select, "select", T("Select (V): click a sprite, drag a frame around several"), true},
            {Tool::Rectangle, "rect", T("Rectangle (R): drag to draw a new sprite"), manual},
            {Tool::Move, "move", T("Move (M): drag selected sprites"), true},
            {Tool::Resize, "scale", T("Resize (S): drag the handles of the selected sprite"), true},
            {Tool::Pivot, "anchor-cc", T("Pivot (P): drag the pivot point of the selected sprite"), true},
        };
        int shown = 0;
        for (const ToolDef& t : tools) shown += t.show ? 1 : 0;
        const ImVec2 p(a.x + 10.0f, a.y + 10.0f);
        const float w = shown * (btn + st.ItemSpacing.x) + st.ItemSpacing.x;
        dl->AddRectFilled(ImVec2(p.x - 4.0f, p.y - 4.0f), ImVec2(p.x + w, p.y + btn + 4.0f),
                          EditorTheme::Color32Alpha(EditorTheme::Role::Overlay, 0.92f), 8.0f);
        ImGui::SetCursorScreenPos(p);
        for (const ToolDef& t : tools) {
            if (!t.show) continue;
            ImGui::PushID((int)t.tool);
            if (EditorIcons::IconOnlyButton(t.icon, t.tip, m_tool == t.tool)) m_tool = t.tool;
            ImGui::PopID();
            ImGui::SameLine();
        }
        if (!manual && m_tool == Tool::Rectangle) m_tool = Tool::Select;
    } else {
        // В девятине — исходник или результат.
        const char* views[] = {T("Source"), T("Result")};
        int v = m_nineResult ? 1 : 0;
        ImGui::SetCursorScreenPos(ImVec2(a.x + 10.0f, a.y + 10.0f));
        if (sp::Segmented("##nine_view", views, 2, &v, 200.0f)) m_nineResult = v == 1;
    }
    const float viewW = btn * 4.0f + ImGui::CalcTextSize("1:1").x + st.FramePadding.x * 2.0f + st.ItemSpacing.x * 6.0f;
    const ImVec2 r(a.x + size.x - viewW - 10.0f, a.y + 10.0f);
    dl->AddRectFilled(ImVec2(r.x - 4.0f, r.y - 4.0f), ImVec2(r.x + viewW, r.y + btn + 4.0f),
                      EditorTheme::Color32Alpha(EditorTheme::Role::Overlay, 0.92f), 8.0f);
    ImGui::SetCursorScreenPos(r);
    float& zoom = m_mode == Mode::Sheet ? m_zoom : m_nineZoom;
    if (EditorIcons::IconOnlyButton("fit", T("Fit to view (F)"))) {
        zoom = 0.0f;
        m_userView = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("1:1", ImVec2(0.0f, btn))) {
        zoom = 1.0f;
        m_userView = true;
        if (m_mode == Mode::Sheet) m_pan = glm::vec2(0.0f);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Zoom 100%: one texture pixel is one screen pixel"));
    ImGui::SameLine();
    {
        // Приближение — одна группа kit: [−|+] значками Tabler.
        sage::editor::ictl::GroupItem z[2];
        z[0].Icon = "minus";
        z[0].Tooltip = T("Zoom out (mouse wheel)");
        z[1].Icon = "plus";
        z[1].Tooltip = T("Zoom in (mouse wheel)");
        const int hit = sage::editor::ictl::ButtonGroup("##zoom", z, 2);
        if (hit == 0) zoom = std::max(0.05f, (zoom > 0 ? zoom : 1.0f) / 1.25f);
        if (hit == 1) zoom = std::min(64.0f, (zoom > 0 ? zoom : 1.0f) * 1.25f);
        if (hit >= 0) m_userView = true;
    }
    ImGui::SameLine();
    if (EditorIcons::IconOnlyButton("grid", T("Pixel grid: shown from 8x zoom"), m_pixelGrid))
        m_pixelGrid = !m_pixelGrid;
}

// --- Мышь ----------------------------------------------------------------------

void SpriteEditor::HandleSheetMouse(ImVec2 a, ImVec2 size) {
    (void)a;
    (void)size;
    ImGuiIO& io = ImGui::GetIO();
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 mouse = io.MousePos;
    const glm::vec2 px = ToPixel(mouse);
    m_cursorPx = hovered ? px : glm::vec2(-1.0f);
    auto& regions = m_doc.Sheet.Regions;

    // Колесо — масштаб вокруг курсора: точка под мышью остаётся на месте.
    if (hovered && io.MouseWheel != 0.0f) {
        const float nz = std::clamp(m_zoom * (io.MouseWheel > 0 ? 1.2f : 1.0f / 1.2f), 0.05f, 64.0f);
        const float ox = mouse.x - px.x * nz, oy = mouse.y - px.y * nz;
        m_pan.x = ox - (m_canvasA.x + m_canvasSize.x * 0.5f - m_contentW * nz * 0.5f);
        m_pan.y = oy - (m_canvasA.y + m_canvasSize.y * 0.5f - m_contentH * nz * 0.5f);
        m_zoom = nz;
        m_userView = true;
    }
    // Сдвиг вида — средней кнопкой или с зажатым пробелом.
    const bool spacePan = ImGui::IsKeyDown(ImGuiKey_Space) && ImGui::IsItemActive();
    if (ImGui::IsItemActive() && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f) || spacePan)) {
        m_pan += glm::vec2(io.MouseDelta.x, io.MouseDelta.y);
        m_userView = true;
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        return;
    }

    m_hover = hovered && m_drag == Drag::None ? sp::HitTest(m_doc.Sheet, px) : m_hover;
    const int primary = Primary();
    // Ручки основного выбранного — у «Выбора» и «Размера».
    int handleHot = -1;
    if (hovered && m_sel.size() == 1 && primary >= 0 && primary < (int)regions.size() &&
        (m_tool == Tool::Select || m_tool == Tool::Resize) && m_drag == Drag::None) {
        const glm::ivec4 r = regions[(size_t)primary].Rect;
        const ImVec2 p0 = ToScreen(glm::vec2(r.x, r.y)), p1 = ToScreen(glm::vec2(r.x + r.z, r.y + r.w));
        for (int h = 0; h < sp::HCount; ++h) {
            const ImVec2 hp = HandlePos(p0, p1, h);
            if (std::fabs(mouse.x - hp.x) <= 6.0f && std::fabs(mouse.y - hp.y) <= 6.0f) {
                handleHot = h;
                break;
            }
        }
        if (handleHot >= 0) ImGui::SetMouseCursor(HandleCursor(handleHot));
    }

    const glm::ivec2 pi((int)std::floor(px.x + 0.5f), (int)std::floor(px.y + 0.5f));
    if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        m_dragFrom = px;
        m_dragOrig.clear();
        if (handleHot >= 0) {
            m_drag = Drag::Resize;
            m_dragHandle = handleHot;
            m_dragOrig.push_back(regions[(size_t)primary].Rect);
            Snapshot();
        } else if (m_tool == Tool::Pivot) {
            int target = primary;
            if (m_hover >= 0 && !Selected(m_hover)) {
                SelectOnly(m_hover);
                target = m_hover;
            }
            if (target >= 0) {
                m_drag = Drag::Pivot;
                Snapshot();
            }
        } else if (m_tool == Tool::Rectangle) {
            m_drag = Drag::NewRect;
            m_dragRect = sp::RectFromCorners(pi, pi, m_w, m_h);
        } else {
            const int hit = m_hover;
            if (hit >= 0) {
                if (io.KeyCtrl || io.KeyShift) {
                    if (Selected(hit)) m_sel.erase(std::find(m_sel.begin(), m_sel.end(), hit));
                    else m_sel.push_back(hit);
                } else if (!Selected(hit)) {
                    SelectOnly(hit);
                } else {
                    // Щелчок по уже выбранному делает его основным.
                    m_sel.erase(std::find(m_sel.begin(), m_sel.end(), hit));
                    m_sel.push_back(hit);
                }
                m_drag = Drag::Move;   // сдвиг начнётся, когда мышь действительно поедет
            } else if (m_tool == Tool::Select) {
                if (!io.KeyCtrl && !io.KeyShift) m_sel.clear();
                m_drag = Drag::Marquee;
                m_dragRect = sp::RectFromCorners(pi, pi, m_w, m_h);
            }
        }
        m_nameFor = -1;
    }

    if (m_drag != Drag::None && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const glm::ivec2 d((int)std::lround(px.x - m_dragFrom.x), (int)std::lround(px.y - m_dragFrom.y));
        switch (m_drag) {
        case Drag::Resize:
            if (primary >= 0 && primary < (int)regions.size())
                regions[(size_t)primary].Rect = sp::ResizeRect(m_dragOrig[0], m_dragHandle, d, m_w, m_h);
            ImGui::SetMouseCursor(HandleCursor(m_dragHandle));
            m_touched = true;
            break;
        case Drag::Pivot:
            if (primary >= 0 && primary < (int)regions.size()) {
                const glm::ivec4 r = regions[(size_t)primary].Rect;
                glm::vec2 pv((px.x - r.x) / std::max(1, r.z), (px.y - r.y) / std::max(1, r.w));
                pv = glm::clamp(pv, glm::vec2(0.0f), glm::vec2(1.0f));
                // Рядом с готовым положением — прилипнуть к нему: «низ по
                // центру» ставится точно, а не 0.49.
                for (int i = 0; i < 9; ++i)
                    if (glm::length(pv - sp::PivotPreset(i)) < 0.04f) pv = sp::PivotPreset(i);
                regions[(size_t)primary].Pivot = pv;
                m_touched = true;
            }
            break;
        case Drag::NewRect:
        case Drag::Marquee:
            m_dragRect = sp::RectFromCorners(glm::ivec2((int)std::lround(m_dragFrom.x), (int)std::lround(m_dragFrom.y)),
                                             pi, m_w, m_h);
            break;
        case Drag::Move:
            if (!m_sel.empty() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f)) {
                if (m_dragOrig.empty()) {
                    Snapshot();
                    for (int i : m_sel) m_dragOrig.push_back(regions[(size_t)i].Rect);
                }
                for (size_t k = 0; k < m_sel.size() && k < m_dragOrig.size(); ++k)
                    regions[(size_t)m_sel[k]].Rect = sp::MoveRect(m_dragOrig[k], d, m_w, m_h);
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
                m_touched = true;
            }
            break;
        default: break;
        }
    }
    if (m_drag != Drag::None && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (m_drag == Drag::NewRect && m_dragRect.z >= 1 && m_dragRect.w >= 1) {
            Snapshot();
            sage::ui::SpriteRegion reg;
            reg.Name = m_doc.Sheet.FreeName(m_doc.Sheet.Slicing.Grid.Prefix.empty() ? "sprite"
                                                                                    : m_doc.Sheet.Slicing.Grid.Prefix);
            reg.Rect = m_dragRect;
            regions.push_back(reg);
            SelectOnly((int)regions.size() - 1);
        } else if (m_drag == Drag::Marquee && m_dragRect.z > 0 && m_dragRect.w > 0) {
            for (int i : sp::HitRect(m_doc.Sheet, m_dragRect))
                if (!Selected(i)) m_sel.push_back(i);
        }
        m_drag = Drag::None;
        m_dragOrig.clear();
    }
}

// --- Рисование листа ------------------------------------------------------------

void SpriteEditor::DrawSheetCanvas(ImVec2 a, ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 i0 = ToScreen(glm::vec2(0.0f)), i1 = ToScreen(glm::vec2((float)m_w, (float)m_h));
    sp::Checkers(dl, ImVec2(std::max(i0.x, a.x), std::max(i0.y, a.y)),
                 ImVec2(std::min(i1.x, a.x + size.x), std::min(i1.y, a.y + size.y)), 10.0f);
    // V перевёрнута: у текстуры движка начало координат внизу.
    sp::Nearest(dl);
    dl->AddImage((ImTextureID)(std::intptr_t)m_tex->NativeHandle(), i0, i1, ImVec2(0, 1), ImVec2(1, 0));
    sp::Linear(dl);
    // Сетка пикселей — с крупного масштаба, только видимая часть.
    if (m_pixelGrid && m_zoom >= 8.0f) {
        const ImU32 c = IM_COL32(255, 255, 255, 26);
        const int x0 = std::max(0, (int)std::floor(ToPixel(a).x)), y0 = std::max(0, (int)std::floor(ToPixel(a).y));
        const glm::vec2 far = ToPixel(ImVec2(a.x + size.x, a.y + size.y));
        const int x1 = std::min(m_w, (int)std::ceil(far.x)), y1 = std::min(m_h, (int)std::ceil(far.y));
        for (int x = x0; x <= x1; ++x)
            dl->AddLine(ToScreen(glm::vec2(x, y0)), ToScreen(glm::vec2(x, y1)), c);
        for (int y = y0; y <= y1; ++y)
            dl->AddLine(ToScreen(glm::vec2(x0, y)), ToScreen(glm::vec2(x1, y)), c);
    }
    dl->AddRect(ImVec2(i0.x - 1, i0.y - 1), ImVec2(i1.x + 1, i1.y + 1), IM_COL32(255, 255, 255, 60));

    DrawSlicingPreview(dl);

    const auto& regions = m_doc.Sheet.Regions;
    // Сначала обычные рамки — тонко, затем наведённая и выбранные поверх.
    for (int i = 0; i < (int)regions.size(); ++i) {
        if (Selected(i) || i == m_hover) continue;
        const glm::ivec4 r = regions[(size_t)i].Rect;
        dl->AddRect(ToScreen(glm::vec2(r.x, r.y)), ToScreen(glm::vec2(r.x + r.z, r.y + r.w)), sp::FrameColor());
    }
    if (m_hover >= 0 && m_hover < (int)regions.size() && !Selected(m_hover)) {
        const glm::ivec4 r = regions[(size_t)m_hover].Rect;
        const ImVec2 p0 = ToScreen(glm::vec2(r.x, r.y)), p1 = ToScreen(glm::vec2(r.x + r.z, r.y + r.w));
        dl->AddRectFilled(p0, p1, IM_COL32(200, 235, 255, 28));
        dl->AddRect(p0, p1, sp::HoverColor(), 0.0f, 0, 1.5f);
        NameTag(dl, p0, regions[(size_t)m_hover].Name.c_str(), sp::HoverColor());
    }
    for (int i : m_sel) {
        if (i < 0 || i >= (int)regions.size()) continue;
        const sage::ui::SpriteRegion& reg = regions[(size_t)i];
        const ImVec2 p0 = ToScreen(glm::vec2(reg.Rect.x, reg.Rect.y));
        const ImVec2 p1 = ToScreen(glm::vec2(reg.Rect.x + reg.Rect.z, reg.Rect.y + reg.Rect.w));
        dl->AddRectFilled(p0, p1, EditorTheme::Color32Alpha(EditorTheme::Role::Accent, 0.10f));
        dl->AddRect(p0, p1, sp::SelectedColor(), 0.0f, 0, 2.0f);
        const bool primary = i == Primary();
        if (primary) NameTag(dl, p0, reg.Name.c_str(), sp::SelectedColor());
        // Точка опоры: кружок с перекрестьем; у основного — крупнее.
        const ImVec2 pv = ToScreen(glm::vec2(reg.Rect.x + reg.Rect.z * reg.Pivot.x, reg.Rect.y + reg.Rect.w * reg.Pivot.y));
        const float rad = primary ? 5.0f : 3.0f;
        dl->AddCircleFilled(pv, rad + 1.5f, IM_COL32(20, 20, 24, 220));
        dl->AddCircle(pv, rad, IM_COL32(255, 110, 110, 255), 0, 2.0f);
        if (primary && m_tool == Tool::Pivot) {
            dl->AddLine(ImVec2(pv.x - 10, pv.y), ImVec2(pv.x + 10, pv.y), IM_COL32(255, 110, 110, 255), 1.0f);
            dl->AddLine(ImVec2(pv.x, pv.y - 10), ImVec2(pv.x, pv.y + 10), IM_COL32(255, 110, 110, 255), 1.0f);
        }
        if (primary && m_sel.size() == 1 && (m_tool == Tool::Select || m_tool == Tool::Resize)) {
            const float hs = m_tool == Tool::Resize ? 5.0f : 3.5f;
            for (int h = 0; h < sp::HCount; ++h) {
                const ImVec2 hp = HandlePos(p0, p1, h);
                dl->AddRectFilled(ImVec2(hp.x - hs, hp.y - hs), ImVec2(hp.x + hs, hp.y + hs), IM_COL32(20, 20, 24, 255));
                dl->AddRect(ImVec2(hp.x - hs, hp.y - hs), ImVec2(hp.x + hs, hp.y + hs), sp::SelectedColor(), 0.0f, 0, 1.5f);
            }
        }
    }
    if (m_drag == Drag::NewRect || m_drag == Drag::Marquee) {
        const ImVec2 p0 = ToScreen(glm::vec2(m_dragRect.x, m_dragRect.y));
        const ImVec2 p1 = ToScreen(glm::vec2(m_dragRect.x + m_dragRect.z, m_dragRect.y + m_dragRect.w));
        const ImU32 c = m_drag == Drag::NewRect ? sp::PreviewColor() : IM_COL32(200, 235, 255, 200);
        dl->AddRectFilled(p0, p1, (c & 0x00FFFFFF) | 0x22000000);
        dl->AddRect(p0, p1, c, 0.0f, 0, 1.5f);
        if (m_drag == Drag::NewRect) {
            char buf[48];
            std::snprintf(buf, sizeof(buf), "%d x %d", m_dragRect.z, m_dragRect.w);
            NameTag(dl, ImVec2(p0.x, p1.y + ImGui::GetTextLineHeight() + 6.0f), buf, c);
        }
    }
}

void SpriteEditor::DrawSlicingPreview(ImDrawList* dl) {
    const sage::ui::SpriteSlicing& s = m_doc.Sheet.Slicing;
    if (s.Method == Method::Manual) return;
    auto same = [&](const glm::ivec4& r) {
        for (const auto& reg : m_doc.Sheet.Regions)
            if (reg.Rect == r) return true;
        return false;
    };
    if (s.Method == Method::Grid) {
        // Поля — затемнением: видно, какая часть картинки не режется.
        const glm::ivec4 m = s.Grid.Margin;
        if (m != glm::ivec4(0)) {
            const ImU32 shade = IM_COL32(0, 0, 0, 90);
            const ImVec2 i0 = ToScreen(glm::vec2(0.0f)), i1 = ToScreen(glm::vec2((float)m_w, (float)m_h));
            const ImVec2 c0 = ToScreen(glm::vec2(m.x, m.y)), c1 = ToScreen(glm::vec2(m_w - m.z, m_h - m.w));
            dl->AddRectFilled(i0, ImVec2(i1.x, c0.y), shade);
            dl->AddRectFilled(ImVec2(i0.x, c1.y), i1, shade);
            dl->AddRectFilled(ImVec2(i0.x, c0.y), ImVec2(c0.x, c1.y), shade);
            dl->AddRectFilled(ImVec2(c1.x, c0.y), ImVec2(i1.x, c1.y), shade);
        }
        for (const sage::ui::GridCell& c : GridPreview()) {
            if (same(c.Rect)) continue;
            const ImVec2 p0 = ToScreen(glm::vec2(c.Rect.x, c.Rect.y));
            const ImVec2 p1 = ToScreen(glm::vec2(c.Rect.x + c.Rect.z, c.Rect.y + c.Rect.w));
            if (!c.Inside) {
                // Клетка за краем картинки — красным: такого куска не будет.
                dl->AddRectFilled(p0, p1, IM_COL32(255, 70, 70, 45));
                dl->AddRect(p0, p1, sp::InvalidColor());
            } else if (c.Empty && s.Grid.SkipEmpty) {
                dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 40));
            } else {
                dl->AddRect(p0, p1, sp::PreviewColor());
            }
        }
        return;
    }
    // Острова: прежний результат виден, пока считается новый — приглушённым.
    const ImU32 c = m_autoRunning || !m_autoValid ? IM_COL32(150, 255, 170, 80) : sp::PreviewColor();
    for (const glm::ivec4& r : m_autoRects) {
        if (same(r)) continue;
        dl->AddRect(ToScreen(glm::vec2(r.x, r.y)), ToScreen(glm::vec2(r.x + r.z, r.y + r.w)), c);
    }
}

// --- Строка состояния --------------------------------------------------------------

void SpriteEditor::DrawStatusBar() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = ImGui::GetFrameHeight() + 6.0f;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), EditorTheme::Color32(EditorTheme::Role::SurfaceAlt));
    dl->AddLine(p, ImVec2(p.x + w, p.y), EditorTheme::Color32(EditorTheme::Role::Line));
    char buf[256];
    const float zoom = m_mode == Mode::Sheet ? m_zoom : m_nineZoom;
    buf[0] = 0;
    int n = m_tex ? std::snprintf(buf, sizeof(buf), "%s %.0f%%", T("Zoom:"), zoom * 100.0f)
                  : std::snprintf(buf, sizeof(buf), "%s", T("No texture"));
    if (m_mode == Mode::Sheet && m_cursorPx.x >= 0.0f && m_cursorPx.x < (float)m_w && m_cursorPx.y >= 0.0f &&
        m_cursorPx.y < (float)m_h)
        n += std::snprintf(buf + n, sizeof(buf) - (size_t)n, "     %s %d, %d", T("Cursor:"), (int)m_cursorPx.x,
                           (int)m_cursorPx.y);
    if (m_tex)
        n += std::snprintf(buf + n, sizeof(buf) - (size_t)n, "     %s %d\xc3\x97%d", T("Image:"), m_w, m_h);
    if (m_tex && m_mode == Mode::Sheet) {
        n += std::snprintf(buf + n, sizeof(buf) - (size_t)n, "     %s %d", T("Sprites:"),
                           (int)m_doc.Sheet.Regions.size());
        if (!m_sel.empty())
            std::snprintf(buf + n, sizeof(buf) - (size_t)n, "     %s %d", T("Selected:"), (int)m_sel.size());
    }
    const float ty = p.y + (h - ImGui::GetTextLineHeight()) * 0.5f;
    dl->AddText(ImVec2(p.x + 10.0f, ty), EditorTheme::Color32(EditorTheme::Role::TextDim), buf);
    if (m_autoRunning) {
        const char* busy = T("Analyzing the texture...");
        const ImVec2 ts = ImGui::CalcTextSize(busy);
        dl->AddText(ImVec2(p.x + w - ts.x - 10.0f, ty), EditorTheme::Color32(EditorTheme::Role::Warn), busy);
    }
    ImGui::Dummy(ImVec2(w, h));
}
