#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "sage/ui/core/UINode.h"
#include "sage/ui/core/UITypes.h"
#include "sage/ui/visual/UIGradient.h"

class Texture;

// ---------------------------------------------------------------------------
// КОМАНДА ОТРИСОВКИ (§84 ТЗ) — граница между архитектурой интерфейса и GPU.
//
// Рисующий бэкенд НЕ ВИДИТ ни узлов, ни компонентов, ни дерева: он получает
// плоский список команд. Из этого следуют три вещи, ради которых всё и
// затевалось:
//   • интерфейс можно нарисовать другим бэкендом (другой API, отладочный
//     вывод, снимок в файл), не трогая ни одной строки логики интерфейса;
//   • команды можно сортировать и объединять в батчи, ничего не зная о том,
//     откуда они взялись (§85);
//   • отрисовку можно проверить тестом, сравнивая СПИСОК КОМАНД, а не картинку.
//
// Команда описывает ФИГУРУ, а не «элемент». Кнопка не превращается в «команду
// кнопки»: она превращается в скруглённый прямоугольник, текст и, если надо,
// тень — то есть в то же самое, во что превращается любая другая композиция с
// тем же оформлением.
// ---------------------------------------------------------------------------
namespace sage::ui {

enum class UIPrimitive {
    Rect,      // прямоугольник со скруглениями (в т.ч. круг/эллипс)
    Border,    // обводка скруглённого прямоугольника
    Image,     // текстурированный квад
    NineSlice, // девятина (разворачивается бэкендом в девять квадов)
    Glyphs,    // строка глифов (текст)
    Polygon,   // произвольный многоугольник
    Ring,      // кольцо/дуга
    Line,
    Custom,    // пользовательский материал (§45)
};

// Что бэкенд должен сделать с целями рисования (§36, §112).
enum class UIPassOp {
    Draw,          // обычные команды
    BeginOffscreen,// начать рисовать поддерево в промежуточную цель
    EndOffscreen,  // закончить и применить эффекты
    Composite,     // положить результат обратно
};

// Глиф в командe текста: позиция и код символа. Атлас и метрики знает бэкенд —
// это его дело, а не дело интерфейса.
struct UIGlyphDraw {
    uint32_t Codepoint = 0;
    glm::vec2 Pos{0.0f, 0.0f}; // позиция пера на базовой линии
    float Size = 0.0f;
    int Font = -1;
    UIColor Color{1.0f, 1.0f, 1.0f, 1.0f};
};

// ФИГУРНАЯ МАСКА, уже переведённая в экранные пиксели и готовая к отправке в
// шейдер. Прямоугольные маски сюда не попадают — они выражаются ножницами и
// стоят дешевле; здесь только то, что ножницами выразить нельзя.
struct UIMaskShape {
    enum class Kind { RoundedRect, Ellipse, Texture, Gradient };

    Kind Form = Kind::RoundedRect;
    UIRect Rect{};
    UICorners Radius{0.0f};
    float Softness = 0.0f;
    bool Invert = false;
    const Texture* Tex = nullptr;
    int Channel = 0; // 0 альфа, 1 R, 2 G, 3 B, 4 яркость
    float GradientAngle = 0.0f;
    float GradientStart = 0.0f, GradientEnd = 1.0f;
};

// Состояние маски для команды: готовый прямоугольник ножниц плюс, если нужна,
// фигурная маска. Бэкенд не обязан ходить по дереву — всё уже здесь.
struct UIClipState {
    UIRect Scissor{};
    bool HasScissor = false;
    int MaskState = 0;
    // Владеет им список команд (см. UIRenderList::AddMask): указатель обязан
    // пережить кадр, а копия в каждой команде стоила бы дороже самой маски.
    const UIMaskShape* Shape = nullptr;
};

// Пользовательский материал (§44, §45). Ядро интерфейса не знает, что внутри
// шейдера, и не должно: его дело — донести до бэкенда имя и параметры.
struct UIMaterialRef {
    std::string Shader;
    // Что именно рисуем этим материалом: имя значка, имя эффекта, ключ набора.
    // Строка живёт в материале, а не в команде: материалов на кадр единицы, а
    // команд сотни, и строка в каждой стоила бы дороже всего остального.
    std::string Name;
    const Texture* Textures[4] = {nullptr, nullptr, nullptr, nullptr};
    glm::vec4 Params[4]{};
    UIBlendMode Blend = UIBlendMode::Normal;
};

struct UIRenderCommand {
    UIPrimitive Kind = UIPrimitive::Rect;
    UIPassOp Op = UIPassOp::Draw;

    // Прямоугольник В ЭКРАННЫХ ПИКСЕЛЯХ (до собственного преобразования узла).
    UIRect Rect{};
    // Преобразование узла: сдвиг/поворот/масштаб/наклон. Единичная матрица —
    // обычный случай, и бэкенд вправе идти по быстрому пути.
    glm::mat3 Transform{1.0f};
    bool Transformed = false;

    UICorners Radius{0.0f};
    float Thickness = 0.0f;  // для Border/Ring
    float Softness = 0.0f;   // мягкость края: тени, свечения, растворение
    // Внутренняя тень: свечение считается ВНУТРЬ фигуры.
    bool Inner = false;

    UIColor Color{1.0f, 1.0f, 1.0f, 1.0f};
    // Готовый градиент. Разворачивается бэкендом в цвета вершин или в
    // параметры шейдера — это его выбор, а не интерфейса.
    UIGradient Gradient;

    const Texture* Tex = nullptr;
    glm::vec4 Uv{0.0f, 0.0f, 1.0f, 1.0f};
    UIEdges Slice{0.0f, 0.0f, 0.0f, 0.0f}; // для NineSlice, в пикселях исходника
    glm::vec2 SourceSize{0.0f, 0.0f};
    bool PixelArt = false;

    // Точки для Polygon/Line В ЭКРАННЫХ ПИКСЕЛЯХ.
    std::vector<glm::vec2> Points;
    float StartAngle = 0.0f, SweepAngle = 360.0f; // для Ring/Arc

    // Диапазон в общем массиве глифов списка (см. UIRenderList::Glyphs).
    int GlyphFirst = 0, GlyphCount = 0;

    UIClipState Clip;
    UIBlendMode Blend = UIBlendMode::Normal;
    const UIMaterialRef* Material = nullptr;

    uint64_t SortKey = 0;
    UINodeId Owner = 0; // кто породил команду — для отладки и профайлера
};

// Батч: подряд идущие команды с одинаковым состоянием (§85). Ключ батча — не
// «тип элемента», а именно СОСТОЯНИЕ GPU: текстура, режим наложения, маска,
// материал, цель рисования. Всё, что состояние не меняет, склеивается.
struct UIRenderBatch {
    int First = 0, Count = 0;
    const Texture* Tex = nullptr;
    UIBlendMode Blend = UIBlendMode::Normal;
    UIClipState Clip;
    const UIMaterialRef* Material = nullptr;
    // В батче есть текст. Информационно (глифы и заливки идут одним
    // шейдером и состояние не меняют) — нужно профайлеру и отладке.
    bool Text = false;
};

} // namespace sage::ui
