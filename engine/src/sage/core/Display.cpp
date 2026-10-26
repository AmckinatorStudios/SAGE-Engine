#include "sage/core/Display.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <numeric>
#include <set>

#include <GLFW/glfw3.h>

#include "sage/core/Log.h"
#include "sage/core/Problems.h"

namespace sage::display {

namespace {
std::mutex g_mutex;
std::set<std::string> g_seen;
int g_count = 0;
} // namespace

bool IsMonitorQueryError(int code, const char* description) {
    if (code != GLFW_PLATFORM_ERROR || !description) return false;
    const std::string d = description;
    return d.find("Failed to query monitor DPI") != std::string::npos ||
           d.find("Failed to query display settings") != std::string::npos ||
           d.find("Failed to query monitor") != std::string::npos;
}

void NoteMonitorQueryError(const char* description) {
    const std::string d = description ? description : "";
    bool first = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ++g_count;
        first = g_seen.insert(d).second;
    }
    if (!first) return;
    const bool dpi = d.find("DPI") != std::string::npos;
    // Одна запись в «Проблемах» (она же строка журнала) — без отдельного
    // LOG_WARN: тот сам стал бы второй проблемой (Log.cpp).
    sage::Problem p;
    p.Level = sage::Severity::Warning;
    p.Category = "Display";
    p.Code = dpi ? "display.dpi" : "display.mode";
    p.Title = dpi ? "Монитор не сообщил масштаб (DPI)" : "Монитор не сообщил режим экрана";
    p.Detail = "Так отвечают выключенные или спящие экраны, виртуальные дисплеи удалённого доступа и захвата, "
               "а также монитор, отключённый в момент опроса. Программа работает: такой монитор пропущен, "
               "масштаб интерфейса взят у окна (или 100%), режим — у главного монитора.";
    p.Hints = {"Включите монитор или отключите неиспользуемый виртуальный дисплей — сообщение уйдёт",
               "Если окно редактора на этом мониторе выглядит мелким или крупным — поправьте «Масштаб интерфейса»"};
    p.Technical = "GLFW 65544: " + d;
    p.Resource = "display";
    sage::Problems::Report(std::move(p));
}

int MonitorQueryErrorCount() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_count;
}

std::vector<Monitor> QueryMonitors() {
    std::vector<Monitor> out;
    int count = 0;
    GLFWmonitor** list = glfwGetMonitors(&count);
    GLFWmonitor* primary = glfwGetPrimaryMonitor();
    for (int i = 0; i < count && list; ++i) {
        Monitor m;
        m.Index = i;
        m.Handle = list[i];
        m.Primary = list[i] == primary;
        const char* name = glfwGetMonitorName(list[i]);
        m.Name = name && *name ? name : ("Monitor " + std::to_string(i + 1));
        glfwGetMonitorPos(list[i], &m.X, &m.Y);
        if (const GLFWvidmode* v = glfwGetVideoMode(list[i]); v && v->width > 0 && v->height > 0) {
            m.Current = {v->width, v->height, v->refreshRate};
            m.ModeKnown = true;
        }
        int n = 0;
        if (const GLFWvidmode* modes = m.ModeKnown ? glfwGetVideoModes(list[i], &n) : nullptr) {
            for (int k = 0; k < n; ++k) {
                const VideoMode vm{modes[k].width, modes[k].height, modes[k].refreshRate};
                if (vm.Width > 0 && vm.Height > 0 && std::find(m.Modes.begin(), m.Modes.end(), vm) == m.Modes.end())
                    m.Modes.push_back(vm);
            }
            std::sort(m.Modes.begin(), m.Modes.end(), [](const VideoMode& a, const VideoMode& b) {
                if (a.Width != b.Width) return a.Width > b.Width;
                if (a.Height != b.Height) return a.Height > b.Height;
                return a.RefreshHz > b.RefreshHz;
            });
        }
        float sx = 0.0f, sy = 0.0f;
        glfwGetMonitorContentScale(list[i], &sx, &sy);
        // Ноль — монитор не ответил (или виртуальный с «DPI 0»): 100%.
        if (sx > 0.01f) {
            m.Scale = sx;
            m.ScaleKnown = true;
        }
        out.push_back(std::move(m));
    }
    return out;
}

int MonitorOfWindow(GLFWwindow* window, const std::vector<Monitor>& monitors) {
    if (!window || monitors.empty()) return -1;
    if (GLFWmonitor* full = glfwGetWindowMonitor(window)) {
        for (const Monitor& m : monitors)
            if (m.Handle == full) return m.Index;
    }
    int wx = 0, wy = 0, ww = 0, wh = 0;
    glfwGetWindowPos(window, &wx, &wy);
    glfwGetWindowSize(window, &ww, &wh);
    int best = -1;
    long long bestArea = -1;
    for (const Monitor& m : monitors) {
        if (!m.ModeKnown) continue;
        const int x0 = std::max(wx, m.X), y0 = std::max(wy, m.Y);
        const int x1 = std::min(wx + ww, m.X + m.Current.Width), y1 = std::min(wy + wh, m.Y + m.Current.Height);
        const long long area = (long long)std::max(0, x1 - x0) * std::max(0, y1 - y0);
        if (area > bestArea) {
            bestArea = area;
            best = m.Index;
        }
    }
    if (best < 0)
        for (const Monitor& m : monitors)
            if (m.Primary) best = m.Index;
    return best;
}

std::string AspectName(int width, int height) {
    if (width <= 0 || height <= 0) return "?";
    const float r = (float)width / (float)height;
    // Маркетинговые имена — по близости: 2560×1080 — это «21:9», хотя
    // точная дробь 64:27, а 1366×768 — «16:9», хотя 683:384.
    struct Named { float Ratio; const char* Name; };
    static const Named names[] = {{4.0f / 3.0f, "4:3"}, {5.0f / 4.0f, "5:4"}, {16.0f / 10.0f, "16:10"},
                                  {16.0f / 9.0f, "16:9"}, {21.0f / 9.0f, "21:9"}, {32.0f / 9.0f, "32:9"},
                                  {3.0f / 2.0f, "3:2"}};
    for (const Named& n : names)
        if (std::abs(r - n.Ratio) < 0.06f) return n.Name;
    const int g = std::gcd(width, height);
    return std::to_string(width / g) + ":" + std::to_string(height / g);
}

} // namespace sage::display
