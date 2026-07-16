#pragma once
#include "sage/core/InputSystem.h"
#include "sage/core/InputBinding.h"
#include "sage/core/KeyNames.h"
#include "sage/core/Log.h"
#include <GLFW/glfw3.h>
#include <fstream>
#include <string>

// ---------------------------------------------------------------------
// GameActions — список всех именованных действий The Boat и их привязки
// по умолчанию в одном месте. Остальной код (PlayerActions, main.cpp)
// обращается к вводу только через эти константы, никогда через сырые
// GLFW_KEY_* — значит, чтобы полностью переназначить управление, достаточно
// поменять этот файл, либо (без пересборки) положить рядом с игрой файл
// настроек и вызвать LoadBindingsFromFile() поверх RegisterDefaultBindings()
// — см. её ниже.
// ---------------------------------------------------------------------
namespace GameActions {

// Движение
inline constexpr const char* MoveForward = "MoveForward";
inline constexpr const char* MoveBackward = "MoveBackward";
inline constexpr const char* MoveLeft = "MoveLeft";
inline constexpr const char* MoveRight = "MoveRight";
inline constexpr const char* Jump = "Jump";
inline constexpr const char* Run = "Run";
inline constexpr const char* FlyUp = "FlyUp";     // noclip: вверх
inline constexpr const char* FlyDown = "FlyDown"; // noclip: вниз

// Взаимодействие с миром
inline constexpr const char* BreakOrHook = "BreakOrHook";   // ЛКМ: сломать блок / подсечь рыбу
inline constexpr const char* UseItem = "UseItem";           // ПКМ: использовать предмет в руке
inline constexpr const char* PickUpTrash = "PickUpTrash";   // F

// Хотбар
inline constexpr const char* HotbarNext = "HotbarNext"; // колесо вниз
inline constexpr const char* HotbarPrev = "HotbarPrev"; // колесо вверх
inline constexpr const char* HotbarSlot1 = "HotbarSlot1";
inline constexpr const char* HotbarSlot2 = "HotbarSlot2";
inline constexpr const char* HotbarSlot3 = "HotbarSlot3";
inline constexpr const char* HotbarSlot4 = "HotbarSlot4";
inline constexpr const char* HotbarSlot5 = "HotbarSlot5";
inline constexpr const char* HotbarSlot6 = "HotbarSlot6";
inline constexpr const char* HotbarSlot7 = "HotbarSlot7";
inline constexpr const char* HotbarSlot8 = "HotbarSlot8";
inline constexpr const char* HotbarSlot9 = "HotbarSlot9";

// Меню/режимы
inline constexpr const char* ToggleCrafting = "ToggleCrafting"; // Tab
inline constexpr const char* CraftNavigateUp = "CraftNavigateUp";
inline constexpr const char* CraftNavigateDown = "CraftNavigateDown";
inline constexpr const char* CraftConfirm = "CraftConfirm";
inline constexpr const char* ToggleNoclip = "ToggleNoclip"; // V

// Отладка/система
inline constexpr const char* Screenshot = "Screenshot";     // F2
inline constexpr const char* ToggleDebugHud = "ToggleDebugHud"; // F3
inline constexpr const char* QuitGame = "QuitGame";         // Esc

// Регистрирует все действия игры с их привязками по умолчанию.
// Вызывается один раз при старте, до первого InputSystem::Update().
inline void RegisterDefaultBindings(InputSystem& input) {
    InputMap& actions = input.Actions();

    actions.Register(MoveForward).Bind(InputBinding::Key(GLFW_KEY_W));
    actions.Register(MoveBackward).Bind(InputBinding::Key(GLFW_KEY_S));
    actions.Register(MoveLeft).Bind(InputBinding::Key(GLFW_KEY_A));
    actions.Register(MoveRight).Bind(InputBinding::Key(GLFW_KEY_D));
    actions.Register(Jump).Bind(InputBinding::Key(GLFW_KEY_SPACE));
    actions.Register(Run).Bind(InputBinding::Key(GLFW_KEY_LEFT_SHIFT));
    actions.Register(FlyUp).Bind(InputBinding::Key(GLFW_KEY_SPACE));
    actions.Register(FlyDown).Bind(InputBinding::Key(GLFW_KEY_LEFT_CONTROL));

    actions.Register(BreakOrHook).Bind(InputBinding::Mouse(GLFW_MOUSE_BUTTON_LEFT));
    actions.Register(UseItem).Bind(InputBinding::Mouse(GLFW_MOUSE_BUTTON_RIGHT));
    actions.Register(PickUpTrash).Bind(InputBinding::Key(GLFW_KEY_F));

    actions.Register(HotbarNext).Bind(InputBinding::WheelDown());
    actions.Register(HotbarPrev).Bind(InputBinding::WheelUp());
    actions.Register(HotbarSlot1).Bind(InputBinding::Key(GLFW_KEY_1));
    actions.Register(HotbarSlot2).Bind(InputBinding::Key(GLFW_KEY_2));
    actions.Register(HotbarSlot3).Bind(InputBinding::Key(GLFW_KEY_3));
    actions.Register(HotbarSlot4).Bind(InputBinding::Key(GLFW_KEY_4));
    actions.Register(HotbarSlot5).Bind(InputBinding::Key(GLFW_KEY_5));
    actions.Register(HotbarSlot6).Bind(InputBinding::Key(GLFW_KEY_6));
    actions.Register(HotbarSlot7).Bind(InputBinding::Key(GLFW_KEY_7));
    actions.Register(HotbarSlot8).Bind(InputBinding::Key(GLFW_KEY_8));
    actions.Register(HotbarSlot9).Bind(InputBinding::Key(GLFW_KEY_9));

    actions.Register(ToggleCrafting).Bind(InputBinding::Key(GLFW_KEY_TAB));
    actions.Register(CraftNavigateUp).Bind(InputBinding::Key(GLFW_KEY_UP));
    actions.Register(CraftNavigateDown).Bind(InputBinding::Key(GLFW_KEY_DOWN));
    actions.Register(CraftConfirm).Bind(InputBinding::Key(GLFW_KEY_ENTER));
    actions.Register(ToggleNoclip).Bind(InputBinding::Key(GLFW_KEY_V));

    actions.Register(Screenshot).Bind(InputBinding::Key(GLFW_KEY_F2));
    actions.Register(ToggleDebugHud).Bind(InputBinding::Key(GLFW_KEY_F3));
    actions.Register(QuitGame).Bind(InputBinding::Key(GLFW_KEY_ESCAPE));
}

// Читает переопределения раскладки из простого текстового файла настроек и
// накатывает их поверх RegisterDefaultBindings() через InputAction::Rebind().
// Формат — построчно "ActionName=KeyName" (см. имена в KeyNames.h: буквы,
// цифры, SPACE/TAB/ENTER/ESCAPE/F1-F12/стрелки/MOUSE_LEFT/MOUSE_RIGHT/
// MOUSE_MIDDLE), "#" начинает комментарий, пустые строки пропускаются.
// Пример строки: "Jump=E". Файл опционален — если его нет, молча остаёмся
// на дефолтной раскладке; нераспознанные строки логируются и пропускаются.
inline void LoadBindingsFromFile(InputMap& actions, const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return;

    auto trim = [](std::string s) {
        size_t start = s.find_first_not_of(" \t\r");
        if (start == std::string::npos) return std::string();
        size_t end = s.find_last_not_of(" \t\r");
        return s.substr(start, end - start + 1);
    };

    std::string line;
    while (std::getline(file, line)) {
        size_t hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string name = trim(line.substr(0, eq));
        std::string keyName = trim(line.substr(eq + 1));
        if (name.empty() || keyName.empty()) continue;

        if (!actions.Has(name)) {
            LOG_WARN("Input") << "keybindings: неизвестное действие '" << name << "' в " << path;
            continue;
        }
        auto binding = KeyNames::Parse(keyName);
        if (!binding) {
            LOG_WARN("Input") << "keybindings: неизвестная клавиша '" << keyName << "' для '" << name << "' в " << path;
            continue;
        }
        actions.Get(name).Rebind(*binding);
    }
}

// Хотбар-слоты по порядку — удобно для цикла `for (i, name) : HotbarSlotNames()`
inline const char* const* HotbarSlotNames() {
    static const char* names[9] = {
        HotbarSlot1, HotbarSlot2, HotbarSlot3, HotbarSlot4, HotbarSlot5,
        HotbarSlot6, HotbarSlot7, HotbarSlot8, HotbarSlot9,
    };
    return names;
}

} // namespace GameActions
