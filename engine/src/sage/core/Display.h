#pragma once
#include <string>
#include <vector>

#include "sage/core/Config.h"

struct GLFWmonitor;
struct GLFWwindow;
class Window;

// ---------------------------------------------------------------------------
// ДИСПЛЕИ: мониторы, их режимы и масштаб — с запасом на мониторы, которые не
// отвечают.
//
// В Windows GLFW спрашивает у каждого подключённого монитора масштаб
// (GetDpiForMonitor) и текущий режим (EnumDisplaySettings). Монитор, который
// не может ответить, — выключенный экран, телевизор в режиме ожидания,
// виртуальный дисплей удалённого доступа или захвата, монитор, отключённый
// прямо в момент опроса, — даёт «GLFW 65544: Win32: Failed to query monitor
// DPI» / «… display settings». Это не поломка программы: такой монитор
// пропускается, масштаб берётся у окна (или 100%), режим — у главного
// монитора. Здесь это собрано в одном месте: и список мониторов для настроек,
// и масштаб интерфейса читают его, а не GLFW напрямую.
// ---------------------------------------------------------------------------
namespace sage::display {

struct VideoMode {
    int Width = 0;
    int Height = 0;
    int RefreshHz = 0;
    bool operator==(const VideoMode& o) const {
        return Width == o.Width && Height == o.Height && RefreshHz == o.RefreshHz;
    }
};

struct Monitor {
    int Index = 0;
    std::string Name;
    bool Primary = false;
    int X = 0, Y = 0;               // левый верхний угол в координатах рабочего стола
    VideoMode Current;              // текущий режим (если ModeKnown)
    bool ModeKnown = false;         // монитор ответил о своём режиме
    std::vector<VideoMode> Modes;   // поддерживаемые режимы, без повторов, по убыванию
    float Scale = 1.0f;             // масштаб Windows (1.25 — 125%)
    bool ScaleKnown = false;        // монитор ответил о масштабе
    GLFWmonitor* Handle = nullptr;  // действителен до смены набора мониторов
};

// Все мониторы. Не ответившие о режиме остаются в списке (их можно назвать),
// но выбрать их режим нельзя — ModeKnown = false.
std::vector<Monitor> QueryMonitors();
// Номер монитора, на котором больше всего окна (-1 — не нашёлся).
int MonitorOfWindow(GLFWwindow* window, const std::vector<Monitor>& monitors);

// Ошибка GLFW — неответ монитора на запрос режима или масштаба? Такие не
// фатальны: вызывающий уже взял запасное значение.
bool IsMonitorQueryError(int code, const char* description);
// Запомнить такую ошибку: первое появление каждого текста — предупреждение в
// журнале и запись в «Проблемы» с объяснением, повторы только считаются.
void NoteMonitorQueryError(const char* description);
int MonitorQueryErrorCount();

// Соотношение сторон текстом: 1920×1080 -> «16:9», 2560×1080 -> «21:9».
std::string AspectName(int width, int height);

// РЕЖИМ ЭКРАНА НА ПРОБУ — как в любой игре: новый режим ставится сразу,
// человека спрашивают «оставить?», и если он не ответил (экран погас, картинка
// уехала за край), через несколько секунд всё возвращается само. Не
// получилось применить вовсе — возвращается сразу, с причиной.
struct DisplayRequest {
    sage::WindowMode Mode = sage::WindowMode::Windowed;
    int Monitor = -1;
    int Width = 1280;
    int Height = 720;
    int RefreshRate = 0;
};

class DisplayTrial {
public:
    // Применить next на seconds секунд. false — не применилось (окно как было).
    bool Begin(Window& window, const DisplayRequest& next, double nowSeconds, double seconds, std::string* err);
    bool Active() const { return m_active; }
    double SecondsLeft(double nowSeconds) const;
    void Confirm() { m_active = false; }
    void Revert(Window& window);
    // Время вышло — откат. true — откатили в этом вызове.
    bool Tick(Window& window, double nowSeconds);
    const DisplayRequest& Previous() const { return m_previous; }

private:
    bool m_active = false;
    double m_deadline = 0.0;
    DisplayRequest m_previous;
};

} // namespace sage::display
