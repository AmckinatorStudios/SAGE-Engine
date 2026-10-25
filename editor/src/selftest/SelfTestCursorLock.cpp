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
//   • программный захват (sage::CursorLockMode::Software): курсор спрятан и
//     возвращается в центр окна;
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
            m_probeLockRestore = m_settings.CursorLock;
            m_settings.CursorLock = sage::CursorLockMode::System;
            window.SetCursorLockMode(sage::CursorLockMode::System);
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
            // Дальше — программный захват. Настройкой, а не прямо окну:
            // OnUpdate каждый кадр ставит окну способ из настроек.
            m_settings.CursorLock = sage::CursorLockMode::Software;
            return;
        }
        case 3:
            window.SetCursorCaptured(true);
            return;
        case 4: {
            // Прошёл PollEvents: курсор спрятан (не отключён), а у окна в
            // фокусе — ещё и в центре. Фокус у окна xvfb без оконного
            // менеджера бывает не всегда — тогда центр не проверяется.
            const bool hidden = glfwGetInputMode(handle, GLFW_CURSOR) == GLFW_CURSOR_HIDDEN;
            const bool focused = glfwGetWindowAttrib(handle, GLFW_FOCUSED) == GLFW_TRUE;
            double cx = 0, cy = 0;
            int w = 0, h = 0;
            glfwGetCursorPos(handle, &cx, &cy);
            glfwGetWindowSize(handle, &w, &h);
            const bool centred = !focused || (std::fabs(cx - w * 0.5) < 1.5 && std::fabs(cy - h * 0.5) < 1.5);
            window.SetCursorCaptured(false);
            const bool freed = glfwGetInputMode(handle, GLFW_CURSOR) == GLFW_CURSOR_NORMAL;
            m_settings.CursorLock = m_probeLockRestore;
            // Окно панели из проверки многооконности закрылось — захват о нём знает.
            const bool forgot = m_detachedProbeWindow == nullptr ||
                                (cursorcapture::LastForgottenWindow() == m_detachedProbeWindow &&
                                 window.CursorCaptureTarget() == handle);
            m_cursorProbeStep = -1;
            if (hidden && centred && freed && forgot) {
                LOG_INFO("Editor") << "CURSOR_SOFTWARE: OK — программный захват прячет курсор"
                                   << (focused ? " и держит его в центре окна" : " (окно без фокуса — центр не проверен)")
                                   << ", закрытое окно панели захват забыл";
            } else {
                LOG_ERROR("Editor") << "CURSOR_SOFTWARE: FAIL — спрятан " << hidden << ", в центре " << centred
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
    // узнать об этом сам (проверяет CURSOR_SOFTWARE).
    m_detachedProbeWindow = panelWindow;

    if (keyReached && panelHeld && mainFree && released) {
        LOG_INFO("Editor") << "DETACHED_INPUT: OK — клавиша из окна панели дошла до ввода движка, "
                              "захват курсора держит окно панели, а не главное";
    } else {
        LOG_ERROR("Editor") << "DETACHED_INPUT: FAIL — клавиша дошла " << keyReached << ", окно панели захвачено "
                            << panelHeld << ", главное свободно " << mainFree << ", отпущено " << released;
    }
}
