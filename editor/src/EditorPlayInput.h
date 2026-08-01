#pragma once
#include "sage/core/InputSystem.h"
#include "sage/core/RawInput.h"
#include "sage/core/Window.h"

// ---------------------------------------------------------------------------
// EditorPlayInput — «сырой» ввод для скриптов в Play-режиме РЕДАКТОРА.
//
// В собранной игре всё просто: окно одно, оно и есть игра (см. WindowRawInput).
// В редакторе игра живёт в панели Game среди десятка других панелей, поэтому у
// ввода появляется условие: он идёт в игру, ТОЛЬКО пока панель Game в фокусе.
// Иначе набор имени сущности в Hierarchy заодно вёл бы игрока вперёд, а
// захваченный игрой курсор не давал бы кликнуть ни по одной кнопке редактора.
//
// Отсюда две вещи, которых нет у WindowRawInput:
//   • дельта мыши и колесо при потере фокуса — нули (игра «не видит» мышь);
//   • захват курсора ЖЕЛАЕМЫЙ и ФАКТИЧЕСКИЙ разведены: скрипт просит захват
//     один раз в OnStart, а редактор включает и выключает его по фокусу
//     (SyncCapture каждый кадр), не теряя желания скрипта.
// ---------------------------------------------------------------------------
class EditorPlayInput : public sage::RawInputSource {
public:
    EditorPlayInput(InputSystem& input, Window& window) : m_input(&input), m_window(&window) {}

    // Редактор сообщает сюда фокус панели Game раз в кадр (см. EditorLayer::OnUpdate).
    void SetGameFocused(bool focused) { m_gameFocused = focused; }
    bool GameFocused() const { return m_gameFocused; }

    // Приводит фактический захват курсора в соответствие с желанием скрипта и
    // текущим фокусом. Звать раз в кадр, пока идёт Play.
    void SyncCapture() { m_window->SetCursorCaptured(m_wantCapture && m_gameFocused); }

    // Play закончился (Stop) — курсор безусловно возвращается человеку.
    void ReleaseCapture() {
        m_wantCapture = false;
        m_window->SetCursorCaptured(false);
    }

    glm::vec2 MouseDelta() const override {
        if (!m_gameFocused) return {0.0f, 0.0f};
        return {m_input->MouseDeltaX(), m_input->MouseDeltaY()};
    }
    glm::vec2 MousePosition() const override { return {m_input->MouseX(), m_input->MouseY()}; }
    int ScrollDelta() const override { return m_gameFocused ? m_input->RawScrollDelta() : 0; }

    void SetMouseCaptured(bool captured) override {
        m_wantCapture = captured;
        SyncCapture();
    }
    // Сообщаем ЖЕЛАНИЕ скрипта, а не состояние окна: скрипт спрашивает «я всё
    // ещё в режиме обзора?», и ответ не должен меняться от того, что человек
    // на секунду кликнул в Inspector.
    bool MouseCaptured() const override { return m_wantCapture; }

private:
    InputSystem* m_input;
    Window* m_window;
    bool m_gameFocused = false;
    bool m_wantCapture = false;
};
