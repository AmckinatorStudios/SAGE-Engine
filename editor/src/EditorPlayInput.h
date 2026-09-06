#pragma once
#include "sage/input/GlfwBridge.h"
#include "sage/input/InputSystem.h"

// ---------------------------------------------------------------------------
// EditorPlayInput — захват курсора для игры, запущенной ВНУТРИ редактора.
//
// В собранной игре всё просто: окно одно, оно и есть игра, и захват курсора
// исполняет мост к окну напрямую. В редакторе игра живёт в панели Game среди
// десятка других панелей, поэтому у захвата появляется условие: он действует,
// ТОЛЬКО пока панель Game в фокусе. Иначе захваченный игрой курсор не даст
// нажать ни одной кнопки редактора, и выйти из Play будет нечем.
//
// Отсюда главное, чего нет у обычного моста: захват ЖЕЛАЕМЫЙ и ФАКТИЧЕСКИЙ
// разведены. Скрипт просит захват один раз в OnStart, а редактор включает и
// выключает его по фокусу (SyncCapture каждый кадр), не теряя желания скрипта.
// Поэтому CursorCaptured() отвечает ЖЕЛАНИЕМ, а не состоянием окна: скрипт
// спрашивает «я всё ещё в режиме обзора?», и ответ не должен меняться от того,
// что человек на секунду щёлкнул в Inspector.
//
// Само чтение клавиш и мыши здесь ни при чём — им занимается общая система
// ввода движка (sage/input/InputSystem.h), одна и та же в редакторе и в игре.
// Пока ввод панели Game был отдельным классом, «в редакторе работает, а в
// сборке нет» приходилось ловить руками.
// ---------------------------------------------------------------------------
class EditorPlayInput : public sage::input::CursorControl {
public:
    // Мост к окну исполняет фактический захват. Указатель, а не ссылка в
    // конструкторе: и мост, и этот объект живут всё время работы редактора, а
    // связываются один раз при подключении слоя.
    void Attach(sage::input::GlfwBridge& bridge) { m_bridge = &bridge; }

    // Редактор сообщает сюда фокус панели Game раз в кадр (см. EditorLayer::OnUpdate).
    void SetGameFocused(bool focused) { m_gameFocused = focused; }
    bool GameFocused() const { return m_gameFocused; }

    // Приводит фактический захват в соответствие с желанием скрипта и текущим
    // фокусом. Звать раз в кадр, пока идёт Play.
    void SyncCapture() { Apply(m_wantCapture && m_gameFocused); }

    // Play закончился (Stop) — курсор безусловно возвращается человеку.
    void ReleaseCapture() {
        m_wantCapture = false;
        Apply(false);
    }

    void SetCursorCaptured(bool captured) override {
        m_wantCapture = captured;
        SyncCapture();
    }
    bool CursorCaptured() const override { return m_wantCapture; }

private:
    void Apply(bool captured) {
        if (m_bridge) m_bridge->SetCursorCaptured(captured);
    }

    sage::input::GlfwBridge* m_bridge = nullptr;
    bool m_gameFocused = false;
    bool m_wantCapture = false;
};
