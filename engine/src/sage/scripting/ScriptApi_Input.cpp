#include "ScriptEngine.h"

#include "sage/core/Log.h"
#include "sage/core/KeyNames.h"

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
    // --- Ввод: именованные действия движка (см. core/InputMap.h), доступно
    // после BindInput. Скрипты читают тот же ввод, что и C++-код игры —
    // никакого параллельного дублирования раскладки клавиш. ---
    Bind("input", "IsDown", "IsActionDown", [this](const std::string& name) -> bool {
        return m_input && m_input->Has(name) && m_input->IsDown(name);
    });
    Bind("input", "WasPressed", "WasActionPressed", [this](const std::string& name) -> bool {
        return m_input && m_input->Has(name) && m_input->WasPressed(name);
    });
    Bind("input", "WasReleased", "WasActionReleased", [this](const std::string& name) -> bool {
        return m_input && m_input->Has(name) && m_input->WasReleased(name);
    });
    Bind("input", "Has", "HasAction", [this](const std::string& name) -> bool {
        return m_input && m_input->Has(name);
    });

    // --- Раскладка ИЗ ИГРЫ: BindAction("Jump", "SPACE") / BindAction("Move
    // Forward", {"W", "UP"}). Раньше действия мог объявить только C++-код
    // хоста, поэтому игра целиком на скриптах не могла завести НИ ОДНОЙ своей
    // клавиши — весь ввод приходилось согласовывать через правку движка.
    // Имена клавиш — те же, что в файлах настроек (см. core/KeyNames.h):
    // "W", "SPACE", "LEFT_SHIFT", "MOUSE_LEFT", "MOUSE_RIGHT", ... Возвращает
    // число реально распознанных привязок (0 — имя клавиши не понято). ---
    auto bindOne = [this](const std::string& action, const std::string& key) -> bool {
        std::optional<InputBinding> binding = KeyNames::Parse(key);
        if (!binding) {
            LOG_WARN("Lua") << "BindAction('" << action << "'): неизвестная клавиша '" << key << "'";
            return false;
        }
        m_input->Register(action).Bind(*binding);
        return true;
    };
    Bind("input", "Bind", "BindAction", [this, bindOne](const std::string& action, sol::object keys) -> int {
        if (!m_input)
            throw std::runtime_error("BindAction: ввод не привязан (ScriptEngine::BindInput не вызван)");
        int bound = 0;
        if (keys.is<std::string>()) {
            if (bindOne(action, keys.as<std::string>())) ++bound;
        } else if (keys.is<sol::table>()) {
            sol::table list = keys.as<sol::table>();
            for (size_t i = 1; i <= list.size(); ++i) {
                sol::optional<std::string> key = list[i];
                if (key && bindOne(action, *key)) ++bound;
            }
        } else {
            throw std::runtime_error("BindAction: вторым аргументом ожидается имя клавиши "
                                     "или таблица имён");
        }
        return bound;
    });

    // --- «Сырой» ввод: обзор от первого лица. Доступно после BindRawInput. ---
    Bind("input", "MouseDelta", "GetMouseDelta", [this]() -> glm::vec2 {
        if (!m_rawInput)
            throw std::runtime_error("GetMouseDelta: сырой ввод не привязан (BindRawInput не вызван)");
        return m_rawInput->MouseDelta();
    });
    Bind("input", "MousePosition", "GetMousePosition", [this]() -> glm::vec2 {
        if (!m_rawInput)
            throw std::runtime_error("GetMousePosition: сырой ввод не привязан (BindRawInput не вызван)");
        return m_rawInput->MousePosition();
    });
    Bind("input", "ScrollDelta", "GetScrollDelta", [this]() -> int {
        return m_rawInput ? m_rawInput->ScrollDelta() : 0;
    });
    Bind("input", "SetMouseCaptured", "SetMouseCaptured", [this](bool captured) {
        if (!m_rawInput)
            throw std::runtime_error("SetMouseCaptured: сырой ввод не привязан (BindRawInput не вызван)");
        m_rawInput->SetMouseCaptured(captured);
    });
    Bind("input", "IsMouseCaptured", "IsMouseCaptured", [this]() -> bool {
        return m_rawInput && m_rawInput->MouseCaptured();
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

