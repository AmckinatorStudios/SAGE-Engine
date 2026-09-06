#include "ScriptEngine.h"

#include "sage/core/Log.h"
#include "sage/core/Paths.h"

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
}

