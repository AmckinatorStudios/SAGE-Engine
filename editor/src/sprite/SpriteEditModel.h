#pragma once
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "sage/ui/NineSlice.h"
#include "sage/ui/SpriteSheet.h"

// ---------------------------------------------------------------------------
// ДАННЫЕ РЕДАКТОРА СПРАЙТОВ — без ImGui.
//
// Всё, что можно ошибиться посчитать (история правок, «добавить или
// заменить», попадание в рамку, тяга за ручки, точка опоры, проверка имени,
// сохранение двух файлов), живёт здесь: это проверяется модульным тестом
// (tests/test_sprite_editor.cpp), а окно лишь рисует и зовёт эти функции.
// ---------------------------------------------------------------------------
namespace sage::editor::sprite {

// Что правится: нарезка листа и девятина картинки целиком (.sage9) — у
// картинки без кусков девятина своя, у куска — в листе.
struct SpriteDoc {
    sage::ui::SpriteSheet Sheet;
    sage::ui::NineSlice Texture;
};

// Текст документа для сравнения «есть ли несохранённое».
std::string Fingerprint(const SpriteDoc& doc);

// ИСТОРИЯ ПРАВОК редактора — своя, не сцены: Ctrl+Z в редакторе спрайтов не
// откатывает объект сцены, и наоборот.
class SpriteHistory {
public:
    // Снимок ПЕРЕД правкой.
    void Push(const SpriteDoc& before);
    bool CanUndo() const { return !m_undo.empty(); }
    bool CanRedo() const { return !m_redo.empty(); }
    bool Undo(SpriteDoc& doc);
    bool Redo(SpriteDoc& doc);
    void Clear();
    int Depth() const { return (int)m_undo.size(); }

private:
    std::vector<SpriteDoc> m_undo, m_redo;
};

// Куски, которые даст сетка: только целиком внутри картинки и (если
// SkipEmpty) не пустые.
std::vector<glm::ivec4> GridRects(int width, int height, const sage::ui::SpriteGrid& grid,
                                  const unsigned char* rgba);

// Повторная нарезка поверх готовых кусков.
enum class SliceApply { Add, Replace };
// Заводит куски rects с именами prefix_N. Add — к имеющимся (кусок с тем же
// прямоугольником второй раз не заводится), Replace — вместо них. Возвращает,
// сколько кусков заведено.
int ApplySlices(sage::ui::SpriteSheet& sheet, const std::vector<glm::ivec4>& rects, const std::string& prefix,
                SliceApply how);

// Верхний кусок под точкой (пиксели листа), -1 — пусто.
int HitTest(const sage::ui::SpriteSheet& sheet, glm::vec2 px);
// Куски, задетые рамкой выделения.
std::vector<int> HitRect(const sage::ui::SpriteSheet& sheet, glm::ivec4 rect);

// Прямоугольник по двум углам, внутри картинки.
glm::ivec4 RectFromCorners(glm::ivec2 a, glm::ivec2 b, int width, int height);
// Ручки рамки: 0..7 — углы и середины сторон по часовой от левого верхнего.
enum Handle { HTopLeft, HTop, HTopRight, HRight, HBottomRight, HBottom, HBottomLeft, HLeft, HCount };
// Потянули ручку на delta пикселей: сторона не переходит через противоположную
// (кусок не короче 1 пикселя) и не выходит за картинку.
glm::ivec4 ResizeRect(glm::ivec4 rect, int handle, glm::ivec2 delta, int width, int height);
// Сдвиг куска целиком — не выходя за картинку.
glm::ivec4 MoveRect(glm::ivec4 rect, glm::ivec2 delta, int width, int height);

// ТОЧКА ОПОРЫ: девять готовых положений (по строкам: верх-лево … низ-право),
// иначе — своя.
constexpr int kPivotCustom = 9;
glm::vec2 PivotPreset(int index);
int PivotPresetOf(glm::vec2 pivot);

// Имя куска: не пустое и не занято другим куском.
bool NameFree(const sage::ui::SpriteSheet& sheet, const std::string& name, int self);

// Поля девятины в пределах куска w x h: не меньше нуля, лево + право и
// верх + низ не больше стороны.
glm::vec4 ClampBorder(glm::vec4 border, float w, float h);

// СОХРАНЕНИЕ: лист — «картинка.sagesheet», девятина картинки — «.sage9». Пишется
// только то, что есть: пустой лист не заводится рядом с картинкой, у которой
// его не было. Оба файла — атомарно (сначала временный, затем подмена).
bool SaveDoc(const std::string& imageDiskPath, const SpriteDoc& doc, std::string& err);
// Открыть: нет файлов — пустой документ (это не ошибка).
bool LoadDoc(const std::string& imageDiskPath, SpriteDoc& doc, std::string& err);

} // namespace sage::editor::sprite
