#include "HotkeyScope.h"

namespace sage::editor::hotkeys {

namespace {
// Статикой, а не полем EditorLayer, потому что заявку подаёт панель, а у
// панели на руках только EditorHost — тащить ради двух флагов ещё один
// виртуальный метод в интерфейс хоста значит платить за флаг ценой интерфейса.
constexpr int kCount = (int)Key::Count;
bool g_now[kCount] = {};
bool g_prev[kCount] = {};
}

void NewFrame() {
    for (int i = 0; i < kCount; ++i) {
        g_prev[i] = g_now[i];
        g_now[i] = false;
    }
}

void Claim(Key key) { g_now[(int)key] = true; }

bool Claimed(Key key) { return g_now[(int)key] || g_prev[(int)key]; }

} // namespace sage::editor::hotkeys
