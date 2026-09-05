#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "sage/ui/core/UITypes.h"

class Texture;

// ---------------------------------------------------------------------------
// КОНТЕКСТ — всё внешнее, что нужно интерфейсу, чтобы посчитать и нарисовать
// себя: размер экрана, время, шрифты, картинки, переводы.
//
// ПОЧЕМУ ЭТО ОТДЕЛЬНЫЙ ОБЪЕКТ, А НЕ ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ (§136). Документ
// интерфейса не должен зависеть ни от «текущего экрана», ни от «текущего
// шрифта»: один и тот же документ считается для окна игры, для превью в
// редакторе и для снимка экрана — одновременно и с разными параметрами. Всё,
// что глобально, в этот момент ломается.
//
// ПОЧЕМУ ШРИФТЫ И КАРТИНКИ — ИНТЕРФЕЙСЫ, А НЕ КЛАССЫ ДВИЖКА (§138–140).
// Интерфейс обязан пользоваться ресурсной системой движка, а не заводить свою.
// Но модуль интерфейса при этом не должен ЗНАТЬ конкретный менеджер ресурсов:
// иначе его нельзя ни протестировать без GPU, ни переиспользовать в
// инструменте. Здесь описано, что интерфейсу НУЖНО (померить строку, взять
// текстуру), а кто это даст — решает вызывающий.
// ---------------------------------------------------------------------------
namespace sage::ui {

// Метрики шрифта, которых достаточно раскладке текста.
struct UIFontMetrics {
    float LineHeight = 0.0f; // шаг между строками при размере 1
    float Ascent = 0.0f;     // от базовой линии вверх
    float Descent = 0.0f;    // от базовой линии вниз (положительное число)
};

// Источник шрифтов. Ручка (handle) — непрозрачное число: чей это шрифт и как он
// устроен, интерфейс не знает и знать не должен.
class IUIFontSource {
public:
    virtual ~IUIFontSource() = default;

    // Ручка шрифта по семейству/пути. -1 — нет такого; вызывающий обязан уметь
    // работать дальше (§134: отсутствующий шрифт — предупреждение и запасной
    // вариант, а не пустой экран).
    virtual int Resolve(const std::string& family, int weight, bool italic) = 0;
    virtual int Fallback() const = 0; // шрифт по умолчанию, всегда существует

    virtual UIFontMetrics Metrics(int font) const = 0;
    // Ширина символа в единицах кегля 1. Отдельным символом, а не строкой:
    // раскладке нужны переносы, многоточие и позиция каретки, то есть ширина
    // каждого шага пера.
    virtual float Advance(int font, uint32_t codepoint) const = 0;
    virtual bool HasGlyph(int font, uint32_t codepoint) const = 0;
};

// Источник изображений.
class IUITextureSource {
public:
    virtual ~IUITextureSource() = default;
    // nullptr — нет такой картинки: рисующий покажет заглушку, а не упадёт.
    virtual const Texture* Get(const std::string& path) = 0;
    virtual glm::ivec2 Size(const std::string& path) = 0;
};

// Отладочные слои (§109). Флаги здесь, а не в редакторе: рисует их рантайм, и
// игра тоже вправе включить рамки в отладочной сборке.
enum UIDebugFlags : uint32_t {
    UIDebug_None      = 0,
    UIDebug_Bounds    = 1u << 0,
    UIDebug_Anchors   = 1u << 1,
    UIDebug_Pivots    = 1u << 2,
    UIDebug_Layout    = 1u << 3,
    UIDebug_Masks     = 1u << 4,
    UIDebug_Clip      = 1u << 5,
    UIDebug_Layers    = 1u << 6,
    UIDebug_Batches   = 1u << 7,
    UIDebug_Dirty     = 1u << 8,
    UIDebug_HitAreas  = 1u << 9,
};

struct UIContext {
    // Размер кадра В ПИКСЕЛЯХ. Логические координаты холста получаются из него
    // и настроек холста (см. UIDocument::ScaleFor).
    glm::vec2 ScreenPixels{1920.0f, 1080.0f};
    float DeltaTime = 0.0f;
    double Time = 0.0;

    IUIFontSource* Fonts = nullptr;
    IUITextureSource* Textures = nullptr;

    // Перевод строки по ключу (§108). Сам словарь интерфейс не хранит: он —
    // задача игры/редактора, и подменяется на лету при смене языка.
    std::function<std::string(const std::string& key)> Localize;

    uint32_t Debug = UIDebug_None;

    // Можно ли пользоваться промежуточными целями рисования. Выключено —
    // сложные эффекты честно деградируют, а не пытаются рисовать в никуда
    // (снимок экрана, тесты без GPU).
    bool AllowOffscreen = true;

    std::string Text(const std::string& key) const {
        return Localize ? Localize(key) : key;
    }
};

} // namespace sage::ui
