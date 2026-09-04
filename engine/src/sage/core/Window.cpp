#include "Window.h"
#include "Log.h"
#include "Paths.h"
#include <stdexcept>
#include <string>

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

void Window::ForwardChar(GLFWwindow* handle, unsigned int codepoint) {
    auto* win = static_cast<Window*>(glfwGetWindowUserPointer(handle));
    if (win && win->m_charFn) win->m_charFn(codepoint);
}

void Window::ForwardKey(GLFWwindow* handle, int key, int, int action, int mods) {
    auto* win = static_cast<Window*>(glfwGetWindowUserPointer(handle));
    if (!win) return;
    for (const KeyFn& fn : win->m_keyFns) fn(key, action, mods);
}

void Window::ForwardMouseButton(GLFWwindow* handle, int button, int action, int mods) {
    auto* win = static_cast<Window*>(glfwGetWindowUserPointer(handle));
    if (!win) return;
    for (const MouseButtonFn& fn : win->m_mouseButtonFns) fn(button, action, mods);
}

void Window::ForwardFileDrop(GLFWwindow* handle, int count, const char** paths) {
    auto* win = static_cast<Window*>(glfwGetWindowUserPointer(handle));
    if (!win || !win->m_fileDropFn || count <= 0 || !paths) return;
    // Копируем СЕЙЧАС: GLFW владеет этими строками только на время колбэка, а
    // подписчик почти наверняка отложит работу до кадра (открыть диалог,
    // спросить подтверждение). Указатели к тому моменту уже недействительны.
    std::vector<std::string> list;
    list.reserve((size_t)count);
    for (int i = 0; i < count; ++i)
        if (paths[i]) list.emplace_back(paths[i]);
    if (!list.empty()) win->m_fileDropFn(list);
}

// ПОЧЕМУ GLFW отказал — его собственными словами.
//
// Без этого колбэка причина теряется целиком: наружу приходит только «не
// удалось создать окно», а GLFW в этот момент знает точный текст — «WGL:
// драйвер не поддерживает OpenGL 3.3», «нет подходящего пиксельного формата»,
// «не найден OpenGL». Именно этот текст и отвечает на вопрос, почему на одном
// ПК запускается, а на другом нет; без него разбор упирается в переписку.
static void GlfwErrorCallback(int code, const char* description) {
    LOG_ERROR("Window") << "GLFW " << code << ": " << (description ? description : "(без текста)");
}

Window::Window(int width, int height, const std::string& title, Params params)
    : m_width(width), m_height(height) {

    glfwSetErrorCallback(&GlfwErrorCallback);
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

    // Отладочный контекст по SAGE_GPU_DEBUG=1.
    //
    // Канал сообщений драйвера (GL_KHR_debug) движок включает всегда, но БЕЗ
    // отладочного контекста драйвер не обязан ничего в него присылать: часть
    // реализаций молчит, часть отдаёт только критичное. Просить это у контекста
    // по умолчанию нельзя — на некоторых драйверах отладочный контекст заметно
    // медленнее, и платить за это каждым запуском ради строк, которые обычно не
    // нужны, неправильно. Поэтому: обычный запуск — как раньше, разбор поломки
    // — с переменной.
    if (const std::string dbg = sage::EnvString("SAGE_GPU_DEBUG"); !dbg.empty() && dbg != "0")
        glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE);
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
        // Причину GLFW уже написал в лог колбэком выше. Наружу отдаём текст,
        // по которому человеку понятно, что делать: почти всегда это
        // «драйвер видеокарты не умеет OpenGL 3.3».
        glfwTerminate();
        LOG_ERROR("Window") << "Не удалось создать окно GLFW";
        throw std::runtime_error(
            "Не удалось создать окно с OpenGL 3.3. Обычно это значит, что драйвер видеокарты "
            "устарел или запуск идёт через удалённый рабочий стол / виртуальную машину без "
            "3D-ускорения. Точная причина — строкой GLFW в логе.");
    }
    LOG_INFO("Window") << "Окно создано: " << createW << "x" << createH
                       << ", контекст OpenGL 3.3 получен";
    m_width = createW;
    m_height = createH;

    glfwMakeContextCurrent(m_handle);
    glfwSwapInterval(params.VSync ? 1 : 0); // вертикальная синхронизация
    glfwSetWindowUserPointer(m_handle, this);
    glfwSetFramebufferSizeCallback(m_handle, FramebufferSizeCallback);
    glfwSetCursorPosCallback(m_handle, &Window::ForwardCursorPos);
    glfwSetScrollCallback(m_handle, &Window::ForwardScroll);
    glfwSetCharCallback(m_handle, &Window::ForwardChar);
    glfwSetKeyCallback(m_handle, &Window::ForwardKey);
    glfwSetMouseButtonCallback(m_handle, &Window::ForwardMouseButton);
    glfwSetDropCallback(m_handle, &Window::ForwardFileDrop);

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

void Window::SetCursorCaptured(bool captured) {
    if (!m_handle || captured == m_cursorCaptured) return;
    m_cursorCaptured = captured;
    glfwSetInputMode(m_handle, GLFW_CURSOR, captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    // Сырой ввод мыши, если драйвер умеет: в захвате нам нужно физическое
    // смещение, а не ускорение/масштабирование рабочего стола — иначе
    // чувствительность обзора зависит от настроек ОС, а не от игры.
    if (glfwRawMouseMotionSupported())
        glfwSetInputMode(m_handle, GLFW_RAW_MOUSE_MOTION, captured ? GLFW_TRUE : GLFW_FALSE);
}

void Window::OnResize(int width, int height) {
    // Только запоминаем новый размер. Обновление viewport под этот размер —
    // задача графического слоя (Application каждый кадр выставляет viewport
    // экранного буфера через rhi::GraphicsDevice), окно к GL не обращается.
    m_width = width;
    m_height = height;
}
