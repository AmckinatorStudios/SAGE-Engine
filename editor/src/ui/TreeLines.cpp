#include "TreeLines.h"

#include <algorithm>
#include <cmath>

#include "imgui_internal.h"   // IM_PI: дуга угла ветки

#include "../EditorIcons.h"

namespace sage::editor::treelines {

void Lines::Draw(const ImVec2& parentPos, float indent, int childDepth) {
    if (m_rows.empty()) return;
    const float line = ImGui::GetTextLineHeight();
    // Вертикаль идёт по середине отступа — там же, где её ждёт глаз: между
    // стрелкой родителя и значком ребёнка.
    const float spineX = std::floor(parentPos.x + indent * 0.5f) + 0.5f;
    const ImU32 col = ImGui::GetColorU32(ImGuiCol_TreeLines);
    const float thickness = std::max(1.0f, ImGui::GetStyle().TreeLinesSize);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    float lastMid = 0.0f;
    float lastEnd = 0.0f;
    for (const RowInfo& r : m_rows) {
        if (r.Depth != childDepth) continue;           // только прямые дети
        const float mid = std::floor(r.Y + line * 0.5f) + 0.5f;
        // ЛИНИЯ НЕ ЛЕЗЕТ В СТРЕЛКУ. У строки с детьми в начале стоит стрелка
        // раскрытия, и горизонталь, доведённая до значка, шла ПРЯМО СКВОЗЬ неё
        // — стрелка выглядела перечёркнутой. У листа стрелки нет, и там линию
        // правильно вести до самого значка: иначе она обрывается в пустоте.
        const float end = r.Arrow ? r.ArrowX - 2.0f : r.IconX - EditorIcons::TextGap() * 0.5f;
        if (end <= spineX + 1.0f) continue;   // вести нечего: стрелка вплотную к вертикали
        lastMid = mid;
        lastEnd = end;
        dl->AddLine(ImVec2(spineX, mid), ImVec2(end, mid), col, thickness);
    }
    if (lastMid > 0.0f) {
        // ВЕРТИКАЛЬ НЕ ДОХОДИТ ДО ПОСЛЕДНЕЙ ГОРИЗОНТАЛИ на радиус скругления:
        // угол дорисовывается дугой. Прямой стык двух линий в конце ветки
        // выглядит обрубком — будто дерево не дорисовали, — а скруглённый
        // читается как «здесь ветка кончилась».
        const float radius = std::min(5.0f, std::max(2.0f, line * 0.22f));
        const float top = std::floor(parentPos.y + line) + 0.5f;
        if (lastMid - radius > top) {
            dl->AddLine(ImVec2(spineX, top), ImVec2(spineX, lastMid - radius), col, thickness);
        }
        // Дуга от вертикали к горизонтали последнего ребёнка.
        dl->PathClear();
        dl->PathArcTo(ImVec2(spineX + radius, lastMid - radius), radius, IM_PI, IM_PI * 0.5f, 8);
        dl->PathStroke(col, 0, thickness);
        // И короткий хвост от дуги до конца горизонтали — дуга кончается
        // правее вертикали, и без хвоста между ней и подписью остаётся провал.
        if (lastEnd > spineX + radius) {
            dl->AddLine(ImVec2(spineX + radius, lastMid), ImVec2(lastEnd, lastMid), col, thickness);
        }
    }
    // Строки этого уровня уже использованы — дальше их не надо ни родителю, ни
    // соседям: иначе вертикаль тянулась бы через чужие ветки.
    m_rows.erase(std::remove_if(m_rows.begin(), m_rows.end(),
                                [childDepth](const RowInfo& r) { return r.Depth >= childDepth; }),
                 m_rows.end());
}

} // namespace sage::editor::treelines
