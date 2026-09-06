#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "sage/ui/core/UITypes.h"
#include "sage/ui/icons/SageIcons.generated.h"

namespace sage::rhi { class Texture2D; }

// ---------------------------------------------------------------------------
// СИСТЕМА ЗНАЧКОВ SAGE (ТЗ «SAGE Icon System»).
//
// ЧТО ЗДЕСЬ ГЛАВНОЕ. Значки — это сотни маленьких картинок на экране, и почти
// всё, что с ними делают неправильно, стоит дорого именно в мелочах: папка с
// тысячами SVG и разбор XML при запуске; по текстуре на значок и, значит, по
// вызову рисования на кнопку; по картинке на каждый цвет состояния.
//
// Поэтому здесь ровно одно решение, проведённое до конца: нужные значки
// (engine/icons/icons.json) собраны СБОРЩИКОМ в один атлас покрытия и вшиты в
// двоичный файл движка. В рантайме нет ни SVG, ни чтения с диска, ни поиска по
// строкам: Icon — это перечисление, а обращение к нему — сложение адреса.
//
// ЦВЕТА У ЗНАЧКА НЕТ. Атлас хранит ПОКРЫТИЕ (один байт на пиксель, как атлас
// шрифта), а цвет даёт интерфейс — тем же умножением, каким красится текст.
// Шесть состояний (обычное, наведение, нажатие, выключено, выбрано, акцент) —
// это шесть цветов и ОДНА текстура, а не шесть текстур.
//
// БАТЧИНГ. Все значки кадра лежат в одном атласе, значит рисуются одной
// привязкой текстуры — как и весь текст. Сто кнопок со значками не стоят ста
// вызовов рисования.
//
// TABLER — ИСХОДНЫЙ РЕСУРС, А НЕ ЗАВИСИМОСТЬ. Набор нужен только сборщику.
// Имя в коде (Icon::Play) отвязано от имени рисунка (player-play): поменять
// рисунок — это правка одной строки манифеста, без единой правки в редакторе.
// ---------------------------------------------------------------------------
namespace sage::ui::icons {

// Ячейка значка в атласе: левый верхний угол в пикселях, по одному на размер.
struct IconCell {
    struct XY { uint16_t X, Y; };
    XY Cells[kAtlasCount];
};

// Атлас: размер ячейки, размеры текстуры и сами пиксели (R8).
struct IconAtlasData {
    int Size;
    int Width, Height;
    const unsigned char* Pixels;
    unsigned PixelCount;
};

// Что получает интерфейс. Ничего, кроме текстуры и четырёх чисел: ни строки,
// ни указателя на «объект значка», который пришлось бы освобождать.
struct IconHandle {
    // Индекс атласа среди загруженных. Ноль — основной атлас SAGE.
    uint32_t Atlas = 0;
    float U0 = 0.0f, V0 = 0.0f, U1 = 0.0f, V1 = 0.0f;
    bool Valid() const { return U1 > U0 && V1 > V0; }
};

// ---------------------------------------------------------------------------
// Реестр. Один на процесс, как и атлас текстур.
// ---------------------------------------------------------------------------
class IconRegistry {
public:
    static IconRegistry& Instance();

    // Координаты значка в атласе, ближайшем по размеру к запрошенному
    // (§11: 16/20/24 вместо одной огромной текстуры под каждый масштаб).
    IconHandle Handle(Icon icon, float pixelSize = 20.0f) const;
    // По имени — для плагинов, документов интерфейса и редактора: там значок
    // приходит строкой из файла и статически известен быть не может.
    IconHandle Handle(const std::string& name, float pixelSize = 20.0f) const;
    bool Has(const std::string& name) const;

    const char* Name(Icon icon) const;
    // Все имена — для палитры редактора и проверки «такой значок есть».
    std::vector<std::string> Names() const;

    // Текстура атласа. Создаётся при первом обращении и живёт до конца
    // процесса: заново загружать её неоткуда — пиксели вшиты в двоичный файл.
    sage::rhi::Texture2D* Texture(uint32_t atlas) const;

    // --- Плагины (§16) ------------------------------------------------------
    //
    // Плагин приносит СВОЙ атлас и не трогает основной. Возвращается его номер;
    // дальше значки плагина живут по именам с его префиксом.
    uint32_t AddAtlas(const std::string& prefix, const IconAtlasData& atlas,
                      const std::vector<std::string>& names, const std::vector<IconCell>& cells);

private:
    IconRegistry();
    struct Extra {
        std::string Prefix;
        IconAtlasData Data{};
        std::vector<std::string> Names;
        std::vector<IconCell> Cells;
    };
    int AtlasIndexFor(float pixelSize) const;

    std::vector<Extra> m_extra;
    mutable std::vector<sage::rhi::Texture2D*> m_textures;
};

// Короткий доступ: Icons::Get(Icon::Play).
namespace Icons {
inline IconHandle Get(Icon icon, float pixelSize = 20.0f) {
    return IconRegistry::Instance().Handle(icon, pixelSize);
}
inline IconHandle Get(const std::string& name, float pixelSize = 20.0f) {
    return IconRegistry::Instance().Handle(name, pixelSize);
}
inline bool Has(const std::string& name) { return IconRegistry::Instance().Has(name); }
// Имя значка перечисления — единственный законный способ получить строку из
// Icon. Нужен там, где значок обязан попасть в ДОКУМЕНТ (файл интерфейса
// хранит имя, а не номер: номер поехал бы при следующей пересборке набора).
inline const char* Name(Icon icon) { return IconRegistry::Instance().Name(icon); }
} // namespace Icons

namespace generated {
extern const IconAtlasData kAtlases[];
extern const char* const kNames[];
extern const IconCell kCells[];
} // namespace generated

} // namespace sage::ui::icons
