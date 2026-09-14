#include "ScriptEngine.h"

#include "sage/core/Log.h"
#include "sage/core/Paths.h"
#include "sage/ui/UIInteraction.h" // AppendUtf8 — тот же перевод codepoint -> UTF-8, что у полей ввода

// ---------------------------------------------------------------------------
// Ввод и камера: sage.input.*, sage.camera.*
//
// Часть Lua-API движка. Раньше ВСЕ привязки жили в одном ScriptEngine.cpp на
// 1800 строк: 126 функций, восемнадцать областей, и чтобы дописать одну
// строчку про анимацию, приходилось листать интерфейс, физику и таймеры.
// Определения разъехались по файлам ScriptApi_*.cpp — по файлу на область;
// объявления методов остались в ScriptEngine.h, поэтому порядок регистрации
// по-прежнему записан в одном месте (RegisterEngineApi) и не зависит от того,
// в каком файле лежит тело.
// ---------------------------------------------------------------------------

namespace {

// Именованные действия (BindAction/IsActionDown) хватает игровой логике, но не
// экрану настроек: спросить «что нажали?» без уже объявленного действия было
// нечем, а без этого в Lua нельзя написать ни ловлю клавиши по нажатию
// («назначьте кнопку»), ни быстрый прототип без раскладки заранее. Источник —
// та же строка, что везде ("W", "MOUSE_LEFT", "PAD_A"), разобранная тем же
// Binding::Parse, что понимает файл раскладки.
enum class ButtonQuery { Down, Pressed, Released };

bool QueryBindingButton(sage::input::InputSystem& input, const sage::input::Binding& b,
                        ButtonQuery q) {
    using namespace sage::input;
    const Devices& d = input.State();
    auto pick = [&](bool down, bool pressed, bool released) {
        switch (q) {
            case ButtonQuery::Down: return down;
            case ButtonQuery::Pressed: return pressed;
            case ButtonQuery::Released: return released;
        }
        return false;
    };
    switch (b.Kind) {
        case SourceKind::Key: {
            const Keyboard& k = d.Keys();
            return pick(k.Down(b.AsKey()), k.Pressed(b.AsKey()), k.Released(b.AsKey()));
        }
        case SourceKind::MouseButton: {
            const Mouse& m = d.MouseState();
            return pick(m.Down(b.AsMouseButton()), m.Pressed(b.AsMouseButton()), m.Released(b.AsMouseButton()));
        }
        case SourceKind::GamepadButton: {
            const int idx = b.Gamepad >= 0 ? b.Gamepad : d.FirstConnected();
            if (idx < 0) return false; // геймпад не подключён — честное «нет»
            const Gamepad& g = d.Pad(idx);
            return pick(g.Down(b.AsPadButton()), g.Pressed(b.AsPadButton()), g.Released(b.AsPadButton()));
        }
        default:
            return false; // колесо и оси — не кнопки, у них нет «нажато»; см. SourceValue
    }
}

// Аналоговое значение источника — работает для ЛЮБОГО вида, в отличие от
// QueryBindingButton: кнопка честно отвечает 1.0/0.0 (удобно, когда скрипт не
// знает заранее, к чему привязана настраиваемая клавиша).
float QueryBindingValue(sage::input::InputSystem& input, const sage::input::Binding& b) {
    using namespace sage::input;
    const Devices& d = input.State();
    switch (b.Kind) {
        case SourceKind::MouseWheel:
            return input.Wheel();
        case SourceKind::MouseAxis: {
            const glm::vec2 delta = d.MouseState().Delta();
            return b.Code == 0 ? delta.x : delta.y;
        }
        case SourceKind::GamepadAxis: {
            const int idx = b.Gamepad >= 0 ? b.Gamepad : d.FirstConnected();
            return idx < 0 ? 0.0f : d.Pad(idx).Axis(b.AsPadAxis());
        }
        default:
            return QueryBindingButton(input, b, ButtonQuery::Down) ? 1.0f : 0.0f;
    }
}

} // namespace

void ScriptEngine::RegisterInputApi() {
    using sage::input::ActionType;
    using sage::input::TriggerMode;

    // --- Ввод: именованные действия движка (см. sage/input/InputSystem.h),
    // доступно после BindInput. Скрипты читают тот же ввод, что и C++-код игры
    // — никакого параллельного дублирования раскладки клавиш. Неизвестное имя
    // отвечает «нет», а не роняет игру: раскладку объявляют сами скрипты, и
    // опечатка в имени действия не должна прерывать бой. ---
    Bind("input", "IsDown", "IsActionDown", [this](const std::string& name) -> bool {
        return m_input && m_input->IsDown(name);
    });
    Bind("input", "WasPressed", "WasActionPressed", [this](const std::string& name) -> bool {
        return m_input && m_input->WasPressed(name);
    });
    Bind("input", "WasReleased", "WasActionReleased", [this](const std::string& name) -> bool {
        return m_input && m_input->WasReleased(name);
    });
    // Сработало по выбранному режиму (нажатие/отпускание/удержание/короткое
    // нажатие). Для обычного действия совпадает с WasActionPressed.
    Bind("input", "Triggered", "WasActionTriggered", [this](const std::string& name) -> bool {
        return m_input && m_input->Triggered(name);
    });
    Bind("input", "Has", "HasAction", [this](const std::string& name) -> bool {
        return m_input && m_input->Has(name);
    });

    // --- Аналоговые действия: ось (−1..1) и вектор. Ради них ввод и разделён
    // на виды: «Движение» одинаково приходит и с WASD, и с левого стика, и
    // игровой код не знает, чем играют. ---
    Bind("input", "Axis", "GetAxis", [this](const std::string& name) -> float {
        return m_input ? m_input->Value(name) : 0.0f;
    });
    Bind("input", "Vector", "GetVector", [this](const std::string& name) -> glm::vec2 {
        return m_input ? m_input->Vector(name) : glm::vec2(0.0f);
    });

    // --- Раскладка ИЗ ИГРЫ: BindAction("Jump", "SPACE") / BindAction("Move
    // Forward", {"W", "UP"}). Раньше действия мог объявить только C++-код
    // хоста, поэтому игра целиком на скриптах не могла завести НИ ОДНОЙ своей
    // клавиши — весь ввод приходилось согласовывать через правку движка.
    // Имена источников — те же, что в файлах настроек (см. sage/input/Keys.h):
    // "W", "SPACE", "LEFT_SHIFT", "MOUSE_LEFT", "PAD_A", "CTRL+S", "-S", ...
    // Возвращает число реально распознанных привязок (0 — имя не понято). ---
    auto bindMany = [this](sage::input::Action& action, sol::object keys) -> int {
        int bound = 0;
        if (keys.is<std::string>()) {
            const std::string key = keys.as<std::string>();
            if (action.Bind(key)) ++bound;
            else LOG_WARN("Lua") << "BindAction('" << action.Name() << "'): неизвестный источник '"
                                 << key << "'";
        } else if (keys.is<sol::table>()) {
            sol::table list = keys.as<sol::table>();
            for (size_t i = 1; i <= list.size(); ++i) {
                sol::optional<std::string> key = list[i];
                if (!key) continue;
                if (action.Bind(*key)) ++bound;
                else LOG_WARN("Lua") << "BindAction('" << action.Name()
                                     << "'): неизвестный источник '" << *key << "'";
            }
        } else {
            throw std::runtime_error("BindAction: вторым аргументом ожидается имя источника "
                                     "или таблица имён");
        }
        return bound;
    };

    Bind("input", "Bind", "BindAction", [this, bindMany](const std::string& action, sol::object keys) -> int {
        if (!m_input)
            throw std::runtime_error("BindAction: ввод не привязан (ScriptEngine::BindInput не вызван)");
        return bindMany(m_input->Register(action, ActionType::Digital), keys);
    });

    // Ось из двух клавиш: BindAxis("Forward", "W", "S"). Второе имя получает
    // вклад −1 — иначе каждая игра писала бы это вычитание заново.
    Bind("input", "BindAxis", "BindAxis",
         [this](const std::string& action, const std::string& positive, const std::string& negative) -> int {
        if (!m_input)
            throw std::runtime_error("BindAxis: ввод не привязан (ScriptEngine::BindInput не вызван)");
        sage::input::Action& a = m_input->Register(action, ActionType::Axis);
        int bound = 0;
        if (a.Bind(positive)) ++bound;
        if (a.Bind("-" + negative)) ++bound;
        if (bound < 2)
            LOG_WARN("Lua") << "BindAxis('" << action << "'): распознано " << bound << " из 2 источников";
        return bound;
    });

    // Вектор из четырёх направлений: BindVector("Move", "W", "S", "A", "D").
    // Даёт Vector2 — ровно то же, что отдаёт левый стик.
    Bind("input", "BindVector", "BindVector",
         [this](const std::string& action, const std::string& up, const std::string& down,
                const std::string& left, const std::string& right) {
        if (!m_input)
            throw std::runtime_error("BindVector: ввод не привязан (ScriptEngine::BindInput не вызван)");
        m_input->Register(action, ActionType::Vector).BindVector(up, down, left, right);
    });

    // Переназначение управления из игры (§20 ТЗ): экран настроек живёт в игре,
    // а не в движке, и без этой функции написать его на скриптах нельзя.
    Bind("input", "Rebind", "RebindAction",
         [this](const std::string& action, const std::string& source) -> bool {
        return m_input && m_input->Rebind(action, source);
    });
    Bind("input", "SaveMapping", "SaveInputMapping", [this](const std::string& file) -> bool {
        return m_input && m_input->SaveMapping(sage::PathFromUtf8(file));
    });
    Bind("input", "LoadMapping", "LoadInputMapping", [this](const std::string& file) -> bool {
        return m_input && m_input->LoadMapping(sage::PathFromUtf8(file));
    });

    // Режим срабатывания действия: "press" (по умолчанию), "release", "hold",
    // "tap". Так одна клавиша делится на «нажать» и «удержать» (§23 ТЗ).
    Bind("input", "SetTrigger", "SetActionTrigger",
         [this](const std::string& action, const std::string& mode) -> bool {
        if (!m_input) return false;
        sage::input::Action* a = m_input->Find(action);
        if (!a) return false;
        if (mode == "release") a->Settings().Trigger = TriggerMode::Release;
        else if (mode == "hold") a->Settings().Trigger = TriggerMode::Hold;
        else if (mode == "tap") a->Settings().Trigger = TriggerMode::Tap;
        else a->Settings().Trigger = TriggerMode::Press;
        return true;
    });

    // --- Контексты (§8 ТЗ): одна клавиша значит разное в игре, в инвентаре и
    // в диалоге. Без них это условие пришлось бы повторять у каждого действия
    // и не забыть ни одного — а забудут обязательно. ---
    Bind("input", "CreateContext", "CreateInputContext",
         [this](const std::string& name, int priority) {
        if (!m_input)
            throw std::runtime_error("CreateInputContext: ввод не привязан");
        m_input->CreateContext(name, priority);
    });
    Bind("input", "BindIn", "BindActionIn",
         [this, bindMany](const std::string& context, const std::string& action, sol::object keys) -> int {
        if (!m_input)
            throw std::runtime_error("BindActionIn: ввод не привязан");
        return bindMany(m_input->Register(context, action, ActionType::Digital), keys);
    });
    Bind("input", "SetContextEnabled", "SetInputContextEnabled",
         [this](const std::string& name, bool enabled) {
        if (m_input) m_input->SetContextEnabled(name, enabled);
    });
    Bind("input", "IsContextEnabled", "IsInputContextEnabled", [this](const std::string& name) -> bool {
        return m_input && m_input->ContextEnabled(name);
    });

    // --- «Сырой» ввод: обзор от первого лица. ---
    Bind("input", "MouseDelta", "GetMouseDelta", [this]() -> glm::vec2 {
        if (!m_input)
            throw std::runtime_error("GetMouseDelta: ввод не привязан (BindInput не вызван)");
        return m_input->MouseDelta();
    });
    Bind("input", "MousePosition", "GetMousePosition", [this]() -> glm::vec2 {
        if (!m_input)
            throw std::runtime_error("GetMousePosition: ввод не привязан (BindInput не вызван)");
        return m_input->MousePosition();
    });
    // Знак: «вверх/от себя» — положительный. Скрипты получают то же число, что
    // и код движка, а не перевёрнутое.
    Bind("input", "ScrollDelta", "GetScrollDelta", [this]() -> float {
        return m_input ? m_input->Wheel() : 0.0f;
    });
    Bind("input", "SetMouseCaptured", "SetMouseCaptured", [this](bool captured) {
        if (!m_input)
            throw std::runtime_error("SetMouseCaptured: ввод не привязан (BindInput не вызван)");
        m_input->SetCursorCaptured(captured);
    });
    Bind("input", "IsMouseCaptured", "IsMouseCaptured", [this]() -> bool {
        return m_input && m_input->CursorCaptured();
    });

    // --- Источник напрямую, БЕЗ объявленного действия. Игровой логике хватает
    // именованных действий выше, но не экрану настроек: спросить «нажат ли
    // ИМЕННО этот физический источник» без уже заведённого Action было нечем —
    // ни ловлю клавиши для «назначьте новую кнопку», ни быстрый прототип без
    // раскладки заранее на них было не написать. Строка источника — та же, что
    // в BindAction/файле раскладки ("W", "MOUSE_LEFT", "PAD_A", "CTRL+S").
    // Нераспознанное имя — честные false/0, а не ошибка: раскладку строит сам
    // скрипт, и опечатка в строке не должна его ронять. ---
    Bind("input", "SourceDown", nullptr, [this](const std::string& source) -> bool {
        if (!m_input) throw std::runtime_error("SourceDown: ввод не привязан (BindInput не вызван)");
        auto b = sage::input::Binding::Parse(source);
        return b && QueryBindingButton(*m_input, *b, ButtonQuery::Down);
    });
    Bind("input", "SourcePressed", nullptr, [this](const std::string& source) -> bool {
        if (!m_input) throw std::runtime_error("SourcePressed: ввод не привязан (BindInput не вызван)");
        auto b = sage::input::Binding::Parse(source);
        return b && QueryBindingButton(*m_input, *b, ButtonQuery::Pressed);
    });
    Bind("input", "SourceReleased", nullptr, [this](const std::string& source) -> bool {
        if (!m_input) throw std::runtime_error("SourceReleased: ввод не привязан (BindInput не вызван)");
        auto b = sage::input::Binding::Parse(source);
        return b && QueryBindingButton(*m_input, *b, ButtonQuery::Released);
    });
    // Аналоговое значение — колесо, ось стика/курка/мыши, а для кнопки честные
    // 1.0/0.0 (удобно, когда скрипт не знает заранее, что за источник ему
    // назначили: не нужно спрашивать вид отдельно).
    Bind("input", "SourceValue", nullptr, [this](const std::string& source) -> float {
        if (!m_input) throw std::runtime_error("SourceValue: ввод не привязан (BindInput не вызван)");
        auto b = sage::input::Binding::Parse(source);
        return b ? QueryBindingValue(*m_input, *b) : 0.0f;
    });

    // Что нажали В ЭТОМ КАДРЕ — целиком строкой источника, готовой для
    // BindAction/Rebind. Без неё экран «нажмите новую клавишу» на Lua не
    // написать: SourceDown требует уже знать, ЧТО спрашивать, а тут вопрос
    // ровно обратный. Сами модификаторы (Shift/Ctrl/Alt/Super) источником не
    // считаются — нажатие голого Shift почти всегда значит, что сочетание ещё
    // не дожали, — тот же приём, что у собственной ловли редактора
    // (InputPanel::BindingFromEvent). Отмену по Escape (если она нужна) решает
    // сам вызывающий скрипт: это политика экрана, а не источника.
    Bind("input", "AnyPressedSource", nullptr, [this]() -> sol::object {
        if (!m_input) throw std::runtime_error("AnyPressedSource: ввод не привязан (BindInput не вызван)");
        using namespace sage::input;
        for (const InputEvent& e : m_input->FrameEvents()) {
            if (e.Consumed) continue;
            switch (e.Type) {
                case InputEventType::KeyPressed:
                    switch (e.Keyboard) {
                        case Key::LeftShift: case Key::RightShift:
                        case Key::LeftControl: case Key::RightControl:
                        case Key::LeftAlt: case Key::RightAlt:
                        case Key::LeftSuper: case Key::RightSuper:
                            continue;
                        default: break;
                    }
                    return sol::make_object(m_lua, Binding::OfKey(e.Keyboard, e.Modifiers).ToString());
                case InputEventType::MouseButtonPressed:
                    return sol::make_object(m_lua, Binding::OfMouse(e.Button, e.Modifiers).ToString());
                case InputEventType::MouseWheel:
                    return sol::make_object(m_lua,
                        (e.Wheel > 0.0f ? Binding::WheelUp() : Binding::WheelDown()).ToString());
                case InputEventType::GamepadButtonPressed:
                    return sol::make_object(m_lua,
                        Binding::OfPadButton(e.PadButton, (int8_t)e.Gamepad).ToString());
                default: break;
            }
        }
        return sol::nil;
    });

    // Кто уже занял этот источник — экран настроек обязан спросить ДО
    // назначения: молча отобрать клавишу у другого действия значит сломать его
    // управление, не сказав об этом.
    Bind("input", "FindConflict", nullptr, [this](const std::string& source) -> sol::object {
        if (!m_input) throw std::runtime_error("FindConflict: ввод не привязан (BindInput не вызван)");
        auto b = sage::input::Binding::Parse(source);
        if (!b) return sol::nil;
        sage::input::Action* a = m_input->FindByBinding(*b);
        return a ? sol::make_object(m_lua, a->Name()) : sol::nil;
    });
    // Добавить привязку, НЕ снимая прежние — в отличие от Rebind (полная
    // замена). «Прыжок» на пробеле и вдобавок на кнопке A геймпада — это
    // AddBinding, а не Rebind, который стёр бы пробел.
    Bind("input", "AddBinding", nullptr, [this](const std::string& action, const std::string& source) -> bool {
        if (!m_input) throw std::runtime_error("AddBinding: ввод не привязан (BindInput не вызван)");
        return m_input->AddBinding(action, source);
    });

    // Имена всех заведённых контекстов — экрану настроек, который перечисляет
    // «Игра/Инвентарь/Диалог», а не только тот, с которым сейчас работают.
    Bind("input", "ContextNames", nullptr, [this]() -> sol::table {
        sol::table out = m_lua.create_table();
        if (!m_input) return out;
        int i = 1;
        for (const std::string& name : m_input->ContextNames()) out[i++] = name;
        return out;
    });

    // Отпустить весь ввод немедленно — как при потере фокуса окна. Открыли
    // меню паузы с зажатым «вперёд» — без этого игрок вернётся в игру, которая
    // всё это время шла вперёд сама (см. InputSystem::ReleaseAll).
    Bind("input", "ReleaseAll", nullptr, [this]() {
        if (m_input) m_input->ReleaseAll();
    });

    // Набранный за кадр ТЕКСТ — готовые символы Unicode (раскладка, Shift,
    // мёртвые клавиши и композиция уже учтены системой), а не коды клавиш.
    // Нужен полю ввода, написанному на Lua поверх своего интерфейса: имя
    // игрока, чат, консоль команд — без этого текст с кириллицей или составных
    // символов на Lua не набрать вовсе (SourceDown/AnyPressedSource дают только
    // физическую клавишу, не итоговый символ раскладки).
    Bind("input", "TypedText", nullptr, [this]() -> std::string {
        if (!m_input) throw std::runtime_error("TypedText: ввод не привязан (BindInput не вызван)");
        std::string out;
        for (unsigned int cp : m_input->TypedText()) sage::ui::AppendUtf8(out, cp);
        return out;
    });

    // Геймпад: подключён ли (без индекса — «хоть один», ровно то поведение,
    // которое хочет одиночная игра), и его имя — для подсказки «нажмите A» на
    // экране, где показывать её стоит, только если джойстик и правда воткнут.
    Bind("input", "GamepadConnected", nullptr, [this](sol::optional<int> index) -> bool {
        if (!m_input) return false;
        const int idx = index.value_or(-1);
        if (idx < 0) return m_input->State().AnyGamepadConnected();
        return m_input->State().Pad(idx).Connected();
    });
    Bind("input", "GamepadName", nullptr, [this](sol::optional<int> index) -> std::string {
        if (!m_input) return std::string();
        const int idx = index.value_or(m_input->State().FirstConnected());
        if (idx < 0) return std::string();
        return m_input->State().Pad(idx).Name();
    });

    // Тонкая настройка действия — то же самое, что уже умеет ActionSettings в
    // C++ (мёртвая зона, сглаживание, чувствительность, нормировка диагонали,
    // точные модификаторы, тайминги удержания/короткого нажатия), одной
    // таблицей: поля, которых нет в таблице, остаются как были. Экран
    // «Чувствительность стика», «Мёртвая зона» без неё писался бы только
    // правкой движка.
    Bind("input", "Configure", nullptr, [this](const std::string& action, sol::table settings) -> bool {
        if (!m_input) throw std::runtime_error("Configure: ввод не привязан (BindInput не вызван)");
        sage::input::Action* a = m_input->Find(action);
        if (!a) return false;
        sage::input::ActionSettings& s = a->Settings();
        if (sol::optional<float> v = settings["deadZone"]) s.DeadZone = *v;
        if (sol::optional<std::string> v = settings["deadZoneMode"])
            s.Zone = (*v == "axial") ? sage::input::DeadZoneMode::Axial : sage::input::DeadZoneMode::Radial;
        if (sol::optional<float> v = settings["smoothing"]) s.Smoothing = *v;
        if (sol::optional<float> v = settings["sensitivity"]) s.Sensitivity = *v;
        if (sol::optional<bool> v = settings["normalize"]) s.Normalize = *v;
        if (sol::optional<bool> v = settings["exactModifiers"]) s.ExactModifiers = *v;
        if (sol::optional<float> v = settings["holdTime"]) s.HoldTime = *v;
        if (sol::optional<float> v = settings["tapTime"]) s.TapTime = *v;
        if (sol::optional<float> v = settings["pressThreshold"]) s.PressThreshold = *v;
        return true;
    });
}

void ScriptEngine::RegisterCameraApi() {
    // --- Камера: доступно после BindCamera. Position — обычное поле, но
    // Yaw/Pitch выставлены через property-функции, которые сразу пересчитывают
    // Front/Right/Up вызовом ProcessMouse(0,0) — тем же приёмом, что уже
    // использует ApplyDebugEnvOverrides() в main.cpp после ручной правки угла.
    // Без этого камера "смотрела" бы в старом направлении до следующего
    // движения мыши игроком. ---
    m_lua.new_usertype<Camera>("Camera",
        "Position", &Camera::Position,
        "Front", sol::readonly(&Camera::Front),
        "Right", sol::readonly(&Camera::Right),
        "Up", sol::readonly(&Camera::Up),
        "Fov", &Camera::Fov,
        "MovementSpeed", &Camera::MovementSpeed,
        "NearClip", &Camera::NearClip,
        "FarClip", &Camera::FarClip,
        "Yaw", sol::property(
            [](const Camera& c) { return c.Yaw; },
            [](Camera& c, float yaw) { c.Yaw = yaw; c.ProcessMouse(0.0f, 0.0f); }),
        "Pitch", sol::property(
            [](const Camera& c) { return c.Pitch; },
            [](Camera& c, float pitch) { c.Pitch = pitch; c.ProcessMouse(0.0f, 0.0f); })
    );

    Bind("camera", "Get", "GetCamera", [this]() -> Camera& {
        if (!m_camera) throw std::runtime_error("GetCamera: камера не привязана (ScriptEngine::BindCamera не вызван)");
        return *m_camera;
    });

    // --- Экран <-> мир. Между щелчком мыши и лучом в мир не было ничего, кроме
    // ручной геометрии в самом скрипте: без этого «щёлкнуть по объекту» умел
    // только прицел от первого лица (луч из camera.Position/camera.Front, то
    // есть всегда из ЦЕНТРА экрана) — курсор мыши в этот луч превратить было
    // нечем. Расчёт — БЕЗ полных матриц вида/проекции: у камеры уже есть базис
    // (Front/Right/Up) и угол обзора, а привязывать glm::mat4 к Lua ради одной
    // этой пары функций незачем. Аспект и координаты берутся из sage.ui.Cursor/
    // ScreenSize (Scene::UiFrame) — ТОЙ ЖЕ системы координат, что у letterbox-
    // кадра игры, а не окна: иначе луч уезжал бы мимо на нестандартном размере
    // окна или с чёрными полосами по краям.
    //
    // Таблица {origin, dir}, а не два возврата: тот же приём, что у
    // physics.Raycast ({object, point, normal, distance}) — подсказка API
    // читает возврат по сигнатуре лямбды и честно напишет «table», а два
    // отдельных Vec3 она показала бы как один (соврав про второй).
    Bind("camera", "ScreenToRay", nullptr,
         [this](float screenX, float screenY) -> sol::table {
        if (!m_camera) throw std::runtime_error("ScreenToRay: камера не привязана (BindCamera не вызван)");
        const glm::vec2 size = m_scene ? m_scene->UiFrame.Size : glm::vec2(0.0f);
        glm::vec3 dir = m_camera->Front;
        if (size.x > 0.0f && size.y > 0.0f) {
            const float aspect = size.x / size.y;
            const float tanHalfFov = std::tan(glm::radians(m_camera->Fov) * 0.5f);
            const float ndcX = (2.0f * screenX / size.x - 1.0f) * aspect * tanHalfFov;
            const float ndcY = (1.0f - 2.0f * screenY / size.y) * tanHalfFov;
            dir = glm::normalize(m_camera->Front + m_camera->Right * ndcX + m_camera->Up * ndcY);
        }
        sol::table t = m_lua.create_table();
        t["origin"] = m_camera->Position;
        t["dir"] = dir;
        return t;
    });

    // Обратная операция: точка мира -> точка экрана, или nil — точка позади
    // камеры (или ближе NearClip), проецировать её честно нельзя. Нужна всему,
    // что рисует интерфейс НАД объектом мира: полоска здоровья, маркер цели,
    // всплывающее число урона.
    Bind("camera", "WorldToScreen", nullptr,
         [this](const glm::vec3& worldPos) -> sol::object {
        if (!m_camera) throw std::runtime_error("WorldToScreen: камера не привязана (BindCamera не вызван)");
        const glm::vec2 size = m_scene ? m_scene->UiFrame.Size : glm::vec2(0.0f);
        if (size.x <= 0.0f || size.y <= 0.0f) return sol::nil;
        const glm::vec3 toPoint = worldPos - m_camera->Position;
        const float depth = glm::dot(toPoint, m_camera->Front);
        if (depth <= m_camera->NearClip) return sol::nil;
        const float aspect = size.x / size.y;
        const float tanHalfFov = std::tan(glm::radians(m_camera->Fov) * 0.5f);
        const float ndcX = glm::dot(toPoint, m_camera->Right) / (depth * aspect * tanHalfFov);
        const float ndcY = glm::dot(toPoint, m_camera->Up) / (depth * tanHalfFov);
        return sol::make_object(m_lua, glm::vec2((ndcX + 1.0f) * 0.5f * size.x,
                                                 (1.0f - ndcY) * 0.5f * size.y));
    });
}

