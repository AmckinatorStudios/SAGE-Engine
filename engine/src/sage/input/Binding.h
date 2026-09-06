#pragma once
#include <cstdint>
#include <optional>
#include <string>

#include "sage/input/Keys.h"

// ---------------------------------------------------------------------------
// ПРИВЯЗКА — один физический источник, из которого действие берёт сигнал.
//
// Клавиша, кнопка мыши, направление колеса, ось мыши, кнопка геймпада или его
// ось — ОДИН тип с полем Kind, а не шесть разных. Действию всё равно, откуда
// пришёл сигнал: «Прыжок» одинаково законно повесить на пробел, на кнопку A
// геймпада и на среднюю кнопку мыши (§21 ТЗ), и разные типы заставили бы
// каждого потребителя перебирать шесть списков вместо одного.
//
// ТРИ ПОЛЯ СВЕРХ ИСТОЧНИКА, и каждое отвечает на реальный вопрос:
//   Modifiers — «Ctrl+S» это не «S» (§22). Без маски модификаторов сохранение
//               срабатывало бы при каждом наборе буквы s в имени файла.
//   Scale     — вклад в аналоговое значение: W даёт +1, S даёт −1 (§6.2).
//               Без него ось «вперёд-назад» пришлось бы собирать из двух
//               действий и вычитать их в каждой игре заново.
//   Component — какая половина вектора: X или Y (§6.3). Так «Движение» из
//               WASD и «Движение» с левого стика отдают игре ОДИН И ТОТ ЖЕ
//               Vector2, и игровой код не знает, чем играют.
// ---------------------------------------------------------------------------
namespace sage::input {

enum class SourceKind : uint8_t {
    None = 0,
    Key,            // клавиша клавиатуры
    MouseButton,    // кнопка мыши
    MouseWheel,     // тик колеса: Code 0 — вверх, 1 — вниз
    MouseAxis,      // смещение мыши за кадр: Code 0 — X, 1 — Y
    GamepadButton,  // кнопка геймпада
    GamepadAxis,    // стик или курок геймпада
};

// Какую половину вектора питает привязка (§6.3).
enum class Component : uint8_t { X = 0, Y = 1 };

struct Binding {
    SourceKind Kind = SourceKind::None;
    // Код внутри вида: значение Key / MouseButton / GamepadButton /
    // GamepadAxis, направление колеса или номер оси мыши. Одно поле, а не
    // шесть: сравнение, сериализация и поиск «кто занял эту клавишу» тогда
    // пишутся один раз, а не по разу на каждый вид устройства.
    uint16_t Code = 0;

    uint8_t Modifiers = ModNone;
    // Номер геймпада; −1 — «любой подключённый». Это и есть нужное поведение
    // одиночной игры: человек воткнул джойстик в любой порт и играет, не
    // выбирая слот.
    int8_t Gamepad = -1;

    float Scale = 1.0f;
    Component Axis = Component::X;

    // --- Короткие конструкторы --------------------------------------------
    static Binding OfKey(Key key, uint8_t mods = ModNone) {
        Binding b; b.Kind = SourceKind::Key; b.Code = (uint16_t)key; b.Modifiers = mods; return b;
    }
    static Binding OfMouse(MouseButton button, uint8_t mods = ModNone) {
        Binding b; b.Kind = SourceKind::MouseButton; b.Code = (uint16_t)button; b.Modifiers = mods; return b;
    }
    static Binding WheelUp() { Binding b; b.Kind = SourceKind::MouseWheel; b.Code = 0; return b; }
    static Binding WheelDown() { Binding b; b.Kind = SourceKind::MouseWheel; b.Code = 1; return b; }
    static Binding MouseAxisX() { Binding b; b.Kind = SourceKind::MouseAxis; b.Code = 0; return b; }
    static Binding MouseAxisY() { Binding b; b.Kind = SourceKind::MouseAxis; b.Code = 1; return b; }
    static Binding OfPadButton(GamepadButton button, int8_t pad = -1) {
        Binding b; b.Kind = SourceKind::GamepadButton; b.Code = (uint16_t)button; b.Gamepad = pad; return b;
    }
    static Binding OfPadAxis(GamepadAxis axis, int8_t pad = -1) {
        Binding b; b.Kind = SourceKind::GamepadAxis; b.Code = (uint16_t)axis; b.Gamepad = pad; return b;
    }

    // Тот же источник, но с другим вкладом/половиной вектора — чтобы собирать
    // оси и векторы в одну строку, не заводя переменную под каждую привязку.
    Binding With(float scale) const { Binding b = *this; b.Scale = scale; return b; }
    Binding On(Component component) const { Binding b = *this; b.Axis = component; return b; }
    Binding WithMods(uint8_t mods) const { Binding b = *this; b.Modifiers = mods; return b; }

    // Аналоговый источник (стик, курок, смещение мыши) — от кнопки отличается
    // тем, что у него есть промежуточные значения, а значит нужны мёртвая зона
    // и сглаживание, а не «нажато/не нажато».
    bool IsAnalog() const { return Kind == SourceKind::GamepadAxis || Kind == SourceKind::MouseAxis; }

    Key AsKey() const { return (Key)Code; }
    input::MouseButton AsMouseButton() const { return (input::MouseButton)Code; }
    input::GamepadButton AsPadButton() const { return (input::GamepadButton)Code; }
    input::GamepadAxis AsPadAxis() const { return (input::GamepadAxis)Code; }
    bool WheelIsUp() const { return Code == 0; }

    // Равенство — по ИСТОЧНИКУ, без Scale/Axis: вопрос «эта клавиша уже
    // занята?» в настройках управления не должен зависеть от того, с каким
    // знаком её вклад в ось.
    bool SameSource(const Binding& other) const {
        return Kind == other.Kind && Code == other.Code && Modifiers == other.Modifiers &&
               Gamepad == other.Gamepad;
    }
    bool operator==(const Binding& other) const {
        return SameSource(other) && Scale == other.Scale && Axis == other.Axis;
    }

    // Человекочитаемая запись источника: "CTRL+S", "MOUSE_LEFT", "WHEEL_UP",
    // "PAD_A", "PAD_LEFT_X". Именно она попадает в файл настроек и на экран
    // переназначения клавиш — число там не прочитает никто.
    std::string ToString() const;

    // Разбор той же записи. Понимает ведущий минус ("-S") как вклад −1: ось
    // «вперёд-назад» из клавиш описывается одной строкой на привязку, а не
    // парой «строка + отдельно знак».
    // Возвращает пусто, если источник не распознан — вызывающий решает, что с
    // этим делать (скрипт пишет предупреждение, редактор красит поле).
    static std::optional<Binding> Parse(const std::string& text);
};

} // namespace sage::input
