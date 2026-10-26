#include "TestFramework.h"

#include <GLFW/glfw3.h>

#include "sage/core/Display.h"
#include "sage/core/Problems.h"

// «GLFW 65544: Win32: Failed to query monitor DPI / display settings» — неответ
// монитора (выключенный, виртуальный, отключённый в момент опроса). Он не
// поломка: монитор пропускается, масштаб и режим берутся запасные. Здесь — что
// такая ошибка узнаётся, пишется один раз предупреждением с объяснением, а не
// красной ошибкой на каждый опрос.

TEST(Display_monitor_query_errors_are_recognised) {
    using sage::display::IsMonitorQueryError;
    CHECK_TRUE(IsMonitorQueryError(GLFW_PLATFORM_ERROR, "Win32: Failed to query monitor DPI"));
    CHECK_TRUE(IsMonitorQueryError(GLFW_PLATFORM_ERROR, "Win32: Failed to query display settings"));
    // Настоящие отказы — по-прежнему ошибки.
    CHECK_FALSE(IsMonitorQueryError(GLFW_PLATFORM_ERROR, "WGL: Failed to make context current"));
    CHECK_FALSE(IsMonitorQueryError(GLFW_API_UNAVAILABLE, "Win32: Failed to query monitor DPI"));
    CHECK_FALSE(IsMonitorQueryError(GLFW_PLATFORM_ERROR, nullptr));
}

TEST(Display_monitor_query_error_is_one_warning_with_an_explanation) {
    sage::Problems::Clear();
    const int before = sage::display::MonitorQueryErrorCount();
    for (int i = 0; i < 5; ++i) sage::display::NoteMonitorQueryError("Win32: Failed to query monitor DPI");
    sage::display::NoteMonitorQueryError("Win32: Failed to query display settings");
    CHECK_EQ(sage::display::MonitorQueryErrorCount() - before, 6);
    int dpi = 0, mode = 0, errors = 0;
    for (const sage::Problem& p : sage::Problems::Snapshot()) {
        if (p.Level >= sage::Severity::Error) ++errors;
        if (p.Code == "display.dpi") {
            ++dpi;
            CHECK_FALSE(p.Detail.empty());
            CHECK_FALSE(p.Hints.empty());
        }
        if (p.Code == "display.mode") ++mode;
    }
    CHECK_EQ(dpi, 1);
    CHECK_EQ(mode, 1);
    CHECK_EQ(errors, 0);
    sage::Problems::Clear();
}

TEST(Display_aspect_names_follow_the_market_not_the_fraction) {
    using sage::display::AspectName;
    CHECK_EQ(AspectName(1920, 1080), std::string("16:9"));
    CHECK_EQ(AspectName(1366, 768), std::string("16:9"));
    CHECK_EQ(AspectName(1920, 1200), std::string("16:10"));
    CHECK_EQ(AspectName(2560, 1080), std::string("21:9"));
    CHECK_EQ(AspectName(3440, 1440), std::string("21:9"));
    CHECK_EQ(AspectName(1024, 768), std::string("4:3"));
    CHECK_EQ(AspectName(1280, 1024), std::string("5:4"));
    CHECK_EQ(AspectName(0, 0), std::string("?"));
}
