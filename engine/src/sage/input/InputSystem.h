#pragma once
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "sage/input/ActionEvent.h"
#include "sage/input/Context.h"
#include "sage/input/Devices.h"
#include "sage/input/InputEvent.h"

namespace sage::events { class Bus; class TypedBus; }

// ---------------------------------------------------------------------------
// ВВОД ДВИЖКА — одна система на игру, редактор и плеер.
//
// Собирает вместе всё, что описано в соседних файлах, и задаёт единственный
// порядок, в котором это работает:
//
//     платформа -> Push(событие)
//                     |
//     Update(dt):  устройства  ->  контексты по приоритету  ->  действия
//                     |                                            |
//                 подписчики сырых событий                   события действий
//                 (интерфейс: Consumed)                      (шина событий)
//
// СИСТЕМА НЕ ЗНАЕТ ПРО ОКНО. Ни GLFW, ни Window здесь не упоминаются: события
// кладёт снаружи тот, у кого окно есть (в игре — мост GlfwBridge, в редакторе
// — панель Game, в тесте — сам тест). Из этого следует главное практическое
// свойство: ввод проверяется обычным модульным тестом, без окна и без человека
// за клавиатурой, и ведёт себя в тесте ровно так же, как в игре (§1 ТЗ).
//
// РАЗДЕЛЕНИЕ РЕДАКТОРА И ИГРЫ (§27) достигается не флагом, а экземпляром: у
// редактора свой InputSystem со своими контекстами, у запущенной игры свой.
// Раскладка игры физически не может повлиять на горячие клавиши редактора,
// потому что это разные объекты.
// ---------------------------------------------------------------------------
namespace sage::input {

// Захват курсора — единственное, что ввод ДЕЛАЕТ с окном, а не читает из него.
// Интерфейс, а не прямая ссылка: в собранной игре захват означает «спрятать и
// приклеить курсор к окну», а в редакторе — «...но только пока панель Game в
// фокусе», иначе захваченный игрой курсор не даст нажать ни одной кнопки
// редактора.
class CursorControl {
public:
    virtual ~CursorControl() = default;
    virtual void SetCursorCaptured(bool captured) = 0;
    virtual bool CursorCaptured() const = 0;
};

// Имя контекста, который заводится сам и в котором оказываются действия,
// объявленные без указания контекста. Игре без меню контексты не нужны — она
// не должна о них узнавать.
inline constexpr const char* kDefaultContext = "Gameplay";

class InputSystem {
public:
    InputSystem();
    ~InputSystem();

    InputSystem(const InputSystem&) = delete;
    InputSystem& operator=(const InputSystem&) = delete;

    // --- Ввод снаружи -------------------------------------------------------
    //
    // Событие кладётся в очередь кадра, а не применяется на месте: применить
    // его сразу значило бы менять состояние устройств посреди кадра, и два
    // читателя одного действия увидели бы разное.
    void Push(const InputEvent& event);

    // Отобрать устройства на ТЕКУЩИЙ кадр (§29). Зовёт тот, кто ввод
    // перехватил и не является контекстом, — интерфейс редактора на ImGui:
    // пока курсор над панелью, мышь игре не принадлежит. Сбрасывается каждый
    // Update, поэтому «забыть вернуть» невозможно.
    void BlockDevices(uint8_t mask) { m_blocked |= mask; }

    // Один раз за кадр, до чтения любого действия. Короткая форма для тех, у
    // кого между устройствами и действиями ничего не стоит.
    void Update(float dt);

    // --- Тот же кадр, но в два шага ----------------------------------------
    //
    // Между «прочитали устройства» и «посчитали действия» встаёт интерфейс: он
    // должен успеть сказать, что съел щелчок, ДО того как из этого щелчка
    // получится выстрел. Одним Update это невыразимо — интерфейсу нужно
    // состояние устройств, а игре нужен ответ интерфейса, и оба в одном кадре.
    //
    //     BeginFrame();          // устройства и сырые события
    //     ...интерфейс...        // BlockDevices(DeviceMouse), если съел
    //     UpdateActions(dt);     // действия — уже без съеденного
    //
    // Порядок из §19 ТЗ ровно в этом и состоит.
    void BeginFrame();
    void UpdateActions(float dt);

    // Кадр без ввода: всё отпущено, дельты обнулены. Не «не звать Update»:
    // тогда действия навсегда застыли бы нажатыми, и игрок, вернувшийся в
    // окно, обнаружил бы, что всё это время шёл вперёд.
    void ReleaseAll();

    // --- Контексты (§8, §9) -------------------------------------------------
    // Снести ВСЕ контексты и действия, оставив один пустой контекст по
    // умолчанию. Нужно редактору: раскладку объявляют сами скрипты в OnStart,
    // и каждый новый запуск Play обязан начинаться с чистого листа — иначе
    // раскладка прошлого запуска переживает правку скрипта, и «удалил
    // BindAction, а клавиша работает» ищут долго.
    //
    // Подписки, курсор и шины при этом сохраняются: они принадлежат хозяину
    // системы, а не запущенной игре.
    void ClearActions();

    Context& CreateContext(const std::string& name, int priority);
    Context* FindContext(const std::string& name);
    bool RemoveContext(const std::string& name);
    void SetContextEnabled(const std::string& name, bool enabled);
    bool ContextEnabled(const std::string& name) const;
    // Контексты в порядке убывания приоритета — тот самый порядок, в котором
    // они получают ввод.
    std::vector<Context*> ContextsByPriority();
    std::vector<std::string> ContextNames() const;
    Context& Default() { return *m_default; }

    // --- Действия -----------------------------------------------------------
    //
    // Короткая форма: действие заводится в контексте по умолчанию. Игра без
    // меню так и живёт — Register("Jump").Bind("SPACE") и всё.
    Action& Register(const std::string& name, ActionType type = ActionType::Digital);
    Action& Register(const std::string& context, const std::string& name, ActionType type);

    // Поиск по ВСЕМ контекстам, начиная с самого приоритетного: одно имя может
    // быть объявлено в двух контекстах («Взаимодействовать» в игре и в
    // инвентаре), и отвечать должен тот, чей контекст сейчас главный.
    Action* Find(const std::string& name);
    const Action* Find(const std::string& name) const;
    bool Has(const std::string& name) const { return Find(name) != nullptr; }

    // Быстрые вопросы — то, чем игра пользуется каждый кадр. Неизвестное имя
    // не бросает исключение, а отвечает «нет»: раскладку объявляют скрипты, и
    // опечатка в имени действия не должна ронять игру посреди боя. Что имя
    // неизвестно, видно в логе (см. .cpp) и по Has().
    bool IsDown(const std::string& name) const;
    bool WasPressed(const std::string& name) const;
    bool WasReleased(const std::string& name) const;
    bool Triggered(const std::string& name) const;
    float Value(const std::string& name) const;
    glm::vec2 Vector(const std::string& name) const;

    // --- Переназначение (§20) -----------------------------------------------
    //
    // Возвращает false, если имя источника не распознано. Действие ищется по
    // всем контекстам.
    bool Rebind(const std::string& action, const std::string& source);
    bool AddBinding(const std::string& action, const std::string& source);
    // Кто уже занял этот источник (первый по приоритету контекстов). Экран
    // настроек обязан спросить это ДО назначения: молча отобрать клавишу у
    // другого действия — значит сломать управление, не сказав об этом.
    Action* FindByBinding(const Binding& binding);

    // --- Сохранение раскладки (§32) -----------------------------------------
    //
    // Строковые варианты — не только ради тестов: раскладка так же кладётся в
    // файл проекта и в сохранение настроек, а это разные хранилища.
    std::string SaveMappingToString() const;
    bool LoadMappingFromString(const std::string& json);
    bool SaveMapping(const std::filesystem::path& file) const;
    bool LoadMapping(const std::filesystem::path& file);

    // --- Сырой ввод ---------------------------------------------------------
    const Devices& State() const { return m_devices; }
    // Изменяемый доступ — только для платформенного моста: имя подключённого
    // геймпада приходит от оконного слоя, а не событием.
    Devices& MutableState() { return m_devices; }
    glm::vec2 MousePosition() const { return m_devices.MouseState().Position(); }
    glm::vec2 MouseDelta() const { return m_devices.MouseState().Delta(); }
    float Wheel() const { return m_devices.MouseState().Wheel(); }
    const std::vector<unsigned int>& TypedText() const { return m_devices.Keys().TypedText(); }

    void SetCursorControl(CursorControl* control) { m_cursor = control; }
    void SetCursorCaptured(bool captured);
    bool CursorCaptured() const;

    // --- События ------------------------------------------------------------
    //
    // Сырые события кадра — в порядке прихода, с пометкой Consumed. Тому, кто
    // разбирает их сам (поле ввода, панель редактора), они нужны целиком: по
    // состоянию устройств нельзя отличить «нажали дважды» от «держат».
    const std::vector<InputEvent>& FrameEvents() const { return m_frameEvents; }

    // Подписка на сырое событие ДО того, как его увидят действия. Обработчик
    // возвращает true, если событие потреблено (§29) — тогда ни следующие
    // подписчики, ни действия его не получат.
    using RawHandler = std::function<bool(const InputEvent&)>;
    int OnRawEvent(RawHandler handler, int priority = 0);
    void OffRawEvent(int id);

    // Подписка на события действий (§11).
    using ActionHandler = std::function<void(const ActionEvent&)>;
    int OnAction(ActionHandler handler);
    void OffAction(int id);

    // Куда ДОПОЛНИТЕЛЬНО слать события действий:
    //   именная шина — событием "input.<Действие>" со значением в аргументе;
    //     так действие слышат Lua и связи, настроенные в инспекторе;
    //   типизированная — структурой ActionEvent для кода на C++.
    // Обе необязательны: игра, которой хватает опроса, не платит за них ничем.
    void SetEventBus(sage::events::Bus* bus) { m_bus = bus; }
    void SetTypedBus(sage::events::TypedBus* bus) { m_typedBus = bus; }

    uint64_t Frame() const { return m_devices.Frame(); }

private:
    void DrainEvents();
    void PublishActionEvents();
    const Action* FindConst(const std::string& name) const;

    Devices m_devices;
    std::vector<std::unique_ptr<Context>> m_contexts;
    Context* m_default = nullptr;

    std::vector<InputEvent> m_pending;      // пришло между кадрами
    std::vector<InputEvent> m_frameEvents;  // разобрано в этом кадре

    struct RawSlot {
        int Id = 0;
        int Priority = 0;
        RawHandler Fn;
        bool Dead = false;
    };
    std::vector<RawSlot> m_rawSlots;
    struct ActionSlot {
        int Id = 0;
        ActionHandler Fn;
        bool Dead = false;
    };
    std::vector<ActionSlot> m_actionSlots;
    int m_nextSubscription = 1;

    uint8_t m_blocked = DeviceNone;

    CursorControl* m_cursor = nullptr;
    sage::events::Bus* m_bus = nullptr;
    sage::events::TypedBus* m_typedBus = nullptr;
};

} // namespace sage::input
