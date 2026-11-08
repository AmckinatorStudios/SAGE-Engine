#include "sage/core/BorderlessWindow.h"

#include <GLFW/glfw3.h>

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <objbase.h>
#include <shobjidl.h>
#endif

namespace sage::display {

WindowRect BorderlessRect(int monitorX, int monitorY, int monitorW, int monitorH) {
    // +1 по высоте — чтобы окно НЕ совпадало с монитором (см. заголовок).
    return {monitorX, monitorY, monitorW, monitorH + 1};
}

void MarkFullscreenWindow(GLFWwindow* window, bool fullscreen) {
#ifdef _WIN32
    if (!window) return;
    HWND hwnd = glfwGetWin32Window(window);
    if (!hwnd) return;
    // COM на главном потоке мог быть не поднят: поднимаем; уже поднятый в
    // другом режиме (RPC_E_CHANGED_MODE) тоже годится — объект создастся.
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ITaskbarList2* bar = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskbarList2,
                                   reinterpret_cast<void**>(&bar))) && bar) {
        if (SUCCEEDED(bar->HrInit())) bar->MarkFullscreenWindow(hwnd, fullscreen ? TRUE : FALSE);
        bar->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
#else
    (void)window;
    (void)fullscreen;
#endif
}

} // namespace sage::display
