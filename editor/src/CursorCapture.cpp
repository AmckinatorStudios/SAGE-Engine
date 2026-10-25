#include "CursorCapture.h"

#include <GLFW/glfw3.h>
#include <imgui_impl_glfw.h>

#include "sage/app/Application.h"

namespace cursorcapture {

namespace {
// Уничтожение окна платформы у бэкенда ImGui: его вызов сохраняется, и
// перехват зовёт его последним.
void (*s_destroyWindow)(ImGuiViewport*) = nullptr;
// Флаг NoMouseCursorChange поставлен ЭТИМ модулем (а не человеком в
// настройках) — снимать его только тогда.
bool s_cursorChangeLocked = false;
void* s_lastForgotten = nullptr;
} // namespace

void* LastForgottenWindow() { return s_lastForgotten; }

void ForgetPlatformWindow(void* handle) {
    if (!handle) return;
    s_lastForgotten = handle;
    sage::Application::Get().GetWindow().ForgetWindow((GLFWwindow*)handle);
}

void InstallViewportInput() {
    // Бэкенд ImGui ставит окнам панелей свои обработчики событий, а наши
    // (Window) зовёт только для главного. С этим флагом он передаёт события
    // ЛЮБОГО своего окна дальше, Window узнаёт их хозяина (Window::Owner) и
    // отдаёт вводу движка.
    ImGui_ImplGlfw_SetCallbacksChainForAllWindows(true);
    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    if (pio.Platform_DestroyWindow && !s_destroyWindow) {
        s_destroyWindow = pio.Platform_DestroyWindow;
        pio.Platform_DestroyWindow = [](ImGuiViewport* vp) {
            // Панель вернули в док — её окно уничтожается. Захват, который
            // держал это окно, обязан узнать об этом ДО уничтожения: потом
            // любой вызов GLFW с этим указателем — обращение к мёртвой памяти.
            ForgetPlatformWindow(vp->PlatformHandle);
            s_destroyWindow(vp);
        };
    }
}

void BeforeImGuiFrame() {
    ImGuiIO& io = ImGui::GetIO();
    const bool captured = sage::Application::Get().GetWindow().CursorCaptured();
    if (captured && !(io.ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange)) {
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        s_cursorChangeLocked = true;
    } else if (!captured && s_cursorChangeLocked) {
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
        s_cursorChangeLocked = false;
    }
}

void CaptureForCurrentWindow(bool captured) {
    Window& window = sage::Application::Get().GetWindow();
    if (captured && ImGui::GetWindowViewport())
        window.SetCursorCaptureTarget((GLFWwindow*)ImGui::GetWindowViewport()->PlatformHandle);
    window.SetCursorCaptured(captured);
}

ImVec2 LookMouseDelta() {
    Window& window = sage::Application::Get().GetWindow();
    if (!window.CursorCaptured()) return ImGui::GetIO().MouseDelta;
    return ImVec2((float)window.FrameCursorDeltaX(), (float)window.FrameCursorDeltaY());
}

} // namespace cursorcapture
