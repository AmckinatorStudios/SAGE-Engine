#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "sage/ui/core/UIContext.h"
#include "sage/ui/visual/UIText.h"

// ---------------------------------------------------------------------------
// ПРОХОД РАСКЛАДКИ ТЕКСТА — чистая функция: строка + настройки + метрики шрифта
// → готовые строки с позициями глифов.
//
// БЕЗ GPU И БЕЗ ДЕРЕВА. Поэтому она целиком проверяется модульными тестами
// (перенос, многоточие, предел строк, автоподбор кегля, выравнивание) — то
// есть ровно те места, где текст ломается на практике, проверяются без запуска
// игры.
// ---------------------------------------------------------------------------
namespace sage::ui {

// Один выложенный глиф. Позиция — от левого верхнего угла области текста.
struct UIGlyphPlacement {
    uint32_t Codepoint = 0;
    float X = 0.0f;      // позиция пера
    float Baseline = 0.0f;
    float Advance = 0.0f;
    int Font = -1;       // может отличаться от основного: сработал запасной
    int RunIndex = -1;   // какой оформленный кусок (-1 — общий стиль)
    int ByteOffset = 0;  // смещение в исходной строке — для каретки
};

struct UITextLine {
    int First = 0, Count = 0; // диапазон в массиве глифов
    float Width = 0.0f;
    float Top = 0.0f;
    float Baseline = 0.0f;
    float Height = 0.0f;
    int ByteBegin = 0, ByteEnd = 0;
    bool Ellipsized = false;
};

struct UITextLayoutResult {
    std::vector<UIGlyphPlacement> Glyphs;
    std::vector<UITextLine> Lines;
    glm::vec2 Size{0.0f, 0.0f}; // занятая область
    float FontSize = 0.0f;      // фактический кегль (мог быть подобран)
    bool Truncated = false;     // что-то не поместилось
};

// Посчитать раскладку. maxWidth/maxHeight <= 0 — «не ограничено по этой оси».
UITextLayoutResult UILayoutText(const UIContext& ctx, const UIText& text,
                                float maxWidth, float maxHeight);

// Индекс байта в строке, ближайший к точке (для каретки поля ввода). Точка — в
// координатах области текста.
int UITextIndexAt(const UITextLayoutResult& layout, glm::vec2 point);

// Позиция каретки для смещения в байтах.
glm::vec2 UITextCaretPos(const UITextLayoutResult& layout, int byteOffset, float& heightOut);

// --- Работа с UTF-8 ---------------------------------------------------------
//
// Живёт здесь, потому что нужна и раскладке, и полю ввода, и подгонке по
// содержимому. Две копии разбора UTF-8 разъедутся на первой же кириллической
// строке.
uint32_t UIUtf8Next(const std::string& s, int& i);       // читает символ, двигает i
int UIUtf8Prev(const std::string& s, int i);             // начало предыдущего символа
int UIUtf8Length(const std::string& s);                  // длина в СИМВОЛАХ
void UIUtf8Append(std::string& s, uint32_t codepoint);

} // namespace sage::ui
