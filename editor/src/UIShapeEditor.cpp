#include "UIShapeEditor.h"

#include <algorithm>
#include <cmath>

#include "EditorHost.h"
#include "EditorIcons.h"
#include "Localization.h"
#include "ui/UI.h"

#include "sage/scene/Scene.h"
#include "sage/ui/ShapePath.h"
#include "sage/ui/components/Visual.h"

namespace ui = sage::ui;

namespace {

constexpr float kPointGrab = 7.0f;     // радиус захвата точки, пикселей панели
constexpr float kLineGrab = 6.0f;

float DistToSegment(ImVec2 p, ImVec2 a, ImVec2 b, float* tOut = nullptr) {
    const glm::vec2 P(p.x, p.y), A(a.x, a.y), B(b.x, b.y);
    const glm::vec2 d = B - A;
    const float len2 = glm::dot(d, d);
    const float t = len2 > 0.0f ? std::clamp(glm::dot(P - A, d) / len2, 0.0f, 1.0f) : 0.0f;
    if (tOut) *tOut = t;
    return glm::length(P - (A + d * t));
}

float Dist(ImVec2 a, ImVec2 b) { return std::hypot(a.x - b.x, a.y - b.y); }

} // namespace

bool UIShapeEditor::Draw(EditorHost& host, Scene& scene, ImDrawList* dl, entt::entity element,
                         const ui::UIRect& rect, const std::function<ImVec2(float, float)>& toScreen,
                         const std::function<glm::vec2(ImVec2)>& toUI, ImVec2 mouse, bool hovered) {
    entt::registry& reg = scene.Registry();
    ui::Shape* shape = reg.try_get<ui::Shape>(element);
    if (!shape || shape->Form != ui::Shape::Geometry::Custom || rect.w <= 0.0f || rect.h <= 0.0f) {
        m_grab = Grab::None;
        return false;
    }
    if (element != m_entity) {
        m_entity = element;
        m_contour = m_point = -1;
        m_grab = Grab::None;
    }

    std::vector<ui::PathContour> contours;
    ui::ParseShapePath(shape->Path, contours);
    // Пустой путь — весь прямоугольник: править его можно сразу.
    if (contours.empty()) contours.push_back({{{{0, 0}}, {{1, 0}}, {{1, 1}}, {{0, 1}}}, true});

    auto toPanel = [&](glm::vec2 n) { return toScreen(rect.x + n.x * rect.w, rect.y + n.y * rect.h); };
    auto toNorm = [&](ImVec2 p) {
        const glm::vec2 u = toUI(p);
        return glm::vec2((u.x - rect.x) / rect.w, (u.y - rect.y) / rect.h);
    };
    auto commit = [&](const std::vector<ui::PathContour>& c) {
        if (!m_pushedUndo) {
            host.PushUndoSnapshot();
            m_pushedUndo = true;
        }
        shape->Path = ui::FormatShapePath(c);
    };

    // --- Рисунок: линии контура, ручки выбранной точки, точки -----------------
    const ImU32 lineCol = IM_COL32(255, 196, 80, 220);
    for (const ui::Polyline& line : ui::FlattenShapePath(contours, rect)) {
        std::vector<ImVec2> pts;
        for (glm::vec2 p : line.Points) pts.push_back(toScreen(p.x, p.y));
        if (pts.size() >= 2) dl->AddPolyline(pts.data(), (int)pts.size(), lineCol, line.Closed ? ImDrawFlags_Closed : 0, 1.5f);
    }
    struct Hit { int C = -1, P = -1; Grab G = Grab::None; };
    Hit under;
    for (int c = 0; c < (int)contours.size(); ++c) {
        for (int p = 0; p < (int)contours[c].Points.size(); ++p) {
            const ui::PathPoint& pt = contours[c].Points[(size_t)p];
            const ImVec2 at = toPanel(pt.P);
            const bool selected = c == m_contour && p == m_point;
            // Ручки — у выбранной точки и у всех кривых: кривую без ручек не
            // поправить, а ручки прямых углов только мешали бы.
            if (selected || pt.Curved()) {
                for (Grab g : {Grab::In, Grab::Out}) {
                    const glm::vec2 off = g == Grab::In ? pt.In : pt.Out;
                    if (off == glm::vec2(0.0f)) continue;
                    const ImVec2 h = toPanel(pt.P + off);
                    dl->AddLine(at, h, IM_COL32(160, 200, 255, 200), 1.0f);
                    dl->AddCircleFilled(h, 4.0f, IM_COL32(160, 200, 255, 255));
                    if (Dist(mouse, h) <= kPointGrab && under.C < 0) under = {c, p, g};
                }
            }
            dl->AddRectFilled(ImVec2(at.x - 4, at.y - 4), ImVec2(at.x + 4, at.y + 4),
                              selected ? IM_COL32(255, 196, 80, 255) : IM_COL32(30, 30, 36, 255));
            dl->AddRect(ImVec2(at.x - 4, at.y - 4), ImVec2(at.x + 4, at.y + 4), lineCol);
            if (Dist(mouse, at) <= kPointGrab && (under.C < 0 || under.G != Grab::Point)) under = {c, p, Grab::Point};
        }
    }

    // --- Перетаскивание --------------------------------------------------------
    if (m_grab != Grab::None) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            m_grab = Grab::None;
            m_pushedUndo = false;
            return true;
        }
        if (m_contour < 0 || m_contour >= (int)contours.size() ||
            m_point >= (int)contours[(size_t)m_contour].Points.size()) {
            m_grab = Grab::None;
            return true;
        }
        ui::PathPoint& pt = contours[(size_t)m_contour].Points[(size_t)m_point];
        const glm::vec2 n = toNorm(mouse);
        if (m_grab == Grab::Point) {
            pt.P = n;
        } else {
            // Ручки зеркальны (гладкая кривая), Alt — только эта ручка.
            glm::vec2& mine = m_grab == Grab::In ? pt.In : pt.Out;
            glm::vec2& other = m_grab == Grab::In ? pt.Out : pt.In;
            mine = n - pt.P;
            if (!ImGui::GetIO().KeyAlt) other = -mine;
        }
        commit(contours);
        return true;
    }

    // --- Меню точки (правая кнопка) ------------------------------------------
    bool menuOpen = false;
    if (Sage::UI::MenuScope menu; ImGui::BeginPopup("##ui_shape_point")) {
        menuOpen = true;
        const int c = m_menuContour, p = m_menuPoint;
        if (c >= 0 && c < (int)contours.size() && p >= 0 && p < (int)contours[(size_t)c].Points.size()) {
            ui::PathContour& con = contours[(size_t)c];
            ui::PathPoint& pt = con.Points[(size_t)p];
            if (EditorIcons::MenuItem("wire", pt.Curved() ? T("Make a corner") : T("Make a curve"))) {
                if (pt.Curved()) {
                    pt.In = pt.Out = glm::vec2(0.0f);
                } else {
                    // Ручки вдоль хода соседей: кривая сразу гладкая.
                    const size_t n = con.Points.size();
                    const glm::vec2 prev = con.Points[(size_t)(p + (int)n - 1) % n].P;
                    const glm::vec2 next = con.Points[(size_t)(p + 1) % n].P;
                    const glm::vec2 dir = (next - prev) * 0.25f;
                    pt.Out = dir;
                    pt.In = -dir;
                }
                commit(contours);
            }
            if (EditorIcons::MenuItem("trash", T("Delete point"), "Del",
                                      con.Points.size() > (con.Closed ? 3u : 2u))) {
                con.Points.erase(con.Points.begin() + p);
                m_point = -1;
                commit(contours);
            }
            if (EditorIcons::MenuItem("rect", con.Closed ? T("Open contour") : T("Close contour"))) {
                con.Closed = !con.Closed;
                commit(contours);
            }
            if (EditorIcons::MenuItem("trash", T("Delete contour"), nullptr, contours.size() > 1)) {
                contours.erase(contours.begin() + c);
                m_contour = m_point = -1;
                commit(contours);
            }
        }
        ImGui::EndPopup();
    }
    m_pushedUndo = false;
    if (menuOpen || !hovered) return false;

    // Delete — выбранную точку (контуру оставляем минимум).
    if (m_contour >= 0 && m_contour < (int)contours.size() && m_point >= 0 && !ImGui::GetIO().WantTextInput &&
        (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false))) {
        ui::PathContour& con = contours[(size_t)m_contour];
        if (m_point < (int)con.Points.size() && con.Points.size() > (con.Closed ? 3u : 2u)) {
            con.Points.erase(con.Points.begin() + m_point);
            m_point = -1;
            commit(contours);
            m_pushedUndo = false;
            return true;
        }
    }

    if (under.C >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && under.G == Grab::Point) {
        m_menuContour = under.C;
        m_menuPoint = under.P;
        m_contour = under.C;
        m_point = under.P;
        ImGui::OpenPopup("##ui_shape_point");
        return true;
    }
    if (under.C >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        m_contour = under.C;
        m_point = under.P;
        m_grab = under.G;
        m_pushedUndo = false;
        return true;
    }
    // Двойной щелчок по линии — новая точка там, где щёлкнули.
    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        for (int c = 0; c < (int)contours.size(); ++c) {
            ui::PathContour& con = contours[(size_t)c];
            const size_t n = con.Points.size();
            const size_t segs = con.Closed ? n : n - 1;
            for (size_t i = 0; i < segs; ++i) {
                if (DistToSegment(mouse, toPanel(con.Points[i].P), toPanel(con.Points[(i + 1) % n].P)) > kLineGrab)
                    continue;
                ui::PathPoint fresh;
                fresh.P = toNorm(mouse);
                con.Points.insert(con.Points.begin() + (std::ptrdiff_t)i + 1, fresh);
                m_contour = c;
                m_point = (int)i + 1;
                commit(contours);
                m_pushedUndo = false;
                return true;
            }
        }
    }
    return under.C >= 0;   // над точкой — холст не начинает своё
}
