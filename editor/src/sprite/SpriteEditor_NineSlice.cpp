#include "SpriteEditor.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "imgui.h"

#include "../EditorHost.h"
#include "../EditorIcons.h"
#include "../EditorTheme.h"
#include "../InspectorControls.h"
#include "../Localization.h"
#include "../ui/UI.h"
#include "SpriteEditorDetail.h"
#include "sage/render/Texture.h"

using sage::editor::T;
namespace sp = sage::editor::sprite;
namespace ictl = sage::editor::ictl;
using sage::ui::NineSlice;
using sage::ui::SliceFill;
using sage::ui::SliceQuad;
using sage::ui::SliceRequest;

namespace {

// Цвета зон: углы не тянутся, края — по одной оси, середина — по обеим.
constexpr ImU32 kCorner = IM_COL32(255, 196, 64, 70);
constexpr ImU32 kEdge = IM_COL32(96, 176, 255, 60);
constexpr ImU32 kCenter = IM_COL32(128, 255, 160, 45);
// Линии — каждая своим цветом, как подписи L R T B в инспекторе.
constexpr ImU32 kGuides[4] = {IM_COL32(255, 120, 120, 235), IM_COL32(120, 220, 255, 235),
                              IM_COL32(255, 200, 90, 235), IM_COL32(170, 140, 255, 235)};

} // namespace

glm::ivec4 SpriteEditor::NineSource() const {
    const int i = Primary();
    if (i >= 0 && i < (int)m_doc.Sheet.Regions.size()) return m_doc.Sheet.Regions[(size_t)i].Rect;
    return {0, 0, m_w, m_h};
}

NineSlice SpriteEditor::NineGet() const {
    const int i = Primary();
    if (i >= 0 && i < (int)m_doc.Sheet.Regions.size()) return m_doc.Sheet.Regions[(size_t)i].NineSliceOf();
    return m_doc.Texture;
}

void SpriteEditor::NineSet(const NineSlice& n) {
    const int i = Primary();
    if (i >= 0 && i < (int)m_doc.Sheet.Regions.size()) {
        sage::ui::SpriteRegion& r = m_doc.Sheet.Regions[(size_t)i];
        r.Slice = n.Border();
        r.EdgeFill = n.EdgeFill;
        r.CenterFill = n.CenterFill;
        r.DrawCenter = n.DrawCenter;
    } else {
        m_doc.Texture = n;
    }
    m_touched = true;
}

// --- Исходник с линиями -------------------------------------------------------

void SpriteEditor::DrawNineCanvas(ImVec2 a, ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const glm::ivec4 src = NineSource();
    if (src.z <= 0 || src.w <= 0) return;
    NineSlice n = NineGet();
    ImGuiIO& io = ImGui::GetIO();
    if (m_nineZoom <= 0.0f)
        m_nineZoom = std::clamp(std::min(size.x * 0.7f / (float)src.z, size.y * 0.7f / (float)src.w), 0.05f, 64.0f);
    if (ImGui::IsItemHovered() && io.MouseWheel != 0.0f)
        m_nineZoom = std::clamp(m_nineZoom * (io.MouseWheel > 0 ? 1.2f : 1.0f / 1.2f), 0.05f, 64.0f);
    const float z = m_nineZoom;
    const ImVec2 o(std::floor(a.x + (size.x - (float)src.z * z) * 0.5f), std::floor(a.y + (size.y - (float)src.w * z) * 0.5f));
    const ImVec2 e(o.x + (float)src.z * z, o.y + (float)src.w * z);
    sp::Checkers(dl, o, e, 10.0f);
    const float u0 = (float)src.x / (float)m_w, u1 = (float)(src.x + src.z) / (float)m_w;
    const float v0 = 1.0f - (float)src.y / (float)m_h, v1 = 1.0f - (float)(src.y + src.w) / (float)m_h;
    sp::Nearest(dl);
    dl->AddImage((ImTextureID)(std::intptr_t)m_tex->NativeHandle(), o, e, ImVec2(u0, v0), ImVec2(u1, v1));
    sp::Linear(dl);

    // Зоны девятины.
    const float xl = o.x + n.Left * z, xr = e.x - n.Right * z, yt = o.y + n.Top * z, yb = e.y - n.Bottom * z;
    const float xs[4] = {o.x, xl, xr, e.x}, ys[4] = {o.y, yt, yb, e.y};
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col) {
            const bool corner = row != 1 && col != 1, center = row == 1 && col == 1;
            if (center && !n.DrawCenter) {
                // Без середины — штриховка: здесь будет видно то, что под элементом.
                dl->PushClipRect(ImVec2(xs[1], ys[1]), ImVec2(xs[2], ys[2]), true);
                for (float d = -(ys[2] - ys[1]); d < xs[2] - xs[1]; d += 10.0f)
                    dl->AddLine(ImVec2(xs[1] + d, ys[2]), ImVec2(xs[1] + d + (ys[2] - ys[1]), ys[1]),
                                IM_COL32(255, 255, 255, 40));
                dl->PopClipRect();
                continue;
            }
            dl->AddRectFilled(ImVec2(xs[col], ys[row]), ImVec2(xs[col + 1], ys[row + 1]),
                              corner ? kCorner : center ? kCenter : kEdge);
        }
    dl->AddRect(ImVec2(o.x - 1, o.y - 1), ImVec2(e.x + 1, e.y + 1), IM_COL32(255, 255, 255, 70));

    // Линии: наведение, захват, перетаскивание. Значение — целые пиксели
    // исходника; поля не перехлёстываются (ClampBorder).
    const ImVec2 mouse = io.MousePos;
    const float pos[4] = {xl, xr, yt, yb};
    int hot = -1;
    if (ImGui::IsItemHovered() && m_guide < 0) {
        float best = 6.0f;
        for (int g = 0; g < 4; ++g) {
            const bool vertical = g < 2;
            const float d = vertical ? std::fabs(mouse.x - pos[g]) : std::fabs(mouse.y - pos[g]);
            const bool along = vertical ? (mouse.y > o.y - 14 && mouse.y < e.y + 14) : (mouse.x > o.x - 14 && mouse.x < e.x + 14);
            if (along && d < best) {
                best = d;
                hot = g;
            }
        }
    }
    if (hot >= 0 || m_guide >= 0)
        ImGui::SetMouseCursor((hot >= 0 ? hot : m_guide) < 2 ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
    if (hot >= 0 && ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        m_guide = hot;
        Snapshot();
    }
    if (m_guide >= 0) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            m_guide = -1;
        } else {
            glm::vec4 b = n.Border();   // L T R B
            const float px = (mouse.x - o.x) / z, py = (mouse.y - o.y) / z;
            switch (m_guide) {
            case 0: b.x = std::round(px); break;
            case 1: b.z = std::round((float)src.z - px); break;
            case 2: b.y = std::round(py); break;
            default: b.w = std::round((float)src.w - py); break;
            }
            n.SetBorder(sp::ClampBorder(b, (float)src.z, (float)src.w));
            NineSet(n);
        }
    }
    static const char* const tags[4] = {"L", "R", "T", "B"};
    const float values[4] = {n.Left, n.Right, n.Top, n.Bottom};
    for (int g = 0; g < 4; ++g) {
        const bool active = g == m_guide || g == hot;
        const float th = active ? 2.5f : 1.5f;
        char label[32];
        std::snprintf(label, sizeof(label), "%s %.0f", tags[g], values[g]);
        if (g < 2) {
            dl->AddLine(ImVec2(pos[g], o.y - 16), ImVec2(pos[g], e.y + 16), kGuides[g], th);
            dl->AddText(ImVec2(pos[g] + 4, (g == 0 ? o.y - 18 - ImGui::GetTextLineHeight() : e.y + 18)), kGuides[g], label);
        } else {
            dl->AddLine(ImVec2(o.x - 16, pos[g]), ImVec2(e.x + 16, pos[g]), kGuides[g], th);
            const ImVec2 ts = ImGui::CalcTextSize(label);
            dl->AddText(ImVec2(g == 2 ? o.x - 22 - ts.x : e.x + 22, pos[g] - ts.y * 0.5f), kGuides[g], label);
        }
    }
    // Что режется — подписью снизу: кусок или вся картинка.
    char what[160];
    const int i = Primary();
    if (i >= 0) std::snprintf(what, sizeof(what), "%s %s", T("Sprite:"), m_doc.Sheet.Regions[(size_t)i].Name.c_str());
    else std::snprintf(what, sizeof(what), "%s", T("Whole texture (saved as .sage9)"));
    const ImVec2 ts = ImGui::CalcTextSize(what);
    dl->AddText(ImVec2(a.x + (size.x - ts.x) * 0.5f, a.y + size.y - ts.y - 10.0f),
                EditorTheme::Color32(EditorTheme::Role::TextDim), what);
}

// --- Результат -------------------------------------------------------------------

void SpriteEditor::DrawNineResult(ImVec2 a, ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const glm::ivec4 src = NineSource();
    if (src.z <= 0 || src.w <= 0) return;
    const NineSlice n = NineGet();
    ImGuiIO& io = ImGui::GetIO();
    const glm::vec2 minSize = glm::max(sage::ui::MinimumSize(n, 1.0f), glm::vec2(1.0f));
    m_previewSize = glm::max(m_previewSize, minSize);
    if (m_nineZoom <= 0.0f)
        m_nineZoom = std::clamp(std::min(size.x * 0.6f / m_previewSize.x, size.y * 0.6f / m_previewSize.y), 0.05f, 16.0f);
    if (ImGui::IsItemHovered() && io.MouseWheel != 0.0f)
        m_nineZoom = std::clamp(m_nineZoom * (io.MouseWheel > 0 ? 1.2f : 1.0f / 1.2f), 0.05f, 64.0f);
    const float z = m_nineZoom;
    const ImVec2 box(m_previewSize.x * z, m_previewSize.y * z);
    const ImVec2 o(std::floor(a.x + (size.x - box.x) * 0.5f), std::floor(a.y + (size.y - box.y) * 0.5f));
    const ImVec2 e(o.x + box.x, o.y + box.y);
    sp::Checkers(dl, o, e, 10.0f);

    // Тем же решателем, что и в игре (sage::ui::Solve): предпросмотр по своим
    // правилам был бы вторым источником правды. Плитка — SolveTiles на кусок.
    const float tw = (float)m_w, th = (float)m_h;
    SliceRequest req;
    req.SrcX = (float)src.x;
    req.SrcY = (float)src.y;
    req.SrcW = (float)src.z;
    req.SrcH = (float)src.w;
    req.DstX = o.x;
    req.DstY = o.y;
    req.DstW = box.x;
    req.DstH = box.y;
    req.Scale = z;
    const ImTextureID id = (ImTextureID)(std::intptr_t)m_tex->NativeHandle();
    auto draw = [&](const SliceQuad& q) {
        const ImVec2 uv0(q.SrcX / tw, 1.0f - q.SrcY / th);
        const ImVec2 uv1((q.SrcX + q.SrcW) / tw, 1.0f - (q.SrcY + q.SrcH) / th);
        dl->AddImage(id, {q.DstX, q.DstY}, {q.DstX + q.DstW, q.DstY + q.DstH}, uv0, uv1);
    };
    dl->PushClipRect(o, e, true);
    sp::Nearest(dl);
    for (const SliceQuad& q : sage::ui::Solve(n, req)) {
        const bool midX = q.SrcX >= req.SrcX + n.Left - 0.01f && q.SrcX + q.SrcW <= req.SrcX + req.SrcW - n.Right + 0.01f &&
                          n.Left + n.Right < req.SrcW;
        const bool midY = q.SrcY >= req.SrcY + n.Top - 0.01f && q.SrcY + q.SrcH <= req.SrcY + req.SrcH - n.Bottom + 0.01f &&
                          n.Top + n.Bottom < req.SrcH;
        const bool center = midX && midY, edge = midX != midY;
        const SliceFill fill = center ? n.CenterFill : n.EdgeFill;
        if ((center || edge) && fill == SliceFill::Tile) {
            SliceRequest t;
            t.SrcX = q.SrcX; t.SrcY = q.SrcY; t.SrcW = q.SrcW; t.SrcH = q.SrcH;
            t.DstX = q.DstX; t.DstY = q.DstY; t.DstW = q.DstW; t.DstH = q.DstH;
            // По оси, вдоль которой край не тянется, плитка — во весь кусок.
            const float tileW = midX ? q.SrcW * z : q.DstW, tileH = midY ? q.SrcH * z : q.DstH;
            for (const SliceQuad& tq : sage::ui::SolveTiles(t, tileW, tileH)) draw(tq);
        } else {
            draw(q);
        }
    }
    sp::Linear(dl);
    dl->PopClipRect();
    dl->AddRect(ImVec2(o.x - 1, o.y - 1), ImVec2(e.x + 1, e.y + 1), IM_COL32(255, 255, 255, 70));

    // Угол-ручка: размер предпросмотра тянется прямо на холсте.
    const ImVec2 hc(e.x, e.y);
    const bool hotCorner = std::fabs(io.MousePos.x - hc.x) < 8 && std::fabs(io.MousePos.y - hc.y) < 8;
    if (hotCorner && ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) m_drag = Drag::PreviewSize;
    if (m_drag == Drag::PreviewSize) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            m_drag = Drag::None;
        } else {
            // Ящик стоит по центру: угол уходит на delta — сторона на 2·delta.
            m_previewSize.x = std::max(minSize.x, m_previewSize.x + io.MouseDelta.x * 2.0f / z);
            m_previewSize.y = std::max(minSize.y, m_previewSize.y + io.MouseDelta.y * 2.0f / z);
        }
    }
    if (hotCorner || m_drag == Drag::PreviewSize) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
    dl->AddRectFilled(ImVec2(hc.x - 5, hc.y - 5), ImVec2(hc.x + 5, hc.y + 5), sp::SelectedColor(), 2.0f);
    char label[64];
    std::snprintf(label, sizeof(label), "%.0f \xc3\x97 %.0f", m_previewSize.x, m_previewSize.y);
    // Под ящиком, по правому краю: справа от него на узком холсте места нет.
    const ImVec2 ls = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(e.x - ls.x, e.y + 10), EditorTheme::Color32(EditorTheme::Role::TextDim), label);
    const char* note = T("Preview only: the sprite itself does not change size");
    const ImVec2 ts = ImGui::CalcTextSize(note);
    dl->AddText(ImVec2(a.x + (size.x - ts.x) * 0.5f, a.y + size.y - ts.y - 10.0f),
                EditorTheme::Color32(EditorTheme::Role::TextDim), note);
}

// --- Инспектор девятины -------------------------------------------------------------

void SpriteEditor::DrawNineInspector(EditorHost& host) {
    if (!m_tex || !m_pixels) {
        ImGui::TextDisabled("%s", T("Open a texture first."));
        return;
    }
    const auto& regions = m_doc.Sheet.Regions;
    if (ictl::BeginSection("sprite.nine.target", T("Target"))) {
        // Что режется: кусок листа или вся картинка (без кусков — .sage9).
        const int cur = Primary();
        const std::string shown = cur >= 0 ? regions[(size_t)cur].Name : std::string(T("Whole texture"));
        ictl::Label(T("Sprite"));
        if (ImGui::BeginCombo("##nine_target", shown.c_str())) {
            if (ImGui::Selectable(T("Whole texture"), cur < 0)) {
                m_sel.clear();
                m_nineZoom = 0.0f;
            }
            for (int i = 0; i < (int)regions.size(); ++i) {
                ImGui::PushID(i);
                if (ImGui::Selectable(regions[(size_t)i].Name.c_str(), i == cur)) {
                    SelectOnly(i);
                    m_nineZoom = 0.0f;
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        const glm::ivec4 s = NineSource();
        char size[64];
        std::snprintf(size, sizeof(size), "%d \xc3\x97 %d %s", s.z, s.w, T("pixels"));
        ImGui::TextDisabled("%s", size);
        ictl::EndSection();
    }
    NineSlice n = NineGet();
    const glm::ivec4 src = NineSource();
    if (ictl::BeginSection("sprite.nine.borders", T("Nine-Slice"))) {
        ImGui::BeginGroup();
        ictl::GroupTitle(T("Borders"));
        ictl::NumOpts o;
        o.Min = 0.0f;
        o.Max = (float)std::max(src.z, src.w);
        glm::vec4 b = n.Border();   // L T R B
        bool ch = ictl::Pair(host, "lr", &b.x, &b.z, "L", "R", o);
        ch |= ictl::Pair(host, "tb", &b.y, &b.w, "T", "B", o);
        ImGui::EndGroup();
        sp::ParamTip(T("Borders"), T("Fixed edges that never stretch, in pixels of the texture. The lines on the "
                                     "canvas move them too."),
                     T("8 on every side — the 8-pixel corners stay sharp at any size."));
        if (ch) {
            n.SetBorder(sp::ClampBorder(glm::round(b), (float)src.z, (float)src.w));
            NineSet(n);
        }
        const char* fills[] = {T("Stretch"), T("Tile")};
        ImGui::BeginGroup();
        ictl::Label(T("Edges Fill"));
        int ef = (int)n.EdgeFill;
        if (sp::Segmented("##edge", fills, 2, &ef)) {
            Snapshot();
            n.EdgeFill = (SliceFill)ef;
            NineSet(n);
        }
        ImGui::EndGroup();
        sp::ParamTip(T("Edges Fill"), T("Stretch — the edge piece is stretched. Tile — it repeats at its own size."),
                     T("Tile for a patterned or pixel-art border."));
        ImGui::BeginGroup();
        ictl::Label(T("Center Fill"));
        int cf = (int)n.CenterFill;
        if (sp::Segmented("##center", fills, 2, &cf)) {
            Snapshot();
            n.CenterFill = (SliceFill)cf;
            NineSet(n);
        }
        ImGui::EndGroup();
        sp::ParamTip(T("Center Fill"), T("How the middle fills the inside: stretched or repeated."),
                     T("Tile for a brick or fabric texture."));
        ImGui::BeginGroup();
        if (ictl::Switch(host, T("Draw Center"), &n.DrawCenter)) NineSet(n);
        ImGui::EndGroup();
        sp::ParamTip(T("Draw Center"), T("Off — only the frame is drawn, the middle stays transparent."),
                     T("Off for an outline around a portrait."));
        ImGui::Spacing();
        if (EditorIcons::Button("refresh", T("Reset Borders"), T("All borders to zero: the sprite stretches whole"))) {
            Snapshot();
            n.SetBorder(glm::vec4(0.0f));
            NineSet(n);
        }
        ImGui::SameLine();
        if (EditorIcons::Button("eyedropper", T("Guess"), T("Find the borders from the picture: rows and columns "
                                                            "that do not change"))) {
            std::vector<unsigned char> part((size_t)src.z * src.w * 4);
            for (int y = 0; y < src.w; ++y)
                std::copy_n(m_pixels->data() + ((size_t)(src.y + y) * m_w + src.x) * 4, (size_t)src.z * 4,
                            part.data() + (size_t)y * src.z * 4);
            NineSlice guess;
            if (sage::ui::GuessBorder(part.data(), src.z, src.w, guess) && !guess.Empty()) {
                Snapshot();
                n.SetBorder(sp::ClampBorder(guess.Border(), (float)src.z, (float)src.w));
                NineSet(n);
            } else {
                SetStatus(T("Could not guess the borders from this picture"), true);
            }
        }
        ictl::EndSection();
    }
    if (ictl::BeginSection("sprite.nine.preview", T("Preview"))) {
        const char* views[] = {T("Source"), T("Result")};
        int v = m_nineResult ? 1 : 0;
        ImGui::BeginGroup();
        ictl::Label(T("View"));
        if (sp::Segmented("##view", views, 2, &v)) {
            m_nineResult = v == 1;
            m_nineZoom = 0.0f;
        }
        ImGui::EndGroup();
        sp::ParamTip(T("View"), T("Source — the sprite with its lines. Result — how it looks stretched to the preview size."),
                     nullptr);
        ImGui::BeginGroup();
        ictl::GroupTitle(T("Preview Size"));
        // Размер предпросмотра — не правка документа: в историю не идёт.
        ictl::UndoScope none([] {});
        const glm::vec2 lo = glm::max(sage::ui::MinimumSize(n, 1.0f), glm::vec2(1.0f));
        ictl::NumOpts o;
        o.Min = std::min(lo.x, lo.y);
        o.Max = 8192.0f;
        if (ictl::Pair(host, "psize", &m_previewSize.x, &m_previewSize.y, "W", "H", o))
            m_previewSize = glm::max(m_previewSize, lo);
        ImGui::EndGroup();
        sp::ParamTip(T("Preview Size"), T("Size of the result preview. It does not change the sprite; the corner on the "
                                          "canvas drags it too."),
                     T("192 × 96 — a wide button."));
        ictl::EndSection();
    }
}
