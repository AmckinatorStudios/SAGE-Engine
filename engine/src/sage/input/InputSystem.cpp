#include "sage/input/InputSystem.h"

#include <algorithm>
#include <fstream>

#include <nlohmann/json.hpp>

#include "sage/core/Log.h"
#include "sage/events/Events.h"
#include "sage/events/TypedBus.h"

namespace sage::input {

namespace {

// Приоритет контекста по умолчанию. Десять, а не ноль: под игрой почти всегда
// заводят что-то ещё (отладочный оверлей, читы), и оставить ей самый низ
// значило бы заставить каждого писать отрицательные числа.
constexpr int kDefaultPriority = 10;

const char* TypeName(ActionType type) {
    switch (type) {
        case ActionType::Digital: return "digital";
        case ActionType::Axis:    return "axis";
        case ActionType::Vector:  return "vector";
    }
    return "digital";
}

ActionType ParseType(const std::string& name) {
    if (name == "axis") return ActionType::Axis;
    if (name == "vector") return ActionType::Vector;
    return ActionType::Digital;
}

const char* TriggerName(TriggerMode mode) {
    switch (mode) {
        case TriggerMode::Press:   return "press";
        case TriggerMode::Release: return "release";
        case TriggerMode::Hold:    return "hold";
        case TriggerMode::Tap:     return "tap";
    }
    return "press";
}

TriggerMode ParseTrigger(const std::string& name) {
    if (name == "release") return TriggerMode::Release;
    if (name == "hold") return TriggerMode::Hold;
    if (name == "tap") return TriggerMode::Tap;
    return TriggerMode::Press;
}

} // namespace

InputSystem::InputSystem() {
    m_default = &CreateContext(kDefaultContext, kDefaultPriority);
}

InputSystem::~InputSystem() = default;

// --- События снаружи --------------------------------------------------------

void InputSystem::Push(const InputEvent& event) { m_pending.push_back(event); }

// --- Контексты --------------------------------------------------------------

void InputSystem::ClearActions() {
    m_contexts.clear();
    m_default = &CreateContext(kDefaultContext, kDefaultPriority);
    // Незабранные события прошлого запуска не должны достаться следующему:
    // клавиша, нажатая на кнопке Stop, иначе доедет до только что стартовавшей
    // игры.
    m_pending.clear();
    m_frameEvents.clear();
    m_blocked = DeviceNone;
}

Context& InputSystem::CreateContext(const std::string& name, int priority) {
    if (Context* existing = FindContext(name)) {
        existing->SetPriority(priority);
        return *existing;
    }
    m_contexts.push_back(std::make_unique<Context>(name, priority));
    return *m_contexts.back();
}

Context* InputSystem::FindContext(const std::string& name) {
    for (auto& ctx : m_contexts)
        if (ctx->Name() == name) return ctx.get();
    return nullptr;
}

bool InputSystem::RemoveContext(const std::string& name) {
    // Контекст по умолчанию не удаляется: в нём живут действия, объявленные
    // без имени контекста, и его исчезновение оставило бы игру без ввода
    // молча.
    if (name == kDefaultContext) return false;
    const size_t before = m_contexts.size();
    m_contexts.erase(std::remove_if(m_contexts.begin(), m_contexts.end(),
                                    [&](const std::unique_ptr<Context>& c) { return c->Name() == name; }),
                     m_contexts.end());
    return m_contexts.size() != before;
}

void InputSystem::SetContextEnabled(const std::string& name, bool enabled) {
    if (Context* ctx = FindContext(name)) ctx->SetEnabled(enabled);
}

bool InputSystem::ContextEnabled(const std::string& name) const {
    for (const auto& ctx : m_contexts)
        if (ctx->Name() == name) return ctx->Enabled();
    return false;
}

std::vector<Context*> InputSystem::ContextsByPriority() {
    std::vector<Context*> out;
    out.reserve(m_contexts.size());
    for (auto& ctx : m_contexts) out.push_back(ctx.get());
    // Устойчивая сортировка: два контекста с одинаковым приоритетом получают
    // ввод в порядке объявления, а не в случайном — иначе поведение игры
    // меняется от сборки к сборке без единой правки.
    std::stable_sort(out.begin(), out.end(),
                     [](const Context* a, const Context* b) { return a->Priority() > b->Priority(); });
    return out;
}

std::vector<std::string> InputSystem::ContextNames() const {
    std::vector<std::string> out;
    for (const auto& ctx : m_contexts) out.push_back(ctx->Name());
    return out;
}

// --- Действия ---------------------------------------------------------------

Action& InputSystem::Register(const std::string& name, ActionType type) {
    return m_default->Add(name, type);
}

Action& InputSystem::Register(const std::string& context, const std::string& name, ActionType type) {
    Context* ctx = FindContext(context);
    if (!ctx) ctx = &CreateContext(context, kDefaultPriority);
    return ctx->Add(name, type);
}

Action* InputSystem::Find(const std::string& name) {
    for (Context* ctx : ContextsByPriority()) {
        if (Action* action = ctx->Find(name)) return action;
    }
    return nullptr;
}

const Action* InputSystem::Find(const std::string& name) const { return FindConst(name); }

const Action* InputSystem::FindConst(const std::string& name) const {
    // Тот же порядок, что и в неконстантной версии, но без сортировки копии:
    // ищем максимум приоритета вручную, чтобы константный путь не выделял
    // память — его зовут по нескольку раз за кадр из игровой логики.
    const Action* best = nullptr;
    int bestPriority = 0;
    for (const auto& ctx : m_contexts) {
        const Action* action = ctx->Find(name);
        if (!action) continue;
        if (!best || ctx->Priority() > bestPriority) {
            best = action;
            bestPriority = ctx->Priority();
        }
    }
    return best;
}

bool InputSystem::IsDown(const std::string& name) const {
    const Action* a = FindConst(name);
    return a && a->IsDown();
}
bool InputSystem::WasPressed(const std::string& name) const {
    const Action* a = FindConst(name);
    return a && a->WasPressed();
}
bool InputSystem::WasReleased(const std::string& name) const {
    const Action* a = FindConst(name);
    return a && a->WasReleased();
}
bool InputSystem::Triggered(const std::string& name) const {
    const Action* a = FindConst(name);
    return a && a->Triggered();
}
float InputSystem::Value(const std::string& name) const {
    const Action* a = FindConst(name);
    return a ? a->Value() : 0.0f;
}
glm::vec2 InputSystem::Vector(const std::string& name) const {
    const Action* a = FindConst(name);
    return a ? a->Vector() : glm::vec2(0.0f);
}

// --- Переназначение ---------------------------------------------------------

bool InputSystem::Rebind(const std::string& action, const std::string& source) {
    Action* target = Find(action);
    if (!target) {
        LOG_WARN("Input") << "Переназначение: действие '" << action << "' не объявлено";
        return false;
    }
    if (!target->Rebind(source)) {
        LOG_WARN("Input") << "Переназначение '" << action << "': неизвестный источник '" << source << "'";
        return false;
    }
    return true;
}

bool InputSystem::AddBinding(const std::string& action, const std::string& source) {
    Action* target = Find(action);
    if (!target) {
        LOG_WARN("Input") << "Привязка: действие '" << action << "' не объявлено";
        return false;
    }
    if (!target->Bind(source)) {
        LOG_WARN("Input") << "Привязка '" << action << "': неизвестный источник '" << source << "'";
        return false;
    }
    return true;
}

Action* InputSystem::FindByBinding(const Binding& binding) {
    for (Context* ctx : ContextsByPriority()) {
        if (Action* action = ctx->FindByBinding(binding)) return action;
    }
    return nullptr;
}

// --- Кадр -------------------------------------------------------------------

void InputSystem::DrainEvents() {
    m_frameEvents.clear();
    m_frameEvents.swap(m_pending);

    // Подписчики сырых событий — по убыванию приоритета. Живой снимок, потому
    // что обработчик волен отписаться прямо во время разбора.
    std::vector<RawSlot*> slots;
    for (RawSlot& s : m_rawSlots)
        if (!s.Dead) slots.push_back(&s);
    std::stable_sort(slots.begin(), slots.end(),
                     [](const RawSlot* a, const RawSlot* b) { return a->Priority > b->Priority; });

    for (InputEvent& e : m_frameEvents) {
        for (RawSlot* slot : slots) {
            if (slot->Dead || !slot->Fn) continue;
            if (slot->Fn(e)) {
                e.Consumed = true;
                break;
            }
        }
        // В состояние устройств событие попадает ВСЕГДА, даже потреблённое:
        // состояние — это правда о железе («Shift сейчас зажат»), а не о том,
        // кому этот ввод достался. Кому он достался, решают маски контекстов
        // (см. Context::Blocks) — иначе потреблённое интерфейсом отпускание
        // клавиши оставило бы её вечно нажатой для всех остальных.
        m_devices.Handle(e);
    }

    m_rawSlots.erase(std::remove_if(m_rawSlots.begin(), m_rawSlots.end(),
                                    [](const RawSlot& s) { return s.Dead; }),
                     m_rawSlots.end());
}

void InputSystem::BeginFrame() {
    m_devices.BeginFrame();
    DrainEvents();
}

void InputSystem::UpdateActions(float dt) {
    // Устройства, отобранные интерфейсом на этот кадр, плюс то, что заберут
    // контексты повыше.
    uint8_t blocked = m_blocked;
    for (Context* ctx : ContextsByPriority()) {
        blocked |= ctx->Evaluate(m_devices, dt, blocked);
    }
    m_blocked = DeviceNone; // маска живёт ровно кадр — «забыть вернуть» нельзя

    PublishActionEvents();
}

void InputSystem::Update(float dt) {
    BeginFrame();
    UpdateActions(dt);
}

void InputSystem::ReleaseAll() {
    m_pending.clear();
    m_frameEvents.clear();
    m_devices.ReleaseAll();
    for (auto& ctx : m_contexts) ctx->Reset();
    m_blocked = DeviceNone;
}

// --- События действий -------------------------------------------------------

void InputSystem::PublishActionEvents() {
    const bool anyListener = !m_actionSlots.empty() || m_bus || m_typedBus;
    if (!anyListener) return;

    for (auto& ctx : m_contexts) {
        if (!ctx->Enabled()) continue;
        for (const std::string& name : ctx->ActionNames()) {
            const Action* action = ctx->Find(name);
            if (!action) continue;
            // Сообщаем об ИЗМЕНЕНИИ, а не о состоянии: непрерывное действие
            // («Движение») иначе слало бы событие каждый кадр — шестьдесят
            // сообщений в секунду на пустом месте.
            if (!action->WasPressed() && !action->WasReleased() && !action->Triggered()) continue;

            ActionEvent e;
            e.Action = name;
            e.Context = ctx->Name();
            e.State = action->CurrentPhase();
            e.Triggered = action->Triggered();
            e.Value = action->Value();
            e.Vector = action->Vector();

            for (ActionSlot& slot : m_actionSlots) {
                if (!slot.Dead && slot.Fn) slot.Fn(e);
            }
            if (m_typedBus) m_typedBus->Publish(e);
            if (m_bus) {
                // Именное событие — для тех, кто говорит данными: Lua и связи,
                // настроенные в инспекторе. Префикс "input." не украшение: без
                // него действие «Пауза» столкнулось бы с игровым событием
                // «Пауза», и оба сработали бы друг на друге.
                const std::string base = "input." + name;
                if (action->Triggered()) m_bus->Emit(base, sage::vars::Value(e.Value));
                if (action->WasReleased()) m_bus->Emit(base + ".released", sage::vars::Value(e.Value));
            }
        }
    }

    m_actionSlots.erase(std::remove_if(m_actionSlots.begin(), m_actionSlots.end(),
                                       [](const ActionSlot& s) { return s.Dead; }),
                        m_actionSlots.end());
}

int InputSystem::OnRawEvent(RawHandler handler, int priority) {
    if (!handler) return 0;
    const int id = m_nextSubscription++;
    m_rawSlots.push_back(RawSlot{id, priority, std::move(handler), false});
    return id;
}

void InputSystem::OffRawEvent(int id) {
    for (RawSlot& s : m_rawSlots)
        if (s.Id == id) s.Dead = true;
}

int InputSystem::OnAction(ActionHandler handler) {
    if (!handler) return 0;
    const int id = m_nextSubscription++;
    m_actionSlots.push_back(ActionSlot{id, std::move(handler), false});
    return id;
}

void InputSystem::OffAction(int id) {
    for (ActionSlot& s : m_actionSlots)
        if (s.Id == id) s.Dead = true;
}

// --- Курсор -----------------------------------------------------------------

void InputSystem::SetCursorCaptured(bool captured) {
    if (m_cursor) m_cursor->SetCursorCaptured(captured);
}

bool InputSystem::CursorCaptured() const { return m_cursor && m_cursor->CursorCaptured(); }

// --- Сохранение раскладки ---------------------------------------------------

std::string InputSystem::SaveMappingToString() const {
    nlohmann::json root;
    root["version"] = 1;
    nlohmann::json contexts = nlohmann::json::array();
    for (const auto& ctx : m_contexts) {
        nlohmann::json jctx;
        jctx["name"] = ctx->Name();
        jctx["priority"] = ctx->Priority();
        jctx["enabled"] = ctx->Enabled();
        jctx["blocks"] = (int)ctx->Blocks();
        nlohmann::json actions = nlohmann::json::array();
        for (const std::string& name : ctx->ActionNames()) {
            const Action* action = ctx->Find(name);
            if (!action) continue;
            const ActionSettings& s = action->Settings();
            nlohmann::json ja;
            ja["name"] = name;
            ja["type"] = TypeName(action->Type());
            ja["trigger"] = TriggerName(s.Trigger);
            ja["holdTime"] = s.HoldTime;
            ja["tapTime"] = s.TapTime;
            ja["deadZone"] = s.DeadZone;
            ja["zone"] = (s.Zone == DeadZoneMode::Axial) ? "axial" : "radial";
            ja["smoothing"] = s.Smoothing;
            ja["normalize"] = s.Normalize;
            ja["sensitivity"] = s.Sensitivity;
            ja["pressThreshold"] = s.PressThreshold;
            ja["exactModifiers"] = s.ExactModifiers;
            nlohmann::json bindings = nlohmann::json::array();
            for (const Binding& b : action->Bindings()) {
                nlohmann::json jb;
                // Источник — СТРОКОЙ ("CTRL+S", "PAD_A"): в файле, который
                // правит человек, число не читается и не переживает вставку
                // новой клавиши в середину перечисления.
                jb["source"] = b.ToString();
                jb["axis"] = (b.Axis == Component::Y) ? "y" : "x";
                jb["gamepad"] = (int)b.Gamepad;
                bindings.push_back(jb);
            }
            ja["bindings"] = bindings;
            actions.push_back(ja);
        }
        jctx["actions"] = actions;
        contexts.push_back(jctx);
    }
    root["contexts"] = contexts;
    return root.dump(2);
}

bool InputSystem::LoadMappingFromString(const std::string& json) {
    nlohmann::json root = nlohmann::json::parse(json, nullptr, false);
    if (root.is_discarded() || !root.contains("contexts")) {
        LOG_ERROR("Input") << "Раскладка не прочитана: файл не разобрался как JSON";
        return false;
    }

    for (const auto& jctx : root["contexts"]) {
        const std::string name = jctx.value("name", std::string(kDefaultContext));
        Context& ctx = CreateContext(name, jctx.value("priority", kDefaultPriority));
        ctx.SetEnabled(jctx.value("enabled", true));
        ctx.SetBlocks((uint8_t)jctx.value("blocks", 0));

        if (!jctx.contains("actions")) continue;
        for (const auto& ja : jctx["actions"]) {
            const std::string actionName = ja.value("name", std::string());
            if (actionName.empty()) continue;
            Action& action = ctx.Add(actionName, ParseType(ja.value("type", std::string("digital"))));

            ActionSettings& s = action.Settings();
            s.Trigger = ParseTrigger(ja.value("trigger", std::string("press")));
            s.HoldTime = ja.value("holdTime", s.HoldTime);
            s.TapTime = ja.value("tapTime", s.TapTime);
            s.DeadZone = ja.value("deadZone", s.DeadZone);
            s.Zone = (ja.value("zone", std::string("radial")) == "axial") ? DeadZoneMode::Axial
                                                                         : DeadZoneMode::Radial;
            s.Smoothing = ja.value("smoothing", s.Smoothing);
            s.Normalize = ja.value("normalize", s.Normalize);
            s.Sensitivity = ja.value("sensitivity", s.Sensitivity);
            s.PressThreshold = ja.value("pressThreshold", s.PressThreshold);
            s.ExactModifiers = ja.value("exactModifiers", s.ExactModifiers);

            // Привязки ЗАМЕЩАЮТСЯ, а не дописываются: файл настроек — это
            // «как игрок настроил», а не «что ещё добавить к умолчанию».
            // Дописывание оставляло бы переназначенную клавишу работать и на
            // старом месте.
            action.ClearBindings();
            if (!ja.contains("bindings")) continue;
            for (const auto& jb : ja["bindings"]) {
                const std::string source = jb.value("source", std::string());
                std::optional<Binding> parsed = Binding::Parse(source);
                if (!parsed) {
                    LOG_WARN("Input") << "Раскладка '" << actionName << "': источник '" << source
                                      << "' не распознан — привязка пропущена";
                    continue;
                }
                parsed->Axis = (jb.value("axis", std::string("x")) == "y") ? Component::Y : Component::X;
                parsed->Gamepad = (int8_t)jb.value("gamepad", -1);
                action.Bind(*parsed);
            }
        }
    }
    return true;
}

bool InputSystem::SaveMapping(const std::filesystem::path& file) const {
    std::ofstream out(file);
    if (!out) {
        LOG_ERROR("Input") << "Раскладка не сохранена: не открыть файл на запись";
        return false;
    }
    out << SaveMappingToString();
    return out.good();
}

bool InputSystem::LoadMapping(const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in) return false; // нет файла — не ошибка: игрок ещё ничего не менял
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return LoadMappingFromString(text);
}

} // namespace sage::input
