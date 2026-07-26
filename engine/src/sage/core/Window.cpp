#include "Window.h"
#include "Log.h"
#include <stdexcept>

static void FramebufferSizeCallback(GLFWwindow* handle, int width, int height) {
    auto* win = static_cast<Window*>(glfwGetWindowUserPointer(handle));
    if (win) win->OnResize(width, height);
}

void Window::ForwardCursorPos(GLFWwindow* handle, double x, double y) {
    auto* win = static_cast<Window*>(glfwGetWindowUserPointer(handle));
    if (win && win->m_cursorPosFn) win->m_cursorPosFn(x, y);
}

void Window::ForwardScroll(GLFWwindow* handle, double xoffset, double yoffset) {
    auto* win = static_cast<Window*>(glfwGetWindowUserPointer(handle));
    if (win && win->m_scrollFn) win->m_scrollFn(xoffset, yoffset);
}

Window::Window(int width, int height, const std::string& title, Params params)
    : m_width(width), m_height(height) {

    if (!glfwInit()) {
        LOG_ERROR("Window") << "Не удалось инициализировать GLFW";
        throw std::runtime_error("Не удалось инициализировать GLFW");
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
    glfwWindowHint(GLFW_RESIZABLE, params.Resizable ? GLFW_TRUE : GLFW_FALSE);
    if (params.Hidden) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    // sRGB-способный экранный буфер: позволяет включать аппаратную гамма-
    // коррекцию (GraphicsDevice::SetSRGBWrite) при рендере сцены напрямую в
    // экран без пост-процесса. Если драйвер не умеет — хинт игнорируется.
    glfwWindowHint(GLFW_SRGB_CAPABLE, GLFW_TRUE);
    if (params.Msaa > 0) glfwWindowHint(GLFW_SAMPLES, params.Msaa); // сглаживание экранного буфера

    // Полноэкранный/безрамочный режим использует текущий видеорежим монитора
    // (borderless = «окно во весь монитор без рамки», fullscreen = эксклюзивный).
    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    const GLFWvidmode* vmode = monitor ? glfwGetVideoMode(monitor) : nullptr;
    GLFWmonitor* useMonitor = nullptr;
    int createW = width, createH = height;
    if (params.Mode == sage::WindowMode::Fullscreen && monitor && vmode) {
        useMonitor = monitor;
        createW = vmode->width; createH = vmode->height;
    } else if (params.Mode == sage::WindowMode::Borderless && monitor && vmode) {
        // Безрамочное окно во весь монитор: подсказки под видеорежим + decorated=false.
        glfwWindowHint(GLFW_RED_BITS, vmode->redBits);
        glfwWindowHint(GLFW_GREEN_BITS, vmode->greenBits);
        glfwWindowHint(GLFW_BLUE_BITS, vmode->blueBits);
        glfwWindowHint(GLFW_REFRESH_RATE, vmode->refreshRate);
        glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
        createW = vmode->width; createH = vmode->height;
    }

    m_handle = glfwCreateWindow(createW, createH, title.c_str(), useMonitor, nullptr);
    if (!m_handle) {
        glfwTerminate();
        LOG_ERROR("Window") << "Не удалось создать окно GLFW";
        throw std::runtime_error("Не удалось создать окно GLFW");
    }
    m_width = createW;
    m_height = createH;

    glfwMakeContextCurrent(m_handle);
    glfwSwapInterval(params.VSync ? 1 : 0); // вертикальная синхронизация
    glfwSetWindowUserPointer(m_handle, this);
    glfwSetFramebufferSizeCallback(m_handle, FramebufferSizeCallback);
    glfwSetCursorPosCallback(m_handle, &Window::ForwardCursorPos);
    glfwSetScrollCallback(m_handle, &Window::ForwardScroll);

    // Загрузку драйвера (glad) и дефолтное состояние конвейера (depth test,
    // backface culling, бесшовные cubemap) выполняет rhi::GraphicsDevice::Init,
    // который Application создаёт сразу после окна. Здесь — только окно/контекст.
}

Window::~Window() {
    if (m_handle) glfwDestroyWindow(m_handle);
    glfwTerminate();
}

bool Window::ShouldClose() const {
    return glfwWindowShouldClose(m_handle);
}

void Window::SwapBuffers() {
    glfwSwapBuffers(m_handle);
}

void Window::PollEvents() {
    glfwPollEvents();
}

void Window::OnResize(int width, int height) {
    // Только запоминаем новый размер. Обновление viewport под этот размер —
    // задача графического слоя (Application каждый кадр выставляет viewport
    // экранного буфера через rhi::GraphicsDevice), окно к GL не обращается.
    m_width = width;
    m_height = height;
}
