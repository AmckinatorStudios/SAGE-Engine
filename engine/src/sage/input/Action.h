#pragma once
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "sage/input/Binding.h"
#include "sage/input/Devices.h"

// ---------------------------------------------------------------------------
// ДЕЙСТВИЕ — НАМЕРЕНИЕ игрока, а не клавиша.
//
// Это главный (и почти единственный) интерфейс, с которым имеет дело игровая
// логика. «Прыгнуть», «Идти вперёд», «Осмотреться» — вещи, которые игра
// понимает; «пробел», «W» и «смещение мыши по X» — вещи, которые понимает
// железо. Пока код игры спрашивал про клавишу, переназначение управления было
// невозможно в принципе: клавиша была вписана в саму логику. Спросив про
// действие, игра получает то же самое поведение и от клавиатуры, и от
// геймпада, и от заново назначенной игроком кнопки — не изменившись ни на
// строку (§26 ТЗ).
//
// ТРИ ВИДА действий, потому что игре нужны ровно три ответа (§6):
//   Digital — «да или нет»: прыжок, выстрел, пауза.
//   Axis    — число −1..1: «вперёд-назад» с клавиш или с курка.
//   Vector  — Vector2: движение и обзор; WASD и левый стик отдают ОДНО И ТО ЖЕ.
//
// Действие ничего не знает про окно, GLFW и очереди событий: ему дают снимок
// состояния устройств, оно возвращает своё значение. Поэтому его можно
// проверить обычным тестом, без окна и без человека за клавиатурой.
// ---------------------------------------------------------------------------
namespace sage::input {

enum class ActionType : uint8_t { Digital, Axis, Vector };

// Когда именно действие «сработало» (§23). Одна и та же клавиша под двумя
// действиями с разными режимами даёт то, что в играх зовут «нажать/удержать»:
// E коротко — поговорить, E удержать — открыть расширенное меню.
enum class TriggerMode : uint8_t {
    Press,    // в момент нажатия (по умолчанию)
    Release,  // в момент отпускания
    Hold,     // один раз, когда удержание дошло до HoldTime
    Tap,      // при отпускании, если удержали меньше TapTime
};

// Как обрезать «дрожание» аналогового источника около нуля (§24).
enum class DeadZoneMode : uint8_t {
    Radial,  // по ДЛИНЕ вектора — правильный выбор для стика: иначе диагональ
             // проходит зону раньше, чем прямое направление, и персонаж
             // «срывается» наискосок
    Axial,   // по каждой оси отдельно — для раздельных осей и курков
};

// Фаза действия за кадр — один ответ вместо четырёх вопросов (§6.1).
enum class Phase : uint8_t { Released, Pressed, Held, ReleasedThisFrame };

struct ActionSettings {
    TriggerMode Trigger = TriggerMode::Press;
    float HoldTime = 0.4f;   // с какого удержания считается «удержано»
    float TapTime = 0.25f;   // короче этого — «коротко нажато»

    float DeadZone = 0.15f;
    DeadZoneMode Zone = DeadZoneMode::Radial;

    // Сглаживание аналогового значения, В СЕКУНДАХ времени приближения.
    // 0 — сырое значение, и это правильный выбор для обзора от первого лица:
    // сглаженный обзор ощущается как «мышь тонет». Сглаживание нужно другому —
    // движению с клавиш (мгновенный разгон с 0 до 1 выглядит рывком) и
    // разболтанному стику.
    float Smoothing = 0.0f;

    // Ограничить длину вектора единицей. Без этого движение по диагонали с
    // клавиатуры быстрее прямого в 1.41 раза — старейшая ошибка в играх.
    bool Normalize = true;

    float Sensitivity = 1.0f;

    // С какого значения аналоговый источник считается «нажатым», если его
    // привязали к цифровому действию (курок как кнопка выстрела).
    float PressThreshold = 0.5f;

    // Требовать ТОЧНОГО совпадения модификаторов. По умолчанию выключено, и
    // это осознанно: «W» обязана работать и с зажатым Shift, иначе бег ломает
    // ходьбу. Включается там, где нужен именно горячий ключ: у действия
    // «Сохранить» на CTRL+S не должно быть шанса сработать от CTRL+SHIFT+S.
    bool ExactModifiers = false;
};

// Какие устройства сейчас у действия ОТОБРАНЫ (см. §29: интерфейс забрал
// щелчок мыши, игра его не получает). Маска, а не флаг: обычный случай —
// «мышь занята интерфейсом, клавиатура свободна».
enum DeviceMask : uint8_t {
    DeviceNone     = 0,
    DeviceKeyboard = 1 << 0,
    DeviceMouse    = 1 << 1,
    DeviceGamepad  = 1 << 2,
    DeviceAll      = DeviceKeyboard | DeviceMouse | DeviceGamepad,
};

class Action {
public:
    Action() = default;
    Action(std::string name, ActionType type) : m_name(std::move(name)), m_type(type) {}

    const std::string& Name() const { return m_name; }
    ActionType Type() const { return m_type; }

    ActionSettings& Settings() { return m_settings; }
    const ActionSettings& Settings() const { return m_settings; }

    // --- Привязки ----------------------------------------------------------
    Action& Bind(const Binding& binding) { m_bindings.push_back(binding); return *this; }
    // Привязка ИМЕНЕМ ("SPACE", "-S", "PAD_A") — так её пишут в скрипте и в
    // файле настроек. Нераспознанное имя молча не добавляется; распозналась
    // ли она, говорит возвращаемое значение.
    bool Bind(const std::string& source);
    // Векторное действие: одна строка на направление (§6.3).
    Action& BindVector(const std::string& up, const std::string& down,
                       const std::string& left, const std::string& right);

    const std::vector<Binding>& Bindings() const { return m_bindings; }
    void ClearBindings() { m_bindings.clear(); }
    // Переназначение (§20): все прежние привязки заменяются одной новой.
    // Именно этого ждёт экран настроек — «теперь прыжок на Q», а не «прыжок
    // ещё и на Q».
    void Rebind(const Binding& binding) { m_bindings.clear(); m_bindings.push_back(binding); }
    bool Rebind(const std::string& source);
    // Убрать одну привязку по источнику. Нужно экрану настроек: назначая
    // клавишу, занятую другим действием, её сначала снимают там.
    bool Unbind(const Binding& binding);
    bool UsesSource(const Binding& binding) const;

    // --- Состояние за кадр -------------------------------------------------
    bool IsDown() const { return m_down; }
    bool IsHeld() const { return m_down; }              // имя из ТЗ (§12)
    bool WasPressed() const { return m_pressed; }        // стало нажатым в этом кадре
    bool WasReleased() const { return m_released; }      // отпущено в этом кадре
    // Сработало ПО ВЫБРАННОМУ РЕЖИМУ (Press/Release/Hold/Tap). При режиме по
    // умолчанию совпадает с WasPressed.
    bool Triggered() const { return m_triggered; }
    Phase CurrentPhase() const;

    // С каких устройств действие сейчас питается (маска DeviceMask). Нужно
    // потреблению (§29): контекст интерфейса забирает у нижних слоёв ровно то
    // устройство, которым его действие воспользовалось, — щелчок мыши, но не
    // клавиатуру.
    uint8_t ActiveDevices() const { return m_activeDevices; }
    float HeldTime() const { return m_heldTime; }

    float Value() const { return m_value.x; }
    glm::vec2 Vector() const { return m_value; }

    // --- Вычисление --------------------------------------------------------
    // Один раз за кадр, по снимку устройств. blocked — маска отобранных
    // устройств (§29): привязки к ним в этом кадре не считаются нажатыми.
    void Evaluate(const Devices& devices, float dt, uint8_t blocked = DeviceNone);

    // Всё отпущено и обнулено — кадр без ввода. Не «не звать Evaluate»: тогда
    // действие навсегда застыло бы нажатым.
    void Reset();

private:
    bool SourceDown(const Binding& b, const Devices& d, uint8_t blocked) const;
    float SourceValue(const Binding& b, const Devices& d, uint8_t blocked) const;
    bool ModifiersMatch(const Binding& b, const Devices& d) const;
    glm::vec2 ApplyDeadZone(glm::vec2 raw) const;

    std::string m_name;
    ActionType m_type = ActionType::Digital;
    ActionSettings m_settings;
    std::vector<Binding> m_bindings;

    bool m_down = false;
    bool m_pressed = false;
    bool m_released = false;
    bool m_triggered = false;
    bool m_holdFired = false;   // Hold срабатывает ОДИН раз за удержание
    float m_heldTime = 0.0f;
    uint8_t m_activeDevices = DeviceNone;
    glm::vec2 m_value{0.0f};
    glm::vec2 m_smoothed{0.0f};
};

} // namespace sage::input
