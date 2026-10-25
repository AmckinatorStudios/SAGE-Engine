#include "Window.h"
#include "Log.h"
#include <stdexcept>

static void FramebufferSizeCallback(GLFWwindow* handle, int width, int height) {
    auto* win = static_cast<Window*>(glfwGetWindowUserPointer(handle));
    if (win) win->OnResize(width, height);
}

// ЧЕЙ ЭТО ВВОД. У своего окна user pointer — этот Window. У окон, которые
// заводит не Window (окна панелей редактора, их создаёт бэкенд ImGui), его нет,
// а события от них — того же приложения и того же человека: они достаются
// главному окну. Иначе панель Game, вытащенная отдельным окном, не получала ни
// клавиш, ни мыши — игра в ней не управлялась.
static Window* s_primary = nullptr;

Window* Window::Owner(GLFWwindow* handle) {
    if (auto* win = static_cast<Window*>(glfwGetWindowUserPointer(handle))) return win;
    return s_primary;
}

void Window::ForwardCursorPos(GLFWwindow* handle, double x, double y) {
    if (Window* win = Owner(handle)) win->EmitCursorPos(x, y);
}

void Window::TouchInput() { m_lastInput = glfwGetTime(); }

double Window::SecondsSinceInput() const { return glfwGetTime() - m_lastInput; }

void Window::EmitCursorPos(double x, double y) {
    TouchInput();
    if (m_haveLast) {
        m_frameDx += x - m_lastX;
        m_frameDy += y - m_lastY;
    }
    m_lastX = x;
    m_lastY = y;
    m_haveLast = true;
    if (m_cursorPosFn) m_cursorPosFn(x, y);
}

void Window::ForwardScroll(GLFWwindow* handle, double xoffset, double yoffset) {
    Window* win = Owner(handle);
    if (win) win->TouchInput();
    if (win && win->m_scrollFn) win->m_scrollFn(xoffset, yoffset);
}

void Window::ForwardChar(GLFWwindow* handle, unsigned int codepoint) {
    Window* win = Owner(handle);
    if (win) win->TouchInput();
    if (win && win->m_charFn) win->m_charFn(codepoint);
}

void Window::ForwardKey(GLFWwindow* handle, int key, int, int action, int mods) {
    if (Window* win = Owner(handle)) win->EmitKey(key, action, mods);
}

void Window::EmitKey(int key, int action, int mods) {
    TouchInput();
    for (const KeyFn& fn : m_keyFns) fn(key, action, mods);
}

void Window::ForwardMouseButton(GLFWwindow* handle, int button, int action, int mods) {
    Window* win = Owner(handle);
    if (!win) return;
    win->TouchInput();
    for (const MouseButtonFn& fn : win->m_mouseButtonFns) fn(button, action, mods);
}

void Window::ForwardCursorEnter(GLFWwindow* handle, int entered) {
    Window* win = Owner(handle);
    if (!win) return;
    // Курсор перешёл в другое окно: его координаты — от другого угла, и
    // разница с прошлым положением смещением не является.
    win->m_haveLast = false;
    if (win->m_cursorEnterFn) win->m_cursorEnterFn(entered != 0);
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
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif
    glfwWindowHint(GLFW_RESIZABLE, params.Resizable ? GLFW_TRUE : GLFW_FALSE);
    if (params.Hidden) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    // Развернуть — ПОДСКАЗКОЙ ДО СОЗДАНИЯ, а не glfwMaximizeWindow после.
    // Разворот после создания даёт видимый прыжок: окно успевает появиться в
    // заказанном размере, отрисовать первый кадр и только потом растянуться.
    if (params.Maximized && params.Mode == sage::WindowMode::Windowed && !params.Hidden)
        glfwWindowHint(GLFW_MAXIMIZED, GLFW_TRUE);
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
        // ВТОРАЯ ПОПЫТКА — БЕЗ ЗАПРОСОВ «СВЕРХ НЕОБХОДИМОГО». Сглаживание
        // экранного буфера и sRGB — пожелания, а не условия: часть драйверов
        // (виртуальные машины, старые Mac, удалённый рабочий стол) не даёт
        // такой формат пикселей вовсе, и из-за пожелания программа не
        // запускалась совсем. Без них картинка чуть грубее по краям — и только.
        LOG_WARN("Window") << "Окно (сглаживание x" << params.Msaa
                           << ", sRGB) не создалось — пробую без них";
        glfwWindowHint(GLFW_SAMPLES, 0);
        glfwWindowHint(GLFW_SRGB_CAPABLE, GLFW_FALSE);
        m_handle = glfwCreateWindow(createW, createH, title.c_str(), useMonitor, nullptr);
    }
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
    // Размер берём У ОКНА, а не из заказанного: развёрнутое окно получает
    // размер рабочей области монитора, и запомненные 1600x900 разъехались бы с
    // настоящим кадром — вьюпорт считал бы по ним лучи пикинга. По этой же
    // причине в лог идёт ФАКТИЧЕСКИЙ размер: заказанный отвечает не на тот
    // вопрос, когда разбираешься, почему картинка не того размера.
    glfwGetFramebufferSize(m_handle, &createW, &createH);
    m_width = createW;
    m_height = createH;
    LOG_INFO("Window") << "Окно создано: " << createW << "x" << createH
                       << ", контекст OpenGL 3.3 получен";

    glfwMakeContextCurrent(m_handle);
    glfwSwapInterval(params.VSync ? 1 : 0); // вертикальная синхронизация
    glfwSetWindowUserPointer(m_handle, this);
    if (!s_primary) s_primary = this;
    glfwSetFramebufferSizeCallback(m_handle, FramebufferSizeCallback);
    // Свёрнуто ли окно и в фокусе ли оно — из событий: спрашивать систему об
    // этом каждый кадр на X11 значит обмен с сервером на каждый вопрос.
    glfwSetWindowIconifyCallback(m_handle, [](GLFWwindow* h, int iconified) {
        if (auto* win = static_cast<Window*>(glfwGetWindowUserPointer(h))) win->m_iconified = iconified != 0;
    });
    glfwSetWindowFocusCallback(m_handle, [](GLFWwindow* h, int focused) {
        if (auto* win = static_cast<Window*>(glfwGetWindowUserPointer(h))) win->EmitFocus(focused != 0);
    });
    // Считаемся в фокусе, пока система не сказала обратного СОБЫТИЕМ: окно без
    // оконного менеджера (xvfb, CI) фокуса не получает никогда, и вопрос «в
    // фокусе ли» там отвечал бы «нет» — фоновый лимит кадров душил бы прогоны.
    m_focused = true;
    m_lastInput = glfwGetTime();
    glfwSetCursorPosCallback(m_handle, &Window::ForwardCursorPos);
    glfwSetScrollCallback(m_handle, &Window::ForwardScroll);
    glfwSetCharCallback(m_handle, &Window::ForwardChar);
    glfwSetKeyCallback(m_handle, &Window::ForwardKey);
    glfwSetMouseButtonCallback(m_handle, &Window::ForwardMouseButton);
    glfwSetDropCallback(m_handle, &Window::ForwardFileDrop);
    glfwSetCursorEnterCallback(m_handle, &Window::ForwardCursorEnter);

    // Загрузку драйвера (glad) и дефолтное состояние конвейера (depth test,
    // backface culling, бесшовные cubemap) выполняет rhi::GraphicsDevice::Init,
    // который Application создаёт сразу после окна. Здесь — только окно/контекст.
}

Window::~Window() {
    if (s_primary == this) s_primary = nullptr;
    if (m_handle) glfwDestroyWindow(m_handle);
    glfwTerminate();
}

bool Window::ShouldClose() const {
    return glfwWindowShouldClose(m_handle);
}

void Window::CancelClose() {
    glfwSetWindowShouldClose(m_handle, GLFW_FALSE);
}

void Window::SwapBuffers() {
    glfwSwapBuffers(m_handle);
}

void Window::PollEvents() {
    m_frameDx = m_frameDy = 0.0;
    glfwPollEvents();
    AfterEvents();
}

void Window::WaitEvents(double timeoutSeconds) {
    // Смещение за кадр НЕ сбрасывается: ожидание идёт после PollEvents того же
    // кадра (досып до лимита), и сброс потерял бы уже собранное движение мыши.
    glfwWaitEventsTimeout(timeoutSeconds > 0.0 ? timeoutSeconds : 0.0);
    AfterEvents();
}

void Window::AfterEvents() {
    if (!m_handle || !m_cursorCaptured) return;
    // ЗАХВАТ ДЕРЖИТСЯ, А НЕ СТАВИТСЯ ОДИН РАЗ. Режим курсора окна может
    // сбросить кто-то сторонний — бэкенд интерфейса, который сам ставит вид
    // курсора своим окнам. Окно сверяет фактический режим с желаемым и
    // возвращает его. Сверка дешёвая: режим GLFW помнит сам и систему не
    // спрашивает. Потеря фокуса здесь ни при чём: GLFW отпускает курсор у
    // окна без фокуса и захватывает снова при возврате, не меняя режима.
    if (glfwGetInputMode(CursorCaptureTarget(), GLFW_CURSOR) != GLFW_CURSOR_DISABLED) ApplyCursorMode();
}

void Window::EmitFocus(bool focused) {
    const bool changed = focused != m_focused;
    m_focused = focused;
    if (!changed) return;
    // Свободный курсор до ухода и после возврата — в разных местах: разница —
    // не движение. В захвате положение виртуальное и непрерывное (см.
    // CursorMotion::FocusChanged) — сброс выбросил бы первое движение.
    if (!m_cursorCaptured) m_haveLast = false;
    for (const FocusFn& fn : m_focusFns) fn(focused);
}

void Window::SetCursorCaptured(bool captured) {
    if (!m_handle) return;
    const bool changed = captured != m_cursorCaptured;
    m_cursorCaptured = captured;
    if (changed) m_haveLast = false;   // смещение от положения до захвата — не движение
    // Флаг совпал — но режим окна мог уйти сам (см. AfterEvents): ставим, если
    // он не тот, а не «раз флаг тот, значит и окно в порядке».
    if (changed || (captured && glfwGetInputMode(CursorCaptureTarget(), GLFW_CURSOR) != GLFW_CURSOR_DISABLED))
        ApplyCursorMode();
}

void Window::SetCursorCaptureTarget(GLFWwindow* target) {
    if (target == m_handle) target = nullptr;
    if (target == m_captureTarget || !m_handle) return;
    // Прежнее окно отпускается: оно остаётся на экране (в нём живут другие
    // панели), и спрятанный в нём курсор был бы тем же «курсор пропал».
    if (m_cursorCaptured) ApplyCursorModeTo(CursorCaptureTarget(), false);
    m_captureTarget = target;
    m_haveLast = false;
    if (m_cursorCaptured) ApplyCursorMode();
}

void Window::ForgetWindow(GLFWwindow* handle) {
    if (!handle || handle != m_captureTarget) return;
    m_captureTarget = nullptr;
    m_haveLast = false;
    if (m_cursorCaptured) ApplyCursorMode();
}

void Window::ApplyCursorMode() { ApplyCursorModeTo(CursorCaptureTarget(), m_cursorCaptured); }

void Window::ApplyCursorModeTo(GLFWwindow* target, bool captured) {
    if (!target) return;
    // Сырой ввод — ДО смены режима: GLFW включает его при отключении курсора,
    // и первое же смещение после захвата должно быть уже физическим, а не
    // ускоренным рабочим столом (иначе чувствительность обзора зависит от
    // настроек ОС, а не от игры).
    if (glfwRawMouseMotionSupported())
        glfwSetInputMode(target, GLFW_RAW_MOUSE_MOTION, captured ? GLFW_TRUE : GLFW_FALSE);
    glfwSetInputMode(target, GLFW_CURSOR, captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
}

void Window::OnResize(int width, int height) {
    // Только запоминаем новый размер. Обновление viewport под этот размер —
    // задача графического слоя (Application каждый кадр выставляет viewport
    // экранного буфера через rhi::GraphicsDevice), окно к GL не обращается.
    m_width = width;
    m_height = height;
}
