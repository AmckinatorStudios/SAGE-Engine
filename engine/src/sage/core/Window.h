#pragma once
#include <GLFW/glfw3.h>
#include <string>
#include <functional>
#include <vector>

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
        // Окно без показа на экране. Нужно инструментам и тестам, которым
        // требуется только графический КОНТЕКСТ: рендер идёт в offscreen-буферы,
        // а мигающее пустое окно на экране — побочный эффект, которого быть
        // не должно.
        bool Hidden = false;
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
    // Ввод ТЕКСТА — отдельный поток событий, а не опрос клавиш. Клавиша и
    // символ это разные вещи: раскладка, Shift, мёртвые клавиши и композиция
    // (é, ё, иероглифы) превращают нажатия в символы по правилам системы, и
    // повторять эти правила в движке нельзя. GLFW отдаёт уже готовый codepoint.
    using CharFn = std::function<void(unsigned int codepoint)>;
    // Клавиши РЕДАКТИРОВАНИЯ (Backspace, стрелки, Enter…): у них нет символа, а
    // автоповтор при удержании должен работать — поэтому событием, а не опросом.
    using KeyFn = std::function<void(int key, int action, int mods)>;
    // Файлы, брошенные В ОКНО из проводника системы.
    //
    // Это единственный способ узнать о таком перетаскивании: оконная система
    // сообщает о нём событием, опросить его нельзя. Внутреннее перетаскивание
    // (мышью между панелями) к этому отношения не имеет — им занимается ImGui,
    // который про существование файлов вне окна ничего не знает.
    //
    // Приходит СПИСОК: бросают обычно выделенную пачку, а не один файл.
    using FileDropFn = std::function<void(const std::vector<std::string>& paths)>;
    void SetCursorPosCallback(CursorPosFn fn) { m_cursorPosFn = std::move(fn); }
    void SetScrollCallback(ScrollFn fn) { m_scrollFn = std::move(fn); }
    void SetCharCallback(CharFn fn) { m_charFn = std::move(fn); }
    void SetKeyCallback(KeyFn fn) { m_keyFn = std::move(fn); }
    void SetFileDropCallback(FileDropFn fn) { m_fileDropFn = std::move(fn); }

    // Захват курсора: мышь прячется и «прилипает» к окну, продолжая отдавать
    // смещение — режим обзора от первого лица. Без этого игра от первого лица
    // невозможна в принципе: курсор упирается в край экрана и обзор встаёт.
    // Отпускание (false) возвращает обычный курсор — меню, пауза, alt-tab.
    void SetCursorCaptured(bool captured);
    bool CursorCaptured() const { return m_cursorCaptured; }

private:
    static void ForwardCursorPos(GLFWwindow* handle, double x, double y);
    static void ForwardScroll(GLFWwindow* handle, double xoffset, double yoffset);
    static void ForwardChar(GLFWwindow* handle, unsigned int codepoint);
    static void ForwardKey(GLFWwindow* handle, int key, int scancode, int action, int mods);
    static void ForwardFileDrop(GLFWwindow* handle, int count, const char** paths);

    GLFWwindow* m_handle = nullptr;
    int m_width;
    int m_height;
    CursorPosFn m_cursorPosFn;
    ScrollFn m_scrollFn;
    CharFn m_charFn;
    KeyFn m_keyFn;
    FileDropFn m_fileDropFn;
    bool m_cursorCaptured = false;
};
