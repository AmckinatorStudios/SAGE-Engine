#include "AiGraphPanel.h"

#include <algorithm>
#include <cmath>

#include <nlohmann/json.hpp>

#include "../EditorHost.h"
#include "../HotkeyScope.h"
#include "../Localization.h"
#include "AiGraphDetail.h"
#include "sage/ai/GraphDebug.h"
#include "sage/ai/World.h"
#include "sage/vars/ValueJson.h"

// ---------------------------------------------------------------------------
// ХОЛСТ ГРАФА: узлы, провода, группы, отладочная подсветка, работа мышью.
//
// Мышь: левая — выделить и тащить узлы, рамка по пустому месту, провод из
// пина (Alt+щелчок по пину — снять его провода); правая — палитра (по
// пустому) или меню узла; средняя или правая с протягиванием — двигать
// холст; колесо — масштаб вокруг курсора. Двойной щелчок по «Run Task» —
// открыть граф задачи.
// ---------------------------------------------------------------------------

using sage::ai::GraphLink;
using sage::ai::GraphNode;
using sage::ai::NodeType;
using sage::ai::PinSpec;
using sage::vars::Value;
using namespace sage::editor::aigraph;

namespace {

constexpr float kNodeW = 190.0f;
constexpr float kHeader = 26.0f;
constexpr float kRow = 22.0f;
constexpr float kPinR = 5.5f;

ImU32 CategoryColor(const std::string& cat) {
    if (cat.rfind("Flow", 0) == 0) return IM_COL32(70, 70, 80, 255);
    if (cat.rfind("Tasks", 0) == 0) return IM_COL32(40, 90, 150, 255);
    if (cat.rfind("Events", 0) == 0) return IM_COL32(150, 50, 50, 255);
    if (cat.rfind("Variables", 0) == 0) return IM_COL32(60, 110, 70, 255);
    if (cat.rfind("World", 0) == 0) return IM_COL32(150, 100, 40, 255);
    if (cat.rfind("Engine API", 0) == 0) return IM_COL32(110, 70, 140, 255);
    if (cat.rfind("Math", 0) == 0 || cat.rfind("Logic", 0) == 0) return IM_COL32(60, 100, 110, 255);
    return IM_COL32(90, 90, 100, 255);
}

float Dist2(ImVec2 a, ImVec2 b) {
    const float dx = a.x - b.x, dy = a.y - b.y;
    return dx * dx + dy * dy;
}

void Wire(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 color, float thickness) {
    const float dx = std::max(40.0f, std::fabs(b.x - a.x) * 0.5f);
    dl->AddBezierCubic(a, ImVec2(a.x + dx, a.y), ImVec2(b.x - dx, b.y), b, color, thickness);
}

} // namespace

const NodeType* AiGraphPanel::TypeOf(const GraphNode& n) const {
    sage::ai::World* lib = Library();
    return lib ? lib->Nodes().Find(n.Type) : nullptr;
}

sage::ai::NodePins AiGraphPanel::PinsOf(const GraphNode& n) const {
    sage::ai::World* lib = Library();
    const NodeType* t = TypeOf(n);
    if (!lib || !t) {
        // Неизвестный узел: пины по проводам, чтобы их хотя бы было видно.
        sage::ai::NodePins p;
        for (const GraphLink& l : m_doc.Links) {
            if (l.To.Node == n.Id && !sage::ai::FindPin(p.Inputs, l.To.Pin)) p.Inputs.push_back({l.To.Pin, "any"});
            if (l.From.Node == n.Id && !sage::ai::FindPin(p.Outputs, l.From.Pin))
                p.Outputs.push_back({l.From.Pin, "any"});
        }
        return p;
    }
    const sage::ai::GraphEnv env{lib, &lib->Types(), &m_doc};
    return sage::ai::ResolvePins(*t, n, env);
}

ImVec2 AiGraphPanel::ToScreen(float x, float y) const {
    return ImVec2(m_origin.x + (x + m_pan.x) * m_zoom, m_origin.y + (y + m_pan.y) * m_zoom);
}

ImVec2 AiGraphPanel::ToGraph(ImVec2 s) const {
    return ImVec2((s.x - m_origin.x) / m_zoom - m_pan.x, (s.y - m_origin.y) / m_zoom - m_pan.y);
}

void AiGraphPanel::Layout(const GraphNode& n, NodeLayout& out) const {
    const sage::ai::NodePins pins = PinsOf(n);
    const size_t rows = std::max(pins.Inputs.size(), pins.Outputs.size());
    const NodeType* t = TypeOf(n);
    const float titleW = ImGui::CalcTextSize(t ? Tr(t->Title) : n.Type.c_str()).x + 46.0f;
    // Вход и выход стоят в одной строке: ширина — по самым длинным подписям с
    // обеих сторон, иначе «condition = true» наезжает на «completed».
    auto label = [&](const PinSpec& p, bool input) {
        std::string s = p.Title.empty() ? p.Name : std::string(Tr(p.Title));
        if (input && p.Type != sage::ai::type::Exec && !m_doc.LinkInto(n.Id, p.Name)) {
            auto v = n.Values.find(p.Name);
            if (v != n.Values.end()) s += " = " + v->second.AsString().substr(0, 18);
        }
        return ImGui::CalcTextSize(s.c_str()).x * 0.9f;
    };
    float inW = 0.0f, outW = 0.0f;
    for (const PinSpec& p : pins.Inputs) inW = std::max(inW, label(p, true));
    for (const PinSpec& p : pins.Outputs) outW = std::max(outW, label(p, false));
    const float w = std::max({kNodeW, titleW, inW + outW + 48.0f});
    out.Min = ToScreen(n.X, n.Y);
    out.Max = ToScreen(n.X + w, n.Y + kHeader + kRow * (float)std::max<size_t>(rows, 1) + 6.0f);
    out.HeaderH = kHeader * m_zoom;
    out.Pins.clear();
    for (size_t i = 0; i < pins.Inputs.size(); ++i)
        out.Pins.push_back({n.Id, pins.Inputs[i].Name, false, pins.Inputs[i].Type,
                            ToScreen(n.X, n.Y + kHeader + kRow * ((float)i + 0.5f))});
    for (size_t i = 0; i < pins.Outputs.size(); ++i)
        out.Pins.push_back({n.Id, pins.Outputs[i].Name, true, pins.Outputs[i].Type,
                            ToScreen(n.X + w, n.Y + kHeader + kRow * ((float)i + 0.5f))});
}

bool AiGraphPanel::CanLink(const PinHit& a, const PinHit& b, std::string* why) const {
    if (a.Node == b.Node) {
        if (why) *why = T("A node cannot be wired to itself");
        return false;
    }
    if (a.Output == b.Output) {
        if (why) *why = T("Wire an output to an input");
        return false;
    }
    const PinHit& out = a.Output ? a : b;
    const PinHit& in = a.Output ? b : a;
    sage::ai::World* lib = Library();
    if (!lib) return false;
    if (!lib->Types().CanConnect(out.Type, in.Type, why)) return false;
    // Правило самого узла (узлы из Lua: connect = function(pin, type) ...).
    for (const PinHit* side : {&out, &in}) {
        const PinHit* other = side == &out ? &in : &out;
        const GraphNode* n = m_doc.FindNode(side->Node);
        const NodeType* t = n ? TypeOf(*n) : nullptr;
        std::string reason;
        if (t && t->CanConnect && !t->CanConnect(*n, side->Pin, other->Type, reason)) {
            if (why) *why = reason;
            return false;
        }
    }
    return true;
}

void AiGraphPanel::Link(const PinHit& a, const PinHit& b) {
    const PinHit& out = a.Output ? a : b;
    const PinHit& in = a.Output ? b : a;
    m_doc.Link({out.Node, out.Pin}, {in.Node, in.Pin}, out.Type == sage::ai::type::Exec);
}

int AiGraphPanel::AddNodeAt(const std::string& type, ImVec2 graphPos) {
    // Номер, а не ссылка: следующий AddNode вырастит вектор узлов.
    return m_doc.AddNode(type, std::round(graphPos.x / 10.0f) * 10.0f, std::round(graphPos.y / 10.0f) * 10.0f).Id;
}

void AiGraphPanel::DeleteSelection() {
    if (m_selected.empty() && !m_selectedGroup) return;
    PushUndo();
    for (int id : m_selected) m_doc.RemoveNode(id);
    if (m_selectedGroup) m_doc.RemoveGroup(m_selectedGroup);
    m_selected.clear();
    m_selectedGroup = 0;
    MarkChanged();
}

void AiGraphPanel::CopySelection() {
    sage::ai::GraphDoc part;
    for (int id : m_selected)
        if (const GraphNode* n = m_doc.FindNode(id)) part.Nodes.push_back(*n);
    for (const GraphLink& l : m_doc.Links)
        if (m_selected.count(l.From.Node) && m_selected.count(l.To.Node)) part.Links.push_back(l);
    m_clipboard = sage::ai::SaveGraph(part);
    ImGui::SetClipboardText(m_clipboard.c_str());
}

void AiGraphPanel::Paste(ImVec2 at) {
    const char* clip = ImGui::GetClipboardText();
    const std::string text = clip ? clip : m_clipboard;
    sage::ai::GraphDoc part;
    std::string err;
    if (!sage::ai::LoadGraph(text, part, err) || part.Nodes.empty()) return;
    PushUndo();
    float minX = 1e9f, minY = 1e9f;
    for (const GraphNode& n : part.Nodes) {
        minX = std::min(minX, n.X);
        minY = std::min(minY, n.Y);
    }
    std::map<int, int> remap;
    m_selected.clear();
    for (const GraphNode& n : part.Nodes) {
        GraphNode copy = n;
        copy.Id = m_doc.NextId++;
        copy.X = at.x + (n.X - minX);
        copy.Y = at.y + (n.Y - minY);
        remap[n.Id] = copy.Id;
        m_selected.insert(copy.Id);
        m_doc.Nodes.push_back(std::move(copy));
    }
    for (const GraphLink& l : part.Links)
        m_doc.Links.push_back({{remap[l.From.Node], l.From.Pin}, {remap[l.To.Node], l.To.Pin}});
    MarkChanged();
}

void AiGraphPanel::Duplicate() {
    if (m_selected.empty()) return;
    CopySelection();
    const GraphNode* first = m_doc.FindNode(*m_selected.begin());
    Paste(first ? ImVec2(first->X + 40.0f, first->Y + 40.0f) : ImVec2(0, 0));
}

void AiGraphPanel::FrameAll(ImVec2 size) {
    if (m_doc.Nodes.empty()) return;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (const GraphNode& n : m_doc.Nodes) {
        x0 = std::min(x0, n.X);
        y0 = std::min(y0, n.Y);
        x1 = std::max(x1, n.X + kNodeW);
        y1 = std::max(y1, n.Y + 120.0f);
    }
    const float w = x1 - x0 + 120.0f, h = y1 - y0 + 120.0f;
    m_zoom = std::clamp(std::min(size.x / w, size.y / h), 0.3f, 1.2f);
    m_pan = ImVec2(-x0 + 60.0f + (size.x / m_zoom - w) * 0.5f, -y0 + 60.0f + (size.y / m_zoom - h) * 0.5f);
}

void AiGraphPanel::DrawCanvas(EditorHost& host) {
    ImGuiIO& io = ImGui::GetIO();
    m_origin = ImGui::GetCursorScreenPos();
    m_canvasSize = ImGui::GetContentRegionAvail();
    if (m_canvasSize.x < 50.0f || m_canvasSize.y < 50.0f) return;
    if (m_frameRequest) {
        FrameAll(m_canvasSize);
        m_frameRequest = false;
    }
    ImGui::InvisibleButton("##aig_canvas_area", m_canvasSize,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 end(m_origin.x + m_canvasSize.x, m_origin.y + m_canvasSize.y);
    dl->PushClipRect(m_origin, end, true);
    dl->AddRectFilled(m_origin, end, IM_COL32(28, 29, 33, 255));
    const float grid = 32.0f * m_zoom;
    for (float x = std::fmod(m_pan.x * m_zoom, grid); x < m_canvasSize.x; x += grid)
        dl->AddLine(ImVec2(m_origin.x + x, m_origin.y), ImVec2(m_origin.x + x, end.y), IM_COL32(40, 42, 48, 255));
    for (float y = std::fmod(m_pan.y * m_zoom, grid); y < m_canvasSize.y; y += grid)
        dl->AddLine(ImVec2(m_origin.x, m_origin.y + y), ImVec2(end.x, m_origin.y + y), IM_COL32(40, 42, 48, 255));

    // Масштаб и сдвиг.
    if (hovered && io.MouseWheel != 0.0f) {
        const ImVec2 before = ToGraph(io.MousePos);
        m_zoom = std::clamp(m_zoom * (1.0f + 0.1f * io.MouseWheel), 0.25f, 2.0f);
        const ImVec2 after = ToGraph(io.MousePos);
        m_pan.x += after.x - before.x;
        m_pan.y += after.y - before.y;
    }
    if (ImGui::IsItemActive() && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
                                  ImGui::IsMouseDragging(ImGuiMouseButton_Right, 4.0f))) {
        m_pan.x += io.MouseDelta.x / m_zoom;
        m_pan.y += io.MouseDelta.y / m_zoom;
    }

    sage::ai::World* lib = Library();
    sage::ai::GraphDebugger& dbg = sage::ai::GraphDebugger::Get();
    sage::ai::RunDebug* rd = dbg.FindRun(m_debugRun);
    if (rd && rd->Graph != m_doc.Name) rd = nullptr;
    const float fontSize = ImGui::GetFontSize() * m_zoom;
    ImFont* font = ImGui::GetFont();

    std::map<int, NodeLayout> layouts;
    for (const GraphNode& n : m_doc.Nodes) Layout(n, layouts[n.Id]);
    auto pinPos = [&](int node, const std::string& pin, bool output, ImVec2& pos, std::string& type) {
        auto it = layouts.find(node);
        if (it == layouts.end()) return false;
        for (const PinHit& p : it->second.Pins)
            if (p.Pin == pin && p.Output == output) {
                pos = p.Pos;
                type = p.Type;
                return true;
            }
        return false;
    };

    // --- Группы ----------------------------------------------------------------
    for (const sage::ai::GraphGroup& g : m_doc.Groups) {
        const ImVec2 a = ToScreen(g.X, g.Y), b = ToScreen(g.X + g.W, g.Y + g.H);
        dl->AddRectFilled(a, b, g.Color, 6.0f);
        dl->AddRectFilled(a, ImVec2(b.x, a.y + 24.0f * m_zoom), (g.Color & 0x00FFFFFF) | 0x90000000, 6.0f);
        dl->AddRect(a, b, m_selectedGroup == g.Id ? IM_COL32(255, 200, 80, 255) : IM_COL32(255, 255, 255, 40), 6.0f);
        dl->AddText(font, fontSize, ImVec2(a.x + 8.0f, a.y + 4.0f * m_zoom), IM_COL32(240, 240, 240, 230),
                    g.Title.c_str());
        dl->AddTriangleFilled(ImVec2(b.x - 12.0f, b.y), b, ImVec2(b.x, b.y - 12.0f), IM_COL32(255, 255, 255, 80));
    }

    // --- Провода -------------------------------------------------------------
    for (const GraphLink& l : m_doc.Links) {
        ImVec2 a, b;
        std::string ta, tb;
        if (!pinPos(l.From.Node, l.From.Pin, true, a, ta) || !pinPos(l.To.Node, l.To.Pin, false, b, tb)) continue;
        const bool exec = ta == sage::ai::type::Exec;
        ImU32 c = exec ? IM_COL32(230, 230, 230, 220) : (lib ? TypeColor(lib->Types(), ta) : IM_COL32_WHITE);
        // Отладка: провод потока к узлу, шагавшему в этом кадре, — ярче.
        if (rd && exec && rd->NodeFrame.count(l.To.Node) && dbg.LastFrame() - rd->NodeFrame[l.To.Node] < 2)
            c = IM_COL32(255, 220, 90, 255);
        Wire(dl, a, b, c, (exec ? 3.0f : 2.0f) * m_zoom);
    }

    // --- Узлы ----------------------------------------------------------------
    PinHit hoveredPin;
    bool pinUnder = false;
    int nodeUnder = 0;
    for (const GraphNode& n : m_doc.Nodes) {
        const NodeLayout& L = layouts[n.Id];
        const NodeType* t = TypeOf(n);
        const bool sel = m_selected.count(n.Id) != 0;
        if (!n.Comment.empty())
            dl->AddText(font, fontSize * 0.95f, ImVec2(L.Min.x, L.Min.y - fontSize - 4.0f), IM_COL32(200, 200, 160, 230),
                        n.Comment.c_str());
        dl->AddRectFilled(L.Min, L.Max, IM_COL32(44, 46, 54, 240), 6.0f * m_zoom);
        const ImU32 head = t ? (t->Color ? t->Color : CategoryColor(t->Category)) : IM_COL32(160, 40, 40, 255);
        dl->AddRectFilled(L.Min, ImVec2(L.Max.x, L.Min.y + L.HeaderH), head, 6.0f * m_zoom,
                          ImDrawFlags_RoundCornersTop);
        dl->AddText(font, fontSize, ImVec2(L.Min.x + 22.0f * m_zoom, L.Min.y + 5.0f * m_zoom), IM_COL32_WHITE,
                    t ? Tr(t->Title) : n.Type.c_str());
        // Отладка: шагал недавно — свечение гаснет за полсекунды; стоит ветка
        // — жёлтая рамка; пауза — красная.
        ImU32 border = sel ? IM_COL32(255, 200, 80, 255) : IM_COL32(0, 0, 0, 120);
        float bw = sel ? 2.5f : 1.0f;
        if (rd) {
            auto it = rd->NodeFrame.find(n.Id);
            if (it != rd->NodeFrame.end()) {
                const uint64_t age = dbg.LastFrame() - it->second;
                if (age < 30) {
                    const int a = (int)(255 * (1.0f - (float)age / 30.0f));
                    dl->AddRect(ImVec2(L.Min.x - 3, L.Min.y - 3), ImVec2(L.Max.x + 3, L.Max.y + 3),
                                IM_COL32(120, 220, 255, a), 7.0f * m_zoom, 0, 3.0f);
                }
            }
            if (rd->Current.count(n.Id)) { border = IM_COL32(255, 220, 60, 255); bw = 3.0f; }
            if (rd->Paused && rd->PausedNode == n.Id) { border = IM_COL32(255, 70, 70, 255); bw = 3.5f; }
        }
        dl->AddRect(L.Min, L.Max, border, 6.0f * m_zoom, 0, bw);
        // Точка останова — красная метка в углу заголовка (щелчок — переключить).
        const ImVec2 bpPos(L.Min.x + 11.0f * m_zoom, L.Min.y + L.HeaderH * 0.5f);
        if (dbg.HasBreakpoint(m_doc.Name, n.Id)) dl->AddCircleFilled(bpPos, 5.0f * m_zoom, IM_COL32(230, 50, 50, 255));
        else dl->AddCircle(bpPos, 5.0f * m_zoom, IM_COL32(255, 255, 255, 50));

        // Пины.
        const sage::ai::NodePins pins = PinsOf(n);
        for (const PinHit& p : L.Pins) {
            const bool exec = p.Type == sage::ai::type::Exec;
            const ImU32 c = exec ? IM_COL32(235, 235, 235, 255) : (lib ? TypeColor(lib->Types(), p.Type) : IM_COL32_WHITE);
            const bool linked = p.Output ? m_doc.LinkFrom(n.Id, p.Pin) != nullptr : m_doc.LinkInto(n.Id, p.Pin) != nullptr;
            const float r = kPinR * m_zoom;
            if (exec) {
                const ImVec2 a(p.Pos.x - r, p.Pos.y - r), b(p.Pos.x - r, p.Pos.y + r), c2(p.Pos.x + r, p.Pos.y);
                if (linked) dl->AddTriangleFilled(a, b, c2, c);
                else dl->AddTriangle(a, b, c2, c, 1.5f);
            } else {
                if (linked) dl->AddCircleFilled(p.Pos, r, c);
                else dl->AddCircle(p.Pos, r, c, 0, 1.5f);
            }
            const PinSpec* spec = sage::ai::FindPin(p.Output ? pins.Outputs : pins.Inputs, p.Pin);
            std::string label = spec && !spec->Title.empty() ? Tr(spec->Title) : p.Pin;
            // Вписанное значение несоединённого входа — прямо у пина.
            if (!p.Output && !exec && !linked) {
                auto v = n.Values.find(p.Pin);
                if (v != n.Values.end()) label += " = " + v->second.AsString().substr(0, 18);
            }
            const ImVec2 sz = font->CalcTextSizeA(fontSize * 0.9f, FLT_MAX, 0.0f, label.c_str());
            const ImVec2 at = p.Output ? ImVec2(p.Pos.x - r - 6.0f * m_zoom - sz.x, p.Pos.y - sz.y * 0.5f)
                                       : ImVec2(p.Pos.x + r + 6.0f * m_zoom, p.Pos.y - sz.y * 0.5f);
            dl->AddText(font, fontSize * 0.9f, at, IM_COL32(215, 215, 220, 255), label.c_str());
            if (hovered && Dist2(io.MousePos, p.Pos) < (r + 5.0f) * (r + 5.0f)) {
                hoveredPin = p;
                pinUnder = true;
            }
        }
        if (hovered && io.MousePos.x >= L.Min.x && io.MousePos.x <= L.Max.x && io.MousePos.y >= L.Min.y &&
            io.MousePos.y <= L.Max.y)
            nodeUnder = n.Id;
    }

    // Подсказка у пина: тип и (в отладке) последнее значение.
    if (pinUnder && !m_linking) {
        std::string tip = hoveredPin.Pin + " : " + hoveredPin.Type;
        if (rd) {
            auto it = rd->Pins.find({hoveredPin.Node, hoveredPin.Pin});
            if (it != rd->Pins.end()) tip += "\n= " + it->second;
        }
        ImGui::SetTooltip("%s", tip.c_str());
    }

    // --- Мышь ----------------------------------------------------------------
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (pinUnder) {
            if (io.KeyAlt) {
                PushUndo();
                m_doc.Unlink({hoveredPin.Node, hoveredPin.Pin});
                MarkChanged();
            } else {
                m_linking = true;
                m_linkFrom = hoveredPin;
            }
        } else if (nodeUnder) {
            const NodeLayout& L = layouts[nodeUnder];
            const ImVec2 bpPos(L.Min.x + 11.0f * m_zoom, L.Min.y + L.HeaderH * 0.5f);
            if (Dist2(io.MousePos, bpPos) < 64.0f * m_zoom * m_zoom) {
                dbg.ToggleBreakpoint(m_doc.Name, nodeUnder);
            } else {
                if (io.KeyCtrl) {
                    if (!m_selected.erase(nodeUnder)) m_selected.insert(nodeUnder);
                } else if (!m_selected.count(nodeUnder)) {
                    m_selected = {nodeUnder};
                }
                m_selectedGroup = 0;
                m_draggingNodes = true;
                m_pushedForDrag = false;
            }
        } else {
            // Группа: заголовок — тащить с узлами внутри, угол — растянуть.
            int groupHit = 0;
            bool corner = false;
            for (auto it = m_doc.Groups.rbegin(); it != m_doc.Groups.rend(); ++it) {
                const ImVec2 a = ToScreen(it->X, it->Y), b = ToScreen(it->X + it->W, it->Y + it->H);
                if (Dist2(io.MousePos, b) < 196.0f) { groupHit = it->Id; corner = true; break; }
                if (io.MousePos.x >= a.x && io.MousePos.x <= b.x && io.MousePos.y >= a.y &&
                    io.MousePos.y <= a.y + 24.0f * m_zoom) { groupHit = it->Id; break; }
            }
            if (groupHit) {
                m_selectedGroup = groupHit;
                m_selected.clear();
                m_pushedForDrag = false;
                if (corner) m_resizingGroup = groupHit;
                else {
                    m_draggingGroup = groupHit;
                    m_groupNodes.clear();
                    for (const sage::ai::GraphGroup& g : m_doc.Groups)
                        if (g.Id == groupHit)
                            for (const GraphNode& n : m_doc.Nodes)
                                if (n.X >= g.X && n.Y >= g.Y && n.X <= g.X + g.W && n.Y <= g.Y + g.H)
                                    m_groupNodes.push_back(n.Id);
                }
            } else {
                if (!io.KeyCtrl) m_selected.clear();
                m_selectedGroup = 0;
                m_boxSelecting = true;
                m_boxStart = io.MousePos;
            }
        }
    }
    if (hovered && nodeUnder && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        if (const GraphNode* n = m_doc.FindNode(nodeUnder))
            if ((n->Type == "task.run" || n->Type == "task.call") && n->Values.count("task") && lib &&
                lib->FindGraph(n->Values.at("task").AsString()))
                OpenTaskGraph(host, n->Values.at("task").AsString());
    }
    const bool dragging = ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f);
    auto beginDragUndo = [this]() {
        if (!m_pushedForDrag) {
            PushUndo();
            m_pushedForDrag = true;
        }
    };
    if (m_draggingNodes && dragging) {
        beginDragUndo();
        for (int id : m_selected)
            if (GraphNode* n = m_doc.FindNode(id)) {
                n->X += io.MouseDelta.x / m_zoom;
                n->Y += io.MouseDelta.y / m_zoom;
            }
    }
    if ((m_draggingGroup || m_resizingGroup) && dragging) {
        beginDragUndo();
        for (sage::ai::GraphGroup& g : m_doc.Groups) {
            if (g.Id == m_resizingGroup) {
                g.W = std::max(120.0f, g.W + io.MouseDelta.x / m_zoom);
                g.H = std::max(80.0f, g.H + io.MouseDelta.y / m_zoom);
            }
            if (g.Id == m_draggingGroup) {
                g.X += io.MouseDelta.x / m_zoom;
                g.Y += io.MouseDelta.y / m_zoom;
                for (int id : m_groupNodes)
                    if (GraphNode* n = m_doc.FindNode(id)) {
                        n->X += io.MouseDelta.x / m_zoom;
                        n->Y += io.MouseDelta.y / m_zoom;
                    }
            }
        }
    }
    if (m_linking) {
        std::string why;
        const bool ok = !pinUnder || CanLink(m_linkFrom, hoveredPin, &why);
        const ImU32 c = ok ? IM_COL32(255, 255, 255, 200) : IM_COL32(255, 80, 80, 200);
        if (m_linkFrom.Output) Wire(dl, m_linkFrom.Pos, io.MousePos, c, 2.0f * m_zoom);
        else Wire(dl, io.MousePos, m_linkFrom.Pos, c, 2.0f * m_zoom);
        if (pinUnder && !ok) ImGui::SetTooltip("%s", why.c_str());
    }
    if (m_boxSelecting) {
        dl->AddRectFilled(m_boxStart, io.MousePos, IM_COL32(100, 150, 255, 40));
        dl->AddRect(m_boxStart, io.MousePos, IM_COL32(100, 150, 255, 200));
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (m_linking) {
            if (pinUnder) {
                if (CanLink(m_linkFrom, hoveredPin, nullptr)) {
                    PushUndo();
                    Link(m_linkFrom, hoveredPin);
                    MarkChanged();
                }
            } else if (hovered) {
                // Провод в пустоту — палитра с подходящими узлами.
                m_paletteFromLink = true;
                m_paletteLink = m_linkFrom;
                m_paletteAt = io.MousePos;
                m_paletteRequest = true;
            }
        }
        if (m_boxSelecting) {
            const ImVec2 a(std::min(m_boxStart.x, io.MousePos.x), std::min(m_boxStart.y, io.MousePos.y));
            const ImVec2 b(std::max(m_boxStart.x, io.MousePos.x), std::max(m_boxStart.y, io.MousePos.y));
            for (const auto& [id, L] : layouts)
                if (L.Max.x >= a.x && L.Min.x <= b.x && L.Max.y >= a.y && L.Min.y <= b.y) m_selected.insert(id);
        }
        if (m_pushedForDrag) MarkChanged();
        m_linking = m_boxSelecting = m_draggingNodes = false;
        m_draggingGroup = m_resizingGroup = 0;
        m_pushedForDrag = false;
    }
    // Правая кнопка без протягивания: меню узла или палитра.
    if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
        ImGui::GetMouseDragDelta(ImGuiMouseButton_Right).x * ImGui::GetMouseDragDelta(ImGuiMouseButton_Right).x +
                ImGui::GetMouseDragDelta(ImGuiMouseButton_Right).y * ImGui::GetMouseDragDelta(ImGuiMouseButton_Right).y <
            16.0f) {
        if (nodeUnder) {
            if (!m_selected.count(nodeUnder)) m_selected = {nodeUnder};
            m_menuNode = nodeUnder;
            m_menuRequest = true;
        } else {
            m_paletteFromLink = false;
            m_paletteAt = io.MousePos;
            m_paletteRequest = true;
        }
    }
    dl->PopClipRect();

    // --- Клавиши — когда окно графа в фокусе и не печатают в поле ----------
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput) {
        namespace hk = sage::editor::hotkeys;
        hk::Claim(hk::Key::Delete);
        hk::Claim(hk::Key::Duplicate);
        hk::Claim(hk::Key::Undo);
        hk::Claim(hk::Key::Save);
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) DeleteSelection();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) Duplicate();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) CopySelection();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) Paste(ToGraph(io.MousePos));
        if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z)) Undo();
        if (io.KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_Y) || (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z)))) Redo();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) Save(host);
        if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false)) m_frameRequest = true;
        if (ImGui::IsKeyPressed(ImGuiKey_F9, false))
            for (int id : m_selected) dbg.ToggleBreakpoint(m_doc.Name, id);
        if (m_debugRun && ImGui::IsKeyPressed(ImGuiKey_F5, false)) dbg.Continue(m_debugRun);
        if (m_debugRun && ImGui::IsKeyPressed(ImGuiKey_F10, false)) dbg.Step(m_debugRun);
    }
}
