#include "SelfTestCommon.h"

#include <GLFW/glfw3.h>

// ============================================================================
//  ЗАХВАТ КУРСОРА ДЕРЖИТСЯ, А МЫШЬ ИНТЕРФЕЙСА СТОИТ НА МЕСТЕ
// ============================================================================
//
// Жалоба: «курсор не захватывается — он невидимый, но гуляет и не держится»,
// и в игре, и при облёте камеры редактора. Две причины, обе проверяются здесь
// в живом кадре:
//   • режим курсора окна сбросили со стороны (фокус, alt-tab, бэкенд
//     интерфейса), а окно верило своему флагу и захват не возвращало;
//   • «виртуальное» положение курсора в захвате уезжало за край панели, и
//     интерфейс уводил наведение и фокус на соседние панели.
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
        case 1:
            // Сбрасываем режим «со стороны» и уводим мышь интерфейса далеко за
            // край — так, как это делает виртуальный курсор захвата.
            glfwSetInputMode(handle, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            io.AddMousePosEvent(m_mousePin.x + 5000.0f, m_mousePin.y - 3000.0f);
            return;
        case 2: {
            const bool held = glfwGetInputMode(handle, GLFW_CURSOR) == GLFW_CURSOR_DISABLED;
            const bool pinned = std::fabs(io.MousePos.x - m_mousePin.x) < 0.5f &&
                                std::fabs(io.MousePos.y - m_mousePin.y) < 0.5f;
            window.SetCursorCaptured(false);
            m_cursorProbeStep = -1;
            if (held && pinned) {
                LOG_INFO("Editor") << "CURSOR_LOCK: OK — сброшенный со стороны захват вернулся сам, "
                                      "мышь интерфейса стоит на месте, пока курсор захвачен";
            } else {
                LOG_ERROR("Editor") << "CURSOR_LOCK: FAIL — " << (held ? "" : "захват не вернулся после сброса; ")
                                    << (pinned ? "" : "мышь интерфейса уехала вслед за виртуальным курсором");
            }
            return;
        }
        default: return;
    }
}
