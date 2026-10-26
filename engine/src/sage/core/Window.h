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
        // Окно, РАЗВЁРНУТОЕ на весь экран (кнопкой «развернуть», а не
        // полноэкранным режимом). Инструменту это нужно по умолчанию: редактор
        // с десятком панелей в окне 1600x900 показывает их огрызками, и первое,
        // что делает человек, — тянет окно за угол. Полноэкранный и безрамочный
        // режимы для этого не годятся: они забирают монитор целиком и прячут
        // панель задач, а работают в редакторе вместе с браузером и папкой.
        bool Maximized = false;
    };

    Window(int width, int height, const std::string& title)
        : Window(width, height, title, Params{}) {}
    Window(int width, int height, const std::string& title, Params params);
    ~Window();

    bool ShouldClose() const;
    // ОТМЕНИТЬ ЗАКРЫТИЕ, о котором уже попросила система (крестик окна, Alt+F4).
    //
    // Без этого «спросить перед выходом» невозможно в принципе: GLFW ставит
    // флаг закрытия сам, и приложение обязано либо выйти, либо снять флаг.
    // Редактор снимает его, показывает вопрос о несохранённой сцене и закрывает
    // окно уже своим решением.
    void CancelClose();
    void SwapBuffers();
    void PollEvents();
    // То же, что PollEvents, но засыпает до первого события или до timeout
    // секунд: в свёрнутом окне или при досыпе до лимита кадров процессор
    // отдыхает, а ввод будит цикл сразу. Смещение курсора за кадр копится
    // дальше, а не начинается заново.
    void WaitEvents(double timeoutSeconds);

    // Свёрнуто (или рисовать некуда — буфер нулевого размера). Рисовать в такое
    // окно — впустую жечь видеокарту: кадра всё равно никто не увидит.
    bool Iconified() const { return m_iconified || m_width <= 0 || m_height <= 0; }
    // В фокусе ли окно. Из события, а не вопросом системе каждый кадр.
    bool Focused() const { return m_focused; }
    // Секунд с последнего события ввода (клавиша, мышь, колесо, символ) —
    // любого окна приложения.
    double SecondsSinceInput() const;

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
    // Кнопки мыши — тем же событием и по той же причине, что клавиши: щелчок
    // короче кадра существует, а опрос его не видит (см. AddKeyCallback).
    using MouseButtonFn = std::function<void(int button, int action, int mods)>;
    // Курсор ВОШЁЛ в окно или ПОКИНУЛ его. Опросить это нельзя — только
    // событием, а знать надо: пока курсор снаружи, «мышь над кнопкой» ложь, и
    // подсветка остаётся висеть на той кнопке, над которой курсор вышел за
    // край окна.
    using CursorEnterFn = std::function<void(bool entered)>;

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
    // Слушателей клавиш НЕСКОЛЬКО, и это не удобство, а необходимость: их
    // двое сразу — ввод игры (защёлкивает короткие нажатия, см.
    // InputSystem::Attach) и поле ввода интерфейса (Backspace со
    // автоповтором). Пока слот был один, второй подписчик молча отменял
    // первого, и что именно сломается, зависело от порядка инициализации.
    void AddKeyCallback(KeyFn fn) { m_keyFns.push_back(std::move(fn)); }
    void AddMouseButtonCallback(MouseButtonFn fn) { m_mouseButtonFns.push_back(std::move(fn)); }
    void SetFileDropCallback(FileDropFn fn) { m_fileDropFn = std::move(fn); }
    void SetCursorEnterCallback(CursorEnterFn fn) { m_cursorEnterFn = std::move(fn); }

    // ЗАХВАТ КУРСОРА — штатный механизм ОС, а не возврат курсора в центр.
    //
    // GLFW_CURSOR_DISABLED — это то, что система даёт играм сама: Windows
    // прячет курсор и держит его ClipCursor в окне, macOS отвязывает указатель
    // от мыши (CGAssociateMouseAndMouseCursorPosition), X11 захватывает
    // указатель (XGrabPointer). Смещение при этом приходит без границ, а с
    // сырым вводом (GLFW_RAW_MOUSE_MOTION) — ещё и без ускорения рабочего
    // стола. Потерю фокуса система обрабатывает сама: курсор отпускается, пока
    // окно не в фокусе, и захватывается снова, когда фокус вернулся.
    //
    // Прежний «программный» запасной режим (курсор спрятан, движок возвращает
    // его в центр, когда тот отъедет) убран: в нём невидимый курсор гулял по
    // окну, а обзор упирался в край. Случаи, ради которых он заводился (режим
    // курсора сбросил кто-то сторонний), закрывает сверка режима раз в кадр.
    void SetCursorCaptured(bool captured);
    bool CursorCaptured() const { return m_cursorCaptured; }

    // Фокус окна. Подписчики, считающие смещение мыши по разнице положений
    // (GlfwBridge), начинают отсчёт заново: смещение «до» и «после» alt-tab —
    // не движение мыши в игре, а скачок обзора.
    // Масштаб системы для этого окна (Windows: 1.25 при 125%). Не 0 никогда:
    // не ответивший монитор даёт масштаб главного или 1.0 (sage/core/Display.h).
    float ContentScale() const {
#if defined(__APPLE__)
        return 1.0f;
#else
        return m_contentScale;
#endif
    }
    using ScaleFn = std::function<void(float scale)>;
    void AddContentScaleCallback(ScaleFn fn) { m_scaleFns.push_back(std::move(fn)); }

    using FocusFn = std::function<void(bool focused)>;
    void AddFocusCallback(FocusFn fn) { m_focusFns.push_back(std::move(fn)); }

    // ОКНО, КОТОРОЕ ЗАХВАТЫВАЕТ КУРСОР. По умолчанию — своё (nullptr). Редактору
    // нужно другое: панель Game или вьюпорт, вытащенные отдельным окном
    // системы, живут в чужом GLFW-окне, и захват своего окна ничего не давал —
    // мышь гуляла по окну панели. Окно должно быть живым; уничтожаемое
    // сообщается через ForgetWindow ДО уничтожения.
    void SetCursorCaptureTarget(GLFWwindow* target);
    GLFWwindow* CursorCaptureTarget() const { return m_captureTarget ? m_captureTarget : m_handle; }
    // Окно системы вот-вот уничтожат (закрылся вьюпорт интерфейса): забыть его.
    void ForgetWindow(GLFWwindow* handle);

    // СМЕЩЕНИЕ КУРСОРА ЗА КАДР (с последнего PollEvents), в пикселях окна, Y вниз.
    // Пока курсор захвачен, это единственное честное смещение: положение мыши
    // интерфейса редактора в захвате держится на месте (см.
    // EditorLayer::PinMouseWhileCaptured), и его разница — ноль.
    double FrameCursorDeltaX() const { return m_frameDx; }
    double FrameCursorDeltaY() const { return m_frameDy; }

    // События, пришедшие от ДРУГИХ окон системы этого приложения (окна
    // панелей редактора), доставляются подписчикам этого окна, как свои.
    // Поэтому они публичны: их зовёт и статический переходник GLFW, и проверка.
    void EmitCursorPos(double x, double y);
    void EmitKey(int key, int action, int mods);
    void EmitFocus(bool focused);

private:
    void ApplyCursorMode();
    void ApplyCursorModeTo(GLFWwindow* target, bool captured);
    static Window* Owner(GLFWwindow* handle);
    static void ForwardCursorPos(GLFWwindow* handle, double x, double y);
    static void ForwardScroll(GLFWwindow* handle, double xoffset, double yoffset);
    static void ForwardChar(GLFWwindow* handle, unsigned int codepoint);
    static void ForwardKey(GLFWwindow* handle, int key, int scancode, int action, int mods);
    static void ForwardMouseButton(GLFWwindow* handle, int button, int action, int mods);
    static void ForwardFileDrop(GLFWwindow* handle, int count, const char** paths);
    static void ForwardCursorEnter(GLFWwindow* handle, int entered);

    GLFWwindow* m_handle = nullptr;
    int m_width;
    int m_height;
    CursorPosFn m_cursorPosFn;
    ScrollFn m_scrollFn;
    CharFn m_charFn;
    std::vector<KeyFn> m_keyFns;
    std::vector<MouseButtonFn> m_mouseButtonFns;
    FileDropFn m_fileDropFn;
    CursorEnterFn m_cursorEnterFn;
    bool m_cursorCaptured = false;
    GLFWwindow* m_captureTarget = nullptr;
    std::vector<FocusFn> m_focusFns;
    // Смещение за кадр: последнее положение и накопленное с PollEvents.
    double m_lastX = 0.0, m_lastY = 0.0;
    bool m_haveLast = false;
    double m_frameDx = 0.0, m_frameDy = 0.0;
    bool m_iconified = false;
    float m_contentScale = 1.0f;
    std::vector<ScaleFn> m_scaleFns;
    float QueryContentScale() const;
    bool m_focused = true;
    double m_lastInput = 0.0;     // glfwGetTime() последнего события ввода
    void TouchInput();
    void AfterEvents();
};
