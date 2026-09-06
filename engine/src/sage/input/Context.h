#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "sage/input/Action.h"

// ---------------------------------------------------------------------------
// КОНТЕКСТ ВВОДА — набор действий, включаемый и выключаемый целиком.
//
// Одна и та же клавиша обязана значить разное в разных обстоятельствах: E в
// игре — «взаимодействовать», в инвентаре — «надеть», в диалоге — «дальше»
// (§8 ТЗ). Разложить это по условиям внутри игровой логики нельзя: условие
// пришлось бы повторить у каждого действия и не забыть ни одного, а забудут
// обязательно.
//
// ПРИОРИТЕТ И ПОТРЕБЛЕНИЕ (§9, §29). Контексты выстроены сверху вниз:
//
//     Editor UI   100
//     Game UI      80
//     Gameplay     10
//
// Верхний, воспользовавшийся устройством, ЗАБИРАЕТ его у нижних — щелчок по
// кнопке интерфейса не должен одновременно стрелять в игре. Забирается именно
// устройство, а не «ввод целиком»: обычный случай — интерфейс держит мышь,
// пока клавиатура остаётся у игры.
//
// Контекст не «слой ввода на всякий случай»: игра без интерфейса обходится
// одним контекстом по умолчанию и ни разу о них не вспоминает.
// ---------------------------------------------------------------------------
namespace sage::input {

class Context {
public:
    Context() = default;
    Context(std::string name, int priority) : m_name(std::move(name)), m_priority(priority) {}

    const std::string& Name() const { return m_name; }

    int Priority() const { return m_priority; }
    void SetPriority(int priority) { m_priority = priority; }

    // Выключенный контекст не считает свои действия и ничего не забирает —
    // его действия отпущены (см. Reset). Так меню, открытое над игрой, не
    // оставляет игрока идти вперёд.
    bool Enabled() const { return m_enabled; }
    void SetEnabled(bool enabled);

    // Какие устройства контекст забирает у нижних, КОГДА ими воспользовался.
    // По умолчанию — ничего: обычный игровой контекст ни с кем не спорит.
    uint8_t Blocks() const { return m_blocks; }
    void SetBlocks(uint8_t devices) { m_blocks = devices; }

    // --- Действия ----------------------------------------------------------
    //
    // Повторный Add с тем же именем возвращает существующее действие, а не
    // заводит второе: раскладку дописывают в нескольких местах (движок завёл
    // «Пауза», игра добавила ей кнопку геймпада), и вторая копия означала бы
    // действие-призрак, которое никто не читает.
    Action& Add(const std::string& name, ActionType type = ActionType::Digital);
    Action* Find(const std::string& name);
    const Action* Find(const std::string& name) const;
    bool Has(const std::string& name) const { return Find(name) != nullptr; }
    bool Remove(const std::string& name);

    // Имена действий в порядке ОБЪЯВЛЕНИЯ. Порядок хранения (хеш-таблица) для
    // человека случаен, а список действий видят и в настройках управления, и
    // в редакторе — прыгающий от запуска к запуску список нечитаем.
    const std::vector<std::string>& ActionNames() const { return m_order; }

    // Кто уже занял этот источник — вопрос экрана переназначения клавиш:
    // назначая занятую клавишу, её надо снять с прежнего действия.
    Action* FindByBinding(const Binding& binding);

    // --- Кадр --------------------------------------------------------------
    // Считает все действия и возвращает маску устройств, которые контекст
    // забирает у нижних (пусто, если он ничем не воспользовался).
    uint8_t Evaluate(const Devices& devices, float dt, uint8_t blocked);
    void Reset();

private:
    std::string m_name;
    int m_priority = 0;
    bool m_enabled = true;
    uint8_t m_blocks = DeviceNone;

    // unordered_map по имени плюс отдельный порядок объявления. Указатели на
    // Action обязаны переживать добавление новых действий (их держат игра и
    // скрипты), поэтому значения — unique_ptr, а не сам Action: перевыделение
    // таблицы иначе оставило бы висячие ссылки.
    std::unordered_map<std::string, std::unique_ptr<Action>> m_actions;
    std::vector<std::string> m_order;
};

} // namespace sage::input
