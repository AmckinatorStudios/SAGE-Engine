#pragma once
#include "InputBinding.h"
#include <vector>
#include <string>

// ---------------------------------------------------------------------
// InputAction — одно именованное игровое действие ("Jump", "Break Block",
// "Open Crafting") и все физические клавиши/кнопки, которые его вызывают.
//
// Действие хранит три стандартных состояния кнопки за кадр:
//   IsDown()     — удерживается прямо сейчас (для непрерывных действий: ходьба)
//   WasPressed() — стало нажатым именно в этом кадре (для разовых: прыжок, F2)
//   WasReleased()— было отпущено именно в этом кадре
//
// Действие само не читает GLFW — этим занимается InputSystem, вызывая
// Update(down) каждый кадр. Так InputAction остаётся простым и тестируемым:
// он не знает про окна и колбэки, только про свои привязки и текущее состояние.
// ---------------------------------------------------------------------
class InputAction {
public:
    explicit InputAction(std::string name) : m_name(std::move(name)) {}

    InputAction& Bind(InputBinding binding) {
        m_bindings.push_back(binding);
        return *this;
    }

    const std::vector<InputBinding>& Bindings() const { return m_bindings; }
    const std::string& Name() const { return m_name; }

    // Заменяет все привязки на одну новую (переназначение клавиши в настройках)
    void Rebind(InputBinding binding) {
        m_bindings.clear();
        m_bindings.push_back(binding);
    }

    void ClearBindings() { m_bindings.clear(); }

    bool IsDown() const { return m_down; }
    bool WasPressed() const { return m_pressed; }
    bool WasReleased() const { return m_released; }

    // Вызывается InputSystem раз в кадр с итоговым состоянием "зажато ли
    // сейчас хоть одно из привязанных сочетаний" (OR по всем привязкам)
    void Update(bool down) {
        m_pressed = down && !m_down;
        m_released = !down && m_down;
        m_down = down;
    }

    // Скролл — событие разового тика, а не удержания: "нажатие" длится
    // ровно один кадр сам по себе, IsDown() для колеса не имеет смысла
    // (в отличие от клавиши, колесо физически некуда "держать зажатым")
    void PulseOnce() {
        m_pressed = true;
    }

    // Вызывается InputSystem в конце кадра — гасит одно-кадровые pressed/released,
    // чтобы на следующем кадре они не "протухли" сами по себе
    void EndFrame() {
        m_pressed = false;
        m_released = false;
    }

private:
    std::string m_name;
    std::vector<InputBinding> m_bindings;

    bool m_down = false;
    bool m_pressed = false;
    bool m_released = false;
};
