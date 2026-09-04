#pragma once
#include "InputAction.h"
#include <unordered_map>
#include <string>
#include <stdexcept>

// ---------------------------------------------------------------------
// InputMap — реестр всех именованных действий движка/игры. Игра один раз
// при старте объявляет, какие действия существуют и какие клавиши их
// вызывают по умолчанию; дальше весь код обращается к действиям по имени
// ("Jump", "Break Block"), а не по коду клавиши — это и даёт полноценный
// ремаппинг: поменять раскладку значит поменять привязки здесь, ни один
// вызывающий код трогать не нужно.
//
// Использование:
//   InputMap map;
//   map.Register("Jump").Bind(InputBinding::Key(sage::Key::Space));
//   map.Register("Break Block").Bind(InputBinding::Mouse(sage::MouseButton::Left));
//   ...
//   if (map.Get("Jump").WasPressed()) ...
//
// Действия хранятся в unordered_map<string, InputAction>, но для горячего
// пути (Get() каждый кадр в цикле игры) это приемлемо — обращений к вводу
// на порядок меньше, чем к рендеру, и никаких аллокаций Get() не делает.
// ---------------------------------------------------------------------
class InputMap {
public:
    // Регистрирует новое действие (или возвращает существующее с тем же
    // именем — удобно для "дозаписи" привязок в несколько мест инициализации)
    InputAction& Register(const std::string& name) {
        auto it = m_actions.find(name);
        if (it != m_actions.end()) return it->second;
        auto [inserted, _] = m_actions.emplace(name, InputAction(name));
        return inserted->second;
    }

    InputAction& Get(const std::string& name) {
        auto it = m_actions.find(name);
        if (it == m_actions.end()) {
            throw std::runtime_error("InputMap: неизвестное действие '" + name + "' — забыли Register()?");
        }
        return it->second;
    }

    const InputAction& Get(const std::string& name) const {
        auto it = m_actions.find(name);
        if (it == m_actions.end()) {
            throw std::runtime_error("InputMap: неизвестное действие '" + name + "' — забыли Register()?");
        }
        return it->second;
    }

    // Короткие проверки для самых частых обращений — избавляют вызывающий
    // код от Get(name).WasPressed() на каждый чих
    bool IsDown(const std::string& name) const { return Get(name).IsDown(); }
    bool WasPressed(const std::string& name) const { return Get(name).WasPressed(); }
    bool WasReleased(const std::string& name) const { return Get(name).WasReleased(); }

    bool Has(const std::string& name) const { return m_actions.find(name) != m_actions.end(); }

    std::unordered_map<std::string, InputAction>& All() { return m_actions; }

private:
    std::unordered_map<std::string, InputAction> m_actions;
};
