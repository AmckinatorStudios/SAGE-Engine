#include "RectSelect.h"

#include <algorithm>
#include <cmath>

namespace sage::editor::rectselect {

namespace {
// Ниже этого рука не дрожит, выше — уже жест. Четыре пикселя подобраны так,
// чтобы клик по мелкой строке дерева не превращался в рамку.
constexpr float kMinDrag = 4.0f;
} // namespace

bool Begin(State& s) {
    s.Finished = false;
    if (!s.Dragging) return false;
    s.Current = ImGui::GetMousePos();
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        s.Dragging = false;
        s.Finished = true;
    }
    // И в кадре отпускания тоже true: набор собирают по тем же элементам, что
    // сейчас нарисованы.
    return true;
}

bool Hits(const State& s, const ImVec2& itemMin, const ImVec2& itemMax) {
    ImVec2 mn, mx;
    Bounds(s, mn, mx);
    return !(itemMax.x < mn.x || itemMin.x > mx.x || itemMax.y < mn.y || itemMin.y > mx.y);
}

void End(State& s, bool canStart) {
    if (s.Dragging || !canStart) return;
    if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return;
    s.Dragging = true;
    s.Finished = false;
    s.Start = s.Current = ImGui::GetMousePos();
    // Ctrl и Shift означают одно и то же — «добавить к тому, что уже выбрано».
    // Разводить их (одна добавляет, другая выделяет диапазон) здесь не за чем:
    // диапазона у сетки файлов и дерева сцены нет, а две клавиши с разным
    // смыслом человек всё равно перепробует обе.
    s.Additive = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift;
}

bool Meaningful(const State& s) {
    return std::fabs(s.Current.x - s.Start.x) >= kMinDrag ||
           std::fabs(s.Current.y - s.Start.y) >= kMinDrag;
}

void Bounds(const State& s, ImVec2& outMin, ImVec2& outMax) {
    outMin = ImVec2(std::min(s.Start.x, s.Current.x), std::min(s.Start.y, s.Current.y));
    outMax = ImVec2(std::max(s.Start.x, s.Current.x), std::max(s.Start.y, s.Current.y));
}

void Draw(const State& s) {
    if (!s.Dragging || !Meaningful(s)) return;
    ImVec2 mn, mx;
    Bounds(s, mn, mx);
    // Поверх ВСЕГО окна (ForegroundDrawList), а не в его слое: во вьюпорте
    // рамка иначе уехала бы под картинку сцены, а в сетке ассетов — под
    // карточки, и жест выглядел бы неработающим.
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImU32 fill = ImGui::GetColorU32(ImGuiCol_Header, 0.35f);
    const ImU32 line = ImGui::GetColorU32(ImGuiCol_NavHighlight);
    dl->AddRectFilled(mn, mx, fill, 2.0f);
    dl->AddRect(mn, mx, line, 2.0f, 0, 1.4f);
}

} // namespace sage::editor::rectselect
