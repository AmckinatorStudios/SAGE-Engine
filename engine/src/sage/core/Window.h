#pragma once
#include <GLFW/glfw3.h>
#include <string>
#include <functional>

#include "sage/core/Config.h"

// Обёртка над GLFW-окном + графическим контекстом. Само окно — чисто оконная
// система (GLFW): создание окна и контекста, обмен буферов, события. За загрузку
// драйвера и любое состояние конвейера отвечает rhi::GraphicsDevice, а не Window
// — поэтому здесь нет ни одного вызова OpenGL (граница «окно vs графика»).
class Window {
public:
    // Параметры создания окна (режим/ресайз/vsync/сглаживание) берутся из конфига.
    struct Params {
        sage::WindowMode Mode = sage::WindowMode::Windowed;
        bool Resizable = true;
        bool VSync = true;
        int Msaa = 0;
    };

    Window(int width, int height, const std::string& title)
        : Window(width, height, title, Params{}) {}
    Window(int width, int height, const std::string& title, Params params);
    ~Window();

    bool ShouldClose() const;
    void SwapBuffers();
    void PollEvents();

    GLFWwindow* Handle() const { return m_handle; }
    int Width() const { return m_width; }
    int Height() const { return m_height; }

    // Вызывается GLFW при изменении размера окна
    void OnResize(int width, int height);

    // Подписка на события мыши. GLFW даёт ровно ОДИН user pointer и один
    // колбэк каждого типа на окно — ими монопольно владеет Window (user
    // pointer нужен его resize-колбэку), а желающие получать события мыши
    // (InputSystem) подписываются через эти хуки, НЕ трогая GLFW напрямую.
    // Иначе повторный glfwSetWindowUserPointer(окно, не-Window) заставил бы
    // FramebufferSizeCallback кастовать чужой указатель к Window* (UB).
    using CursorPosFn = std::function<void(double x, double y)>;
    using ScrollFn = std::function<void(double xoffset, double yoffset)>;
    void SetCursorPosCallback(CursorPosFn fn) { m_cursorPosFn = std::move(fn); }
    void SetScrollCallback(ScrollFn fn) { m_scrollFn = std::move(fn); }

private:
    static void ForwardCursorPos(GLFWwindow* handle, double x, double y);
    static void ForwardScroll(GLFWwindow* handle, double xoffset, double yoffset);

    GLFWwindow* m_handle = nullptr;
    int m_width;
    int m_height;
    CursorPosFn m_cursorPosFn;
    ScrollFn m_scrollFn;
};
