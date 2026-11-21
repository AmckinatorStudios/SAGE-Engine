// Холст Animation Graph: узлы, провода, масштаб и перемещение, контекстное
// меню создания. Узлы компактные — заголовок и строка на вход; всё, что
// настраивается, — в инспекторе справа.
#include "AnimGraphPanel.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

#include "../EditorIcons.h"
#include "../EditorTheme.h"
#include "../Localization.h"
#include "../ui/UI.h"
#include "imgui_internal.h"
#include "sage/core/Paths.h"

using namespace sage::anim;

namespace {
constexpr float kHeader = 26.0f;   // высота шапки узла (единицы холста)
constexpr float kRow = 22.0f;      // строка входа
constexpr float kPinR = 5.5f;
constexpr const char* kCanvasMenu = "###AnimGraphCanvasMenu";

ImU32 PinColor(AnimPinType t) {
    return t == AnimPinType::Float ? EditorTheme::Color32(EditorTheme::Role::Info)
                                   : EditorTheme::Color32(EditorTheme::Role::Accent);
}

// Цвет полоски шапки — вид узла узнаётся издалека, не читая заголовок.
ImU32 KindColor(AnimNodeKind k) {
    switch (k) {
        case AnimNodeKind::Clip: return EditorTheme::Color32(EditorTheme::Role::Accent);
        case AnimNodeKind::Blend: return EditorTheme::Color32(EditorTheme::Role::Ok);
        case AnimNodeKind::Float: return EditorTheme::Color32(EditorTheme::Role::Info);
        case AnimNodeKind::Output: return EditorTheme::Color32(EditorTheme::Role::Danger);
    }
    return EditorTheme::Color32(EditorTheme::Role::Line);
}

float Dist2(const ImVec2& a, const ImVec2& b) {
    const float dx = a.x - b.x, dy = a.y - b.y;
    return dx * dx + dy * dy;
}

void Wire(ImDrawList* dl, const ImVec2& a, const ImVec2& b, ImU32 color, float thickness) {
    const float dx = std::max(40.0f, std::fabs(b.x - a.x) * 0.5f);
    dl->AddBezierCubic(a, ImVec2(a.x + dx, a.y), ImVec2(b.x - dx, b.y), b, color, thickness);
}

const char* InputLabel(AnimNodeKind k, int pin) {
    if (k == AnimNodeKind::Output) return T("Pose");
    if (k == AnimNodeKind::Blend) return pin == kBlendPinA ? "A" : pin == kBlendPinB ? "B" : T("Weight");
    return "";
}
} // namespace

ImVec2 AnimGraphPanel::ToScreen(const glm::vec2& p) const {
    return ImVec2(m_origin.x + (p.x + m_pan.x) * m_zoom, m_origin.y + (p.y + m_pan.y) * m_zoom);
}

glm::vec2 AnimGraphPanel::ToCanvas(const ImVec2& s) const {
    return glm::vec2((s.x - m_origin.x) / m_zoom - m_pan.x, (s.y - m_origin.y) / m_zoom - m_pan.y);
}

ImVec2 AnimGraphPanel::NodeSize(const AnimGraphNode& n) const {
    const int rows = std::max(1, AnimGraph::InputCount(n.Kind));
    const float w = n.Kind == AnimNodeKind::Clip ? 180.0f : 150.0f;
    return ImVec2(w, kHeader + kRow * (float)rows + 6.0f);
}

ImVec2 AnimGraphPanel::InputPinPos(const AnimGraphNode& n, int pin) const {
    return ToScreen(glm::vec2(n.Pos.x, n.Pos.y + kHeader + kRow * ((float)pin + 0.5f)));
}

ImVec2 AnimGraphPanel::OutputPinPos(const AnimGraphNode& n) const {
    return ToScreen(glm::vec2(n.Pos.x + NodeSize(n).x, n.Pos.y + kHeader + kRow * 0.5f));
}

std::string AnimGraphPanel::NodeTitle(const AnimGraphNode& n) const {
    switch (n.Kind) {
        case AnimNodeKind::Clip: return T("Animation Clip");
        case AnimNodeKind::Blend: return T("Blend");
        case AnimNodeKind::Float: return T("Float Parameter");
        case AnimNodeKind::Output: return T("Output");
    }
    return {};
}

std::string AnimGraphPanel::NodeDetail(const AnimGraphNode& n) const {
    switch (n.Kind) {
        case AnimNodeKind::Clip: {
            std::string s = !n.ClipPath.empty()
                                ? sage::PathToUtf8(std::filesystem::path(sage::PathFromUtf8(n.ClipPath)).stem())
                                : !n.ClipName.empty() ? n.ClipName : std::string(T("no clip"));
            if (!n.Loop) s += "  ·  " + std::string(T("once"));
            return s;
        }
        case AnimNodeKind::Float: return n.Param.empty() ? std::string(T("no parameter")) : n.Param;
        default: return {};
    }
}

void AnimGraphPanel::AddNodeAt(AnimNodeKind kind, const glm::vec2& at) {
    PushUndo();
    const int id = m_graph.Add(kind, at);
    if (!id) {
        m_undo.pop_back();
        return;
    }
    AnimGraphNode* n = m_graph.Find(id);
    // Новый параметр-узел сразу смотрит на первый параметр графа: чаще всего
    // он и нужен, а пустой узел ничего не значит.
    if (kind == AnimNodeKind::Float && !m_graph.Params.empty()) n->Param = m_graph.Params.front().Name;
    m_selected = id;
    Changed();
}

void AnimGraphPanel::DeleteSelected() {
    const AnimGraphNode* n = m_graph.Find(m_selected);
    if (!n || n->Kind == AnimNodeKind::Output) return;
    PushUndo();
    m_graph.Remove(m_selected);
    m_selected = 0;
    Changed();
}

void AnimGraphPanel::FrameAll() {
    if (m_graph.Nodes.empty() || m_canvasSize.x <= 0.0f) return;
    glm::vec2 lo(1e9f), hi(-1e9f);
    for (const AnimGraphNode& n : m_graph.Nodes) {
        const ImVec2 s = NodeSize(n);
        lo = glm::min(lo, n.Pos);
        hi = glm::max(hi, n.Pos + glm::vec2(s.x, s.y));
    }
    const glm::vec2 size = hi - lo + glm::vec2(80.0f);
    m_zoom = std::clamp(std::min(m_canvasSize.x / size.x, m_canvasSize.y / size.y), 0.3f, 1.25f);
    const glm::vec2 view(m_canvasSize.x / m_zoom, m_canvasSize.y / m_zoom);
    m_pan = -lo + (view - (hi - lo)) * 0.5f;
}

void AnimGraphPanel::DrawCanvas(EditorHost& host) {
    (void)host;
    ImGuiIO& io = ImGui::GetIO();
    m_origin = ImGui::GetCursorScreenPos();
    m_canvasSize = ImGui::GetContentRegionAvail();
    if (m_canvasSize.x < 50.0f || m_canvasSize.y < 50.0f) return;
    if (m_frameRequest) {
        FrameAll();
        m_frameRequest = false;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 end(m_origin.x + m_canvasSize.x, m_origin.y + m_canvasSize.y);

    ImGui::InvisibleButton("##ag_canvas_area", m_canvasSize,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();

    // Фон и сетка.
    dl->PushClipRect(m_origin, end, true);
    dl->AddRectFilled(m_origin, end, EditorTheme::Color32(EditorTheme::Role::Bg));
    const float grid = 32.0f * m_zoom;
    const ImU32 gridCol = EditorTheme::Color32Alpha(EditorTheme::Role::Line, 0.45f);
    for (float x = std::fmod(m_pan.x * m_zoom, grid); x < m_canvasSize.x; x += grid)
        dl->AddLine(ImVec2(m_origin.x + x, m_origin.y), ImVec2(m_origin.x + x, end.y), gridCol);
    for (float y = std::fmod(m_pan.y * m_zoom, grid); y < m_canvasSize.y; y += grid)
        dl->AddLine(ImVec2(m_origin.x, m_origin.y + y), ImVec2(end.x, m_origin.y + y), gridCol);

    // Масштаб колесом — вокруг курсора: точка под мышью остаётся на месте.
    if (hovered && io.MouseWheel != 0.0f) {
        const glm::vec2 before = ToCanvas(io.MousePos);
        m_zoom = std::clamp(m_zoom * (1.0f + 0.1f * io.MouseWheel), 0.25f, 2.0f);
        const glm::vec2 after = ToCanvas(io.MousePos);
        m_pan += after - before;
    }
    // Перемещение — средней или правой кнопкой (правая без сдвига — меню).
    if (active && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
                   ImGui::IsMouseDragging(ImGuiMouseButton_Right, 4.0f))) {
        m_pan.x += io.MouseDelta.x / m_zoom;
        m_pan.y += io.MouseDelta.y / m_zoom;
    }

    const float fontSize = ImGui::GetFontSize() * std::clamp(m_zoom, 0.6f, 1.4f);
    ImFont* font = ImGui::GetFont();
    const AnimGraphInstance* live = PreviewInstance();

    // Провода — под узлами.
    for (const AnimGraphLink& l : m_graph.Links) {
        const AnimGraphNode* a = m_graph.Find(l.From);
        const AnimGraphNode* b = m_graph.Find(l.To);
        if (!a || !b) continue;
        Wire(dl, OutputPinPos(*a), InputPinPos(*b, l.Pin), PinColor(AnimGraph::OutputType(a->Kind)), 2.2f * m_zoom);
    }

    // Узлы.
    int hoverNode = 0, hoverOut = 0, hoverInNode = 0, hoverInPin = -1;
    const float pinHit = std::max(8.0f, 9.0f * m_zoom);
    for (const AnimGraphNode& n : m_graph.Nodes) {
        const ImVec2 a = ToScreen(n.Pos);
        const ImVec2 sz = NodeSize(n);
        const ImVec2 b(a.x + sz.x * m_zoom, a.y + sz.y * m_zoom);
        const float round = 6.0f * m_zoom;
        dl->AddRectFilled(a, b, EditorTheme::Color32(EditorTheme::Role::SurfaceAlt), round);
        dl->AddRectFilled(a, ImVec2(b.x, a.y + kHeader * m_zoom), EditorTheme::Color32(EditorTheme::Role::Elevated), round,
                          ImDrawFlags_RoundCornersTop);
        dl->AddRectFilled(a, ImVec2(a.x + 4.0f * m_zoom, a.y + kHeader * m_zoom), KindColor(n.Kind), round,
                          ImDrawFlags_RoundCornersTopLeft);
        const bool sel = n.Id == m_selected;
        dl->AddRect(a, b, sel ? EditorTheme::Color32(EditorTheme::Role::Accent) : EditorTheme::Color32(EditorTheme::Role::LineStrong),
                    round, 0, sel ? 2.0f : 1.0f);

        // Шапка: заголовок; у клипа и параметра — ещё и что выбрано.
        const ImVec4 clip(a.x + 4.0f, a.y, b.x - 14.0f * m_zoom, b.y);
        dl->AddText(font, fontSize, ImVec2(a.x + 10.0f * m_zoom, a.y + (kHeader * m_zoom - fontSize) * 0.5f),
                    EditorTheme::Color32(EditorTheme::Role::Text), NodeTitle(n).c_str(), nullptr, 0.0f, &clip);
        const std::string detail = NodeDetail(n);
        if (!detail.empty()) {
            const float y = a.y + (kHeader + kRow * 0.5f) * m_zoom - fontSize * 0.5f;
            dl->AddText(font, fontSize, ImVec2(a.x + 10.0f * m_zoom, y), EditorTheme::Color32(EditorTheme::Role::TextDim),
                        detail.c_str(), nullptr, 0.0f, &clip);
        }

        // Входы.
        for (int pin = 0; pin < AnimGraph::InputCount(n.Kind); ++pin) {
            const ImVec2 p = InputPinPos(n, pin);
            const AnimPinType t = AnimGraph::InputType(n.Kind, pin);
            const bool linked = m_graph.LinkInto(n.Id, pin) != nullptr;
            if (linked) dl->AddCircleFilled(p, kPinR * m_zoom, PinColor(t));
            else dl->AddCircle(p, kPinR * m_zoom, PinColor(t), 0, 1.8f);
            std::string label = InputLabel(n.Kind, pin);
            // Вес без провода — число самого узла; с проводом — что пришло сейчас.
            if (n.Kind == AnimNodeKind::Blend && pin == kBlendPinWeight) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "  %.2f", live && linked ? live->BlendWeight(n.Id) : n.Weight);
                label += buf;
            }
            dl->AddText(font, fontSize, ImVec2(p.x + 10.0f * m_zoom, p.y - fontSize * 0.5f),
                        EditorTheme::Color32(EditorTheme::Role::TextDim), label.c_str());
            if (hovered && Dist2(io.MousePos, p) < pinHit * pinHit) {
                hoverInNode = n.Id;
                hoverInPin = pin;
            }
        }
        // Выход.
        const AnimPinType outT = AnimGraph::OutputType(n.Kind);
        if (outT != AnimPinType::None) {
            const ImVec2 p = OutputPinPos(n);
            dl->AddCircleFilled(p, kPinR * m_zoom, PinColor(outT));
            if (hovered && Dist2(io.MousePos, p) < pinHit * pinHit) hoverOut = n.Id;
        }
        if (hovered && io.MousePos.x >= a.x && io.MousePos.x <= b.x && io.MousePos.y >= a.y && io.MousePos.y <= b.y)
            hoverNode = n.Id;
    }

    // Провод, который тянут сейчас.
    if (m_linkFrom) {
        if (const AnimGraphNode* a = m_graph.Find(m_linkFrom))
            Wire(dl, OutputPinPos(*a), io.MousePos, PinColor(AnimGraph::OutputType(a->Kind)), 2.0f * m_zoom);
    } else if (m_linkToNode) {
        if (const AnimGraphNode* b = m_graph.Find(m_linkToNode))
            Wire(dl, io.MousePos, InputPinPos(*b, m_linkToPin), PinColor(AnimGraph::InputType(b->Kind, m_linkToPin)),
                 2.0f * m_zoom);
    }

    // --- Мышь -----------------------------------------------------------------
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (hoverOut) {
            m_linkFrom = hoverOut;
        } else if (hoverInNode) {
            // Вход с проводом — провод снимается и «висит» на мыши от прежнего
            // источника: так провод переносят на другой вход одним движением.
            if (const AnimGraphLink* l = m_graph.LinkInto(hoverInNode, hoverInPin)) {
                const int from = l->From;
                PushUndo();
                m_graph.Disconnect(hoverInNode, hoverInPin);
                Changed();
                m_linkFrom = from;
            } else {
                m_linkToNode = hoverInNode;
                m_linkToPin = hoverInPin;
            }
        } else if (hoverNode) {
            m_selected = hoverNode;
            m_dragNode = hoverNode;
            m_dragMoved = false;
        } else {
            m_selected = 0;
        }
    }
    if (m_dragNode && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)) {
        if (AnimGraphNode* n = m_graph.Find(m_dragNode)) {
            if (!m_dragMoved) {
                PushUndo();
                m_dragMoved = true;
            }
            n->Pos.x += io.MouseDelta.x / m_zoom;
            n->Pos.y += io.MouseDelta.y / m_zoom;
            Changed(false);
        }
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        std::string why;
        if (m_linkFrom && hoverInNode) {
            if (m_graph.CanLink(m_linkFrom, hoverInNode, hoverInPin, &why)) {
                PushUndo();
                m_graph.Connect(m_linkFrom, hoverInNode, hoverInPin);
                Changed();
            } else {
                m_status = why;
            }
        } else if (m_linkToNode && hoverOut) {
            if (m_graph.CanLink(hoverOut, m_linkToNode, m_linkToPin, &why)) {
                PushUndo();
                m_graph.Connect(hoverOut, m_linkToNode, m_linkToPin);
                Changed();
            } else {
                m_status = why;
            }
        }
        m_linkFrom = m_linkToNode = 0;
        m_linkToPin = -1;
        m_dragNode = 0;
    }
    // Правый щелчок без перемещения — меню: по узлу — его действия, по пустому
    // месту — создание узла в этой точке.
    if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
        ImGui::GetMouseDragDelta(ImGuiMouseButton_Right).x * ImGui::GetMouseDragDelta(ImGuiMouseButton_Right).x +
                ImGui::GetMouseDragDelta(ImGuiMouseButton_Right).y * ImGui::GetMouseDragDelta(ImGuiMouseButton_Right).y <
            16.0f) {
        m_selected = hoverNode;
        m_menuAt = ToCanvas(io.MousePos);
        m_menuRequest = true;
    }
    if ((hovered || ImGui::IsWindowFocused()) && !io.WantTextInput &&
        (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false)))
        DeleteSelected();

    // Подсказка на пустом графе: как начать.
    if (m_graph.Nodes.size() <= 1) {
        const char* hint = T("Right-click to add nodes: Animation Clip, Blend, Float Parameter");
        const ImVec2 ts = ImGui::CalcTextSize(hint);
        dl->AddText(ImVec2(m_origin.x + (m_canvasSize.x - ts.x) * 0.5f, end.y - ts.y - 12.0f),
                    EditorTheme::Color32(EditorTheme::Role::TextFaint), hint);
    }
    dl->PopClipRect();
}

void AnimGraphPanel::DrawCanvasMenu(EditorHost& host) {
    (void)host;
    if (m_menuRequest) {
        ImGui::OpenPopup(kCanvasMenu);
        m_menuRequest = false;
    }
    Sage::UI::MenuScope menu;
    if (!ImGui::BeginPopup(kCanvasMenu)) return;
    const AnimGraphNode* n = m_graph.Find(m_selected);
    if (n) {
        const bool canDelete = n->Kind != AnimNodeKind::Output;
        if (EditorIcons::MenuItem("wire", T("Disconnect all wires"))) {
            PushUndo();
            const int id = n->Id;
            m_graph.Links.erase(std::remove_if(m_graph.Links.begin(), m_graph.Links.end(),
                                               [id](const AnimGraphLink& l) { return l.From == id || l.To == id; }),
                                m_graph.Links.end());
            Changed();
        }
        if (EditorIcons::MenuItem("trash", T("Delete node"), "Del", canDelete)) DeleteSelected();
    } else {
        if (EditorIcons::MenuItem("anim", T("Animation Clip"))) AddNodeAt(AnimNodeKind::Clip, m_menuAt);
        if (EditorIcons::MenuItem("wire", T("Blend"))) AddNodeAt(AnimNodeKind::Blend, m_menuAt);
        if (EditorIcons::MenuItem("chart", T("Float Parameter"))) AddNodeAt(AnimNodeKind::Float, m_menuAt);
        if (m_graph.OutputNode() == 0 && EditorIcons::MenuItem("exit", T("Output")))
            AddNodeAt(AnimNodeKind::Output, m_menuAt);
    }
    ImGui::EndPopup();
}
