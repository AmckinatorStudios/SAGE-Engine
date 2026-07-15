#pragma once
#include "InputMap.h"
#include "../render/Camera.h"
#include <GLFW/glfw3.h>

// ---------------------------------------------------------------------
// InputSystem — мост между "сырым" GLFW и именованными действиями
// InputMap. Раз в кадр Update() проходит по всем зарегистрированным
// действиям и для каждого решает "нажато ли хоть одно из его привязок".
//
// Также владеет мышью (накопленное смещение для камеры) и колесом —
// это физически не клавиши, поэтому не укладывается в общий Update()
// цикл, но результат всё равно попадает в InputMap как обычные действия,
// если под колесо/кнопки мыши что-то забинженно.
//
// Использование:
//   InputSystem input;
//   input.Attach(window.Handle());
//   InputMap& actions = input.Actions();
//   actions.Register("Jump").Bind(InputBinding::Key(GLFW_KEY_SPACE));
//   ...
//   // каждый кадр, в начале цикла:
//   input.Update(window.Handle());
//   input.ApplyMouseDelta(camera);
//   if (actions.WasPressed("Jump")) ...
// ---------------------------------------------------------------------
class InputSystem {
public:
    void Attach(GLFWwindow* window) {
        glfwSetWindowUserPointer(window, this);
        glfwSetCursorPosCallback(window, &InputSystem::MouseMoveCallback);
        glfwSetMouseButtonCallback(window, &InputSystem::MouseButtonCallback);
        glfwSetScrollCallback(window, &InputSystem::ScrollCallback);
    }

    InputMap& Actions() { return m_actions; }
    const InputMap& Actions() const { return m_actions; }

    // Вызывать РОВНО ОДИН РАЗ в начале каждого кадра, до чтения любых действий.
    // Прогоняет все зарегистрированные действия через их привязки и решает
    // текущее down-состояние; сбрасывает одноразовые mouse-клики и скролл,
    // накопленные колбэками с прошлого кадра.
    void Update(GLFWwindow* window) {
        for (auto& [name, action] : m_actions.All()) {
            action.EndFrame(); // гасим pressed/released прошлого кадра

            bool down = false;
            for (const InputBinding& binding : action.Bindings()) {
                switch (binding.Kind) {
                    case BindingKind::Keyboard:
                        down = down || (glfwGetKey(window, binding.Code) == GLFW_PRESS);
                        break;
                    case BindingKind::MouseButton:
                        down = down || (glfwGetMouseButton(window, binding.Code) == GLFW_PRESS);
                        break;
                    case BindingKind::ScrollUp:
                        if (m_scrollDelta < 0) action.PulseOnce(); // вверх = отрицательный yoffset по конвенции движка
                        break;
                    case BindingKind::ScrollDown:
                        if (m_scrollDelta > 0) action.PulseOnce();
                        break;
                }
            }
            // Скролл уже обработан через PulseOnce внутри цикла привязок —
            // Update(down) применяем только к keyboard/mouse-привязкам,
            // чтобы не затереть импульс скролла принудительным false.
            bool hasDiscreteBinding = false;
            for (const InputBinding& b : action.Bindings()) {
                if (b.Kind == BindingKind::Keyboard || b.Kind == BindingKind::MouseButton) hasDiscreteBinding = true;
            }
            if (hasDiscreteBinding) action.Update(down);
        }
        m_scrollDelta = 0; // одноразовое событие колеса — живёт один кадр
    }

    // Применяет накопленное смещение мыши к камере и сбрасывает его.
    // Отдельно от Update(), потому что не все сцены имеют камеру
    // (например, экран паузы без 3D-вида).
    void ApplyMouseDelta(Camera& camera) {
        camera.ProcessMouse(m_mouseDeltaX, m_mouseDeltaY);
        m_mouseDeltaX = 0.0f;
        m_mouseDeltaY = 0.0f;
    }

    float MouseDeltaX() const { return m_mouseDeltaX; }
    float MouseDeltaY() const { return m_mouseDeltaY; }

    // Сырой сдвиг колеса за кадр (для UI без привязанных действий — скролл
    // хотбара, скролл списков и т.п.), без ремаппинга
    int RawScrollDelta() const { return m_scrollDelta; }

private:
    static void MouseMoveCallback(GLFWwindow* window, double xpos, double ypos) {
        auto* self = static_cast<InputSystem*>(glfwGetWindowUserPointer(window));
        if (!self) return;

        if (self->m_firstMouse) {
            self->m_lastX = (float)xpos;
            self->m_lastY = (float)ypos;
            self->m_firstMouse = false;
        }
        self->m_mouseDeltaX += (float)xpos - self->m_lastX;
        self->m_mouseDeltaY += self->m_lastY - (float)ypos; // Y инвертирован (экран вниз, мир вверх)
        self->m_lastX = (float)xpos;
        self->m_lastY = (float)ypos;
    }

    static void MouseButtonCallback(GLFWwindow*, int, int, int) {
        // Кнопки мыши читаются poll-ом (glfwGetMouseButton) в Update(), не
        // колбэком — тем же способом, что и клавиатура. Колбэк оставлен
        // подключённым про запас (например, для будущего double-click),
        // сейчас он намеренно пуст.
    }

    static void ScrollCallback(GLFWwindow* window, double, double yoffset) {
        auto* self = static_cast<InputSystem*>(glfwGetWindowUserPointer(window));
        if (!self) return;
        // Конвенция движка: "вверх" (yoffset > 0) -> отрицательная дельта,
        // чтобы ScrollUp/ScrollDown читались интуитивно относительно
        // порядка в списках (вверх = к началу = меньший индекс)
        self->m_scrollDelta += (yoffset > 0) ? -1 : 1;
    }

    InputMap m_actions;

    bool m_firstMouse = true;
    float m_lastX = 0.0f, m_lastY = 0.0f;
    float m_mouseDeltaX = 0.0f, m_mouseDeltaY = 0.0f;
    int m_scrollDelta = 0;
};
