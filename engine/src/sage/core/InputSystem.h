#pragma once
#include "InputMap.h"
#include "RawInput.h"
#include "Window.h"
#include "sage/render/Camera.h"
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
//   input.Attach(window);
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
    // Подписка на события мыши идёт ЧЕРЕЗ Window (см. Window.h): GLFW даёт
    // только один user pointer на окно, и он принадлежит Window — прежняя
    // версия перезаписывала его собой, из-за чего resize-колбэк Window
    // кастовал InputSystem* к Window* (порча памяти при изменении размера
    // окна). InputSystem больше не трогает GLFW-колбэки напрямую.
    void Attach(Window& window) {
        window.SetCursorPosCallback([this](double xpos, double ypos) { OnMouseMove(xpos, ypos); });
        window.SetScrollCallback([this](double, double yoffset) {
            // Конвенция движка: "вверх" (yoffset > 0) -> отрицательная дельта,
            // чтобы ScrollUp/ScrollDown читались интуитивно относительно
            // порядка в списках (вверх = к началу = меньший индекс)
            m_scrollDelta += (yoffset > 0) ? -1 : 1;
        });
    }

    InputMap& Actions() { return m_actions; }
    const InputMap& Actions() const { return m_actions; }

    // Вызывать РОВНО ОДИН РАЗ в начале каждого кадра, до чтения любых действий.
    // Прогоняет все зарегистрированные действия через их привязки и решает
    // текущее down-состояние; сбрасывает одноразовые mouse-клики и скролл,
    // накопленные колбэками с прошлого кадра.
    void Update(GLFWwindow* window) {
        // Снимок «сырых» величин на кадр. Копия, а не сам накопитель, потому что
        // потребителей у мыши теперь несколько (камера игры И скрипты через
        // RawInputSource), а событие мыши приходит один раз: кто прочитал
        // первым, тот и обнулил бы её для остальных. Накопитель гасится ЗДЕСЬ,
        // ровно раз в кадр — читатели весь кадр видят одно и то же значение.
        m_frameMouseDeltaX = m_mouseDeltaX;
        m_frameMouseDeltaY = m_mouseDeltaY;
        m_frameScrollDelta = m_scrollDelta;
        m_mouseDeltaX = 0.0f;
        m_mouseDeltaY = 0.0f;

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

    // Кадр БЕЗ ввода: все действия гасятся так, будто ничего не нажато, а
    // сырые дельты обнуляются. Нужно хосту, который временами отбирает ввод у
    // игры — в редакторе фокус уходит из панели Game, и WASD должен уезжать в
    // переименование сущности, а не в игрока. Простое «не звать Update» не
    // годится: тогда действия навсегда застыли бы нажатыми.
    void UpdateIdle() {
        m_frameMouseDeltaX = m_frameMouseDeltaY = 0.0f;
        m_frameScrollDelta = 0;
        m_mouseDeltaX = m_mouseDeltaY = 0.0f;
        m_scrollDelta = 0;
        for (auto& [name, action] : m_actions.All()) {
            action.EndFrame();
            action.Update(false);
        }
    }

    // Применяет смещение мыши текущего кадра к камере. Отдельно от Update(),
    // потому что не все сцены имеют камеру (например, экран паузы без 3D-вида).
    void ApplyMouseDelta(Camera& camera) {
        camera.ProcessMouse(m_frameMouseDeltaX, m_frameMouseDeltaY);
    }

    // Смещение мыши ЗА ТЕКУЩИЙ КАДР (снимок сделан в Update). Читать можно
    // сколько угодно раз и из скольких угодно мест — значение не «съедается».
    float MouseDeltaX() const { return m_frameMouseDeltaX; }
    float MouseDeltaY() const { return m_frameMouseDeltaY; }

    // Сырой сдвиг колеса за кадр (для UI без привязанных действий — скролл
    // хотбара, скролл списков и т.п.), без ремаппинга
    int RawScrollDelta() const { return m_frameScrollDelta; }

    // Положение курсора в окне, в пикселях от левого верхнего угла (последнее
    // пришедшее событие мыши).
    float MouseX() const { return m_lastX; }
    float MouseY() const { return m_lastY; }

private:
    // Кнопки мыши читаются poll-ом (glfwGetMouseButton) в Update(), не
    // колбэком — тем же способом, что и клавиатура.
    void OnMouseMove(double xpos, double ypos) {
        if (m_firstMouse) {
            m_lastX = (float)xpos;
            m_lastY = (float)ypos;
            m_firstMouse = false;
        }
        m_mouseDeltaX += (float)xpos - m_lastX;
        m_mouseDeltaY += m_lastY - (float)ypos; // Y инвертирован (экран вниз, мир вверх)
        m_lastX = (float)xpos;
        m_lastY = (float)ypos;
    }

    InputMap m_actions;

    bool m_firstMouse = true;
    float m_lastX = 0.0f, m_lastY = 0.0f;
    float m_mouseDeltaX = 0.0f, m_mouseDeltaY = 0.0f; // накопитель между кадрами
    int m_scrollDelta = 0;
    // Снимок на кадр (см. Update): то, что видят ВСЕ читатели весь кадр.
    float m_frameMouseDeltaX = 0.0f, m_frameMouseDeltaY = 0.0f;
    int m_frameScrollDelta = 0;
};

// ---------------------------------------------------------------------------
// WindowRawInput — «сырой» ввод скриптам поверх пары InputSystem + Window:
// смещение мыши берётся из снимка кадра InputSystem, захват курсора отдаётся
// окну. Реализация интерфейса из core/RawInput.h для настоящей игры (в
// Play-режиме редактора источник другой — там ввод приходит из панели Game).
// ---------------------------------------------------------------------------
class WindowRawInput : public sage::RawInputSource {
public:
    WindowRawInput(InputSystem& input, Window& window) : m_input(&input), m_window(&window) {}

    glm::vec2 MouseDelta() const override { return {m_input->MouseDeltaX(), m_input->MouseDeltaY()}; }
    glm::vec2 MousePosition() const override { return {m_input->MouseX(), m_input->MouseY()}; }
    int ScrollDelta() const override { return m_input->RawScrollDelta(); }
    void SetMouseCaptured(bool captured) override { m_window->SetCursorCaptured(captured); }
    bool MouseCaptured() const override { return m_window->CursorCaptured(); }

private:
    InputSystem* m_input;
    Window* m_window;
};
