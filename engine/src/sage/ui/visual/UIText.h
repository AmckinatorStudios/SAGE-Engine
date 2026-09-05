#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "sage/ui/core/UIComponent.h"
#include "sage/ui/core/UIContext.h"
#include "sage/ui/visual/UIGradient.h"

// ---------------------------------------------------------------------------
// ТЕКСТ (§15, §16 ТЗ) — полноценная подсистема со своим проходом раскладки.
//
// ЧЕМ ЭТО ОТЛИЧАЕТСЯ ОТ DrawText(). Раньше текст был строкой и масштабом:
// рисующий сам решал, где её начать, и на этом всё заканчивалось. Не было ни
// переноса по словам с учётом кириллицы, ни многоточия, ни ограничения на число
// строк, ни автоподбора кегля, ни выравнивания по базовой линии, ни запасного
// шрифта для отсутствующего символа, ни оформленных кусков внутри одной строки.
// Каждая из этих вещей — не украшение: без переноса длинная реплика уезжает за
// край панели, без многоточия имя предмета налезает на цену, без запасного
// шрифта пользователь видит ряды прямоугольников.
//
// ЗДЕСЬ ТЕКСТ СЧИТАЕТСЯ ОТДЕЛЬНО ОТ РИСОВАНИЯ. Проход раскладки (UITextLayout)
// превращает строку и настройки в список строк с позициями и глифами; рисующий
// только выкладывает готовое. Благодаря этому одна и та же раскладка отвечает
// и на «сколько места займёт текст» (для подгонки по содержимому), и на «куда
// поставить каретку» (для поля ввода), и на «что нарисовать».
// ---------------------------------------------------------------------------
namespace sage::ui {

enum class UITextAlign { Left, Center, Right, Justify };
enum class UITextVAlign { Top, Center, Bottom, Baseline };

// Что делать со строкой, которая не влезла (§15).
enum class UITextOverflow {
    Clip,     // обрезать по краю
    Ellipsis, // многоточие в конце
    Visible,  // не обрезать (пусть вылезает — иногда так и надо)
};

enum class UITextWrap {
    None,     // только явные переводы строки
    Word,     // по словам, длинное слово режется по символам
    Character // по символам
};

// Оформленный кусок текста (§15, rich text runs). Диапазон задаётся в БАЙТАХ
// UTF-8: так его отдаёт разбор, так же адресуется каретка поля ввода.
struct UITextRun {
    int Begin = 0;
    int End = 0;
    UIColor Color{1.0f, 1.0f, 1.0f, 1.0f};
    bool OverrideColor = false;
    float SizeScale = 1.0f;
    int Weight = 0;      // 0 — как у всего текста
    bool Italic = false;
    bool Underline = false;
    bool Strike = false;
};

struct UIText : UIComponentOf<UIText> {
    static const UIComponentType& StaticType();

    // ТЕКСТ ИЛИ КЛЮЧ ПЕРЕВОДА (§108). Ключ имеет приоритет: строка остаётся
    // как запасной вариант и как то, что видно в редакторе, если словаря нет.
    std::string Text;
    std::string Key;

    std::string Font;       // семейство или путь; пусто — шрифт по умолчанию
    int Weight = 400;
    bool Italic = false;
    float Size = 18.0f;     // кегль в логических единицах холста
    UIColor Color{1.0f, 1.0f, 1.0f, 1.0f};
    UIGradient Gradient;

    UITextAlign Align = UITextAlign::Left;
    UITextVAlign VAlign = UITextVAlign::Top;
    UITextWrap Wrap = UITextWrap::Word;
    UITextOverflow Overflow = UITextOverflow::Clip;

    float LetterSpacing = 0.0f;
    float LineSpacing = 1.0f;   // множитель к высоте строки шрифта
    float ParagraphSpacing = 0.0f;
    int MaxLines = 0;           // 0 — без предела

    // Автоподбор кегля под прямоугольник (§15). Границы обязательны: без них
    // «уместить любой ценой» приводит к нечитаемым двум пикселям.
    bool AutoSize = false;
    float MinSize = 8.0f;
    float MaxSize = 96.0f;

    UIEdges Padding{0.0f, 0.0f, 0.0f, 0.0f};

    // Обводка и тень — свойства ТЕКСТА, а не эффект поддерева: они нужны почти
    // каждой надписи поверх картинки, и гонять ради них отдельную цель
    // рисования было бы расточительством (§130).
    float OutlineWidth = 0.0f;
    UIColor OutlineColor{0.0f, 0.0f, 0.0f, 1.0f};
    glm::vec2 ShadowOffset{0.0f, 0.0f};
    UIColor ShadowColor{0.0f, 0.0f, 0.0f, 0.6f};
    float ShadowSoftness = 0.0f;

    std::vector<UITextRun> Runs;

    glm::vec2 Measure(const UIContext& ctx, const UINode& node,
                      glm::vec2 available) const override;

    // Итоговая строка: перевод по ключу, иначе Text.
    std::string Resolve(const UIContext& ctx) const;
};

const char* const* UITextAlignNames();
int UITextAlignCount();
const char* const* UITextVAlignNames();
int UITextVAlignCount();
const char* const* UITextWrapNames();
int UITextWrapCount();
const char* const* UITextOverflowNames();
int UITextOverflowCount();

} // namespace sage::ui
