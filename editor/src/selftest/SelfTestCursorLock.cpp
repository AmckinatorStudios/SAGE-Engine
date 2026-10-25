#include "SelfTestCommon.h"

#include <GLFW/glfw3.h>

#include "../CursorCapture.h"

// ============================================================================
//  ЗАХВАТ КУРСОРА ДЕРЖИТСЯ, А МЫШЬ ИНТЕРФЕЙСА СТОИТ НА МЕСТЕ
// ============================================================================
//
// Жалоба: «курсор не захватывается — он невидимый, но гуляет и не держится»,
// и в игре, и при облёте камеры редактора. Проверяется в живом кадре:
//   • режим курсора окна сбросили со стороны (фокус, alt-tab, бэкенд
//     интерфейса), а окно верило своему флагу и захват не возвращало;
//   • «виртуальное» положение курсора в захвате уезжало за край панели, и
//     интерфейс уводил наведение и фокус на соседние панели;
//   • облёт камеры в захвате брал смещение из мыши интерфейса — а она стоит
//     на месте, и камера не поворачивалась (смещение — из окна, LookMouseDelta);
//   • захват, который просят каждый кадр, терял первое движение мыши каждого
//     кадра — обзор недокручивался (CURSOR_STEADY);
//   • потеря и возврат фокуса давали скачок обзора;
//   • окно панели, закрытое ImGui, захват забыл (иначе держал бы мёртвое окно).
// Запускается ПОСЛЕ остальных живых проверок (последняя — Lua-тесты в Play):
// захват курсора им мешал бы.
void EditorLayer::TickCursorLockProbe() {
    if (m_cursorProbeStep < 0) return;
    if (!std::getenv("SAGE_EDITOR_SELFTEST")) return;
    if (!m_tweenChecked || m_luaSelfTestStep != -1 || m_luaTests.Running || m_play.Active()) return;
    auto& window = sage::Application::Get().GetWindow();
    GLFWwindow* handle = window.Handle();
    ImGuiIO& io = ImGui::GetIO();
    switch (m_cursorProbeStep++) {
        case 0:
            window.SetCursorCaptured(true);
            return;
        case 1: {
            // Сбрасываем режим «со стороны» и уводим мышь интерфейса далеко за
            // край — так, как это делает виртуальный курсор захвата.
            glfwSetInputMode(handle, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            io.AddMousePosEvent(m_mousePin.x + 5000.0f, m_mousePin.y - 3000.0f);
            // Движение мыши в захвате: облёт обязан его увидеть.
            window.EmitCursorPos(100.0, 100.0);
            const double before = window.FrameCursorDeltaX();
            window.EmitCursorPos(220.0, 100.0);
            m_cursorLookOk = std::fabs(window.FrameCursorDeltaX() - before - 120.0) < 0.01 &&
                             std::fabs(LookMouseDelta().x - (float)window.FrameCursorDeltaX()) < 0.01f;
            return;
        }
        case 2: {
            const bool held = glfwGetInputMode(handle, GLFW_CURSOR) == GLFW_CURSOR_DISABLED;
            const bool pinned = std::fabs(io.MousePos.x - m_mousePin.x) < 0.5f &&
                                std::fabs(io.MousePos.y - m_mousePin.y) < 0.5f;
            window.SetCursorCaptured(false);
            if (held && pinned && m_cursorLookOk) {
                LOG_INFO("Editor") << "CURSOR_LOCK: OK — сброшенный со стороны захват вернулся сам, "
                                      "мышь интерфейса стоит на месте, облёт получает смещение мыши";
            } else {
                LOG_ERROR("Editor") << "CURSOR_LOCK: FAIL — " << (held ? "" : "захват не вернулся после сброса; ")
                                    << (pinned ? "" : "мышь интерфейса уехала вслед за виртуальным курсором; ")
                                    << (m_cursorLookOk ? "" : "смещение мыши в захвате не дошло до облёта");
            }
            return;
        }
        case 3:
            // Захват ПРОСЯТ КАЖДЫЙ КАДР (Play редактора, скрипт в Update) — и
            // ни один такой запрос не должен выбрасывать движение мыши.
            m_playInputBridge.SetCursorCaptured(true);
            window.EmitCursorPos(300.0, 200.0);   // точка отсчёта после захвата
            for (int k = 1; k <= 4; ++k) {
                m_playInputBridge.SetCursorCaptured(true);
                window.EmitCursorPos(300.0 + 10.0 * k, 200.0);
            }
            return;
        case 4:
            // События прошлого шага разобраны в начале этого кадра.
            m_cursorSteadyDelta = m_play.Input().MouseDelta().x;
            // Потеря и возврат фокуса: курсор за это время «уехал» далеко —
            // это не движение, обзор не прыгает; дальше движение идёт как шло.
            window.EmitFocus(false);
            window.EmitFocus(true);
            window.EmitCursorPos(5000.0, -300.0);
            window.EmitCursorPos(5003.0, -300.0);
            return;
        case 5: {
            const float afterFocus = m_play.Input().MouseDelta().x;
            const bool held = glfwGetInputMode(handle, GLFW_CURSOR) == GLFW_CURSOR_DISABLED;
            m_playInputBridge.SetCursorCaptured(false);
            const bool freed = glfwGetInputMode(handle, GLFW_CURSOR) == GLFW_CURSOR_NORMAL;
            // Окно панели из проверки многооконности закрылось — захват о нём знает.
            const bool forgot = m_detachedProbeWindow == nullptr ||
                                (cursorcapture::LastForgottenWindow() == m_detachedProbeWindow &&
                                 window.CursorCaptureTarget() == handle);
            m_cursorProbeStep = -1;
            const bool steady = std::fabs(m_cursorSteadyDelta - 40.0f) < 0.01f;
            const bool focusOk = std::fabs(afterFocus - 3.0f) < 0.01f;
            if (steady && focusOk && held && freed && forgot) {
                LOG_INFO("Editor") << "CURSOR_STEADY: OK — захват, запрошенный каждый кадр, не теряет движения мыши, "
                                      "потеря и возврат фокуса не дают скачка, захват держится системой, "
                                      "закрытое окно панели захват забыл";
            } else {
                LOG_ERROR("Editor") << "CURSOR_STEADY: FAIL — движение за кадры " << m_cursorSteadyDelta
                                    << " (ждали 40), после фокуса " << afterFocus << " (ждали 3), держится " << held
                                    << ", отпущен " << freed << ", закрытое окно забыто " << forgot;
            }
            return;
        }
        default: return;
    }
}

// ============================================================================
//  ПАНЕЛЬ, ВЫТАЩЕННАЯ ОТДЕЛЬНЫМ ОКНОМ: ЕЁ ВВОД ДОХОДИТ ДО ИГРЫ, ЗАХВАТ — ЕЙ
// ============================================================================
//
// Окно панели заводит бэкенд ImGui со своими обработчиками событий. Раньше
// дальше ImGui они не шли: игра в вытащенной панели Game не получала ни
// клавиш, ни мыши, а захват курсора ставился главному окну. Здесь событие
// подаётся тем самым обработчиком, который GLFW вызвал бы для окна панели.
void EditorLayer::CheckDetachedWindowInput(void* platformHandle) {
    auto& window = sage::Application::Get().GetWindow();
    GLFWwindow* panelWindow = (GLFWwindow*)platformHandle;
    if (!m_detachedKeyHooked) {
        window.AddKeyCallback([this](int key, int action, int) {
            if (key == GLFW_KEY_F13 && action == GLFW_PRESS) m_detachedKeySeen = true;
        });
        m_detachedKeyHooked = true;
    }
    m_detachedKeySeen = false;
    // Обработчик, который стоит у окна сейчас (ставим его же обратно).
    GLFWkeyfun keyFn = glfwSetKeyCallback(panelWindow, nullptr);
    glfwSetKeyCallback(panelWindow, keyFn);
    if (keyFn) {
        keyFn(panelWindow, GLFW_KEY_F13, 0, GLFW_PRESS, 0);
        keyFn(panelWindow, GLFW_KEY_F13, 0, GLFW_RELEASE, 0);
    }
    const bool keyReached = m_detachedKeySeen;

    window.SetCursorCaptureTarget(panelWindow);
    window.SetCursorCaptured(true);
    const bool panelHeld = glfwGetInputMode(panelWindow, GLFW_CURSOR) == GLFW_CURSOR_DISABLED;
    const bool mainFree = glfwGetInputMode(window.Handle(), GLFW_CURSOR) == GLFW_CURSOR_NORMAL;
    window.SetCursorCaptured(false);
    const bool released = glfwGetInputMode(panelWindow, GLFW_CURSOR) == GLFW_CURSOR_NORMAL;
    // Цель захвата остаётся на окне панели: когда его закроют, захват обязан
    // узнать об этом сам (проверяет CURSOR_STEADY).
    m_detachedProbeWindow = panelWindow;

    if (keyReached && panelHeld && mainFree && released) {
        LOG_INFO("Editor") << "DETACHED_INPUT: OK — клавиша из окна панели дошла до ввода движка, "
                              "захват курсора держит окно панели, а не главное";
    } else {
        LOG_ERROR("Editor") << "DETACHED_INPUT: FAIL — клавиша дошла " << keyReached << ", окно панели захвачено "
                            << panelHeld << ", главное свободно " << mainFree << ", отпущено " << released;
    }
}
