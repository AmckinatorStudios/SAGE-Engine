#include "PanelWindows.h"

#include <algorithm>
#include <set>
#include <string>

#include "imgui_internal.h"

namespace panelwindows {
namespace {

ImGuiID g_homeDock = 0;
// Окна, которые просили вернуть: Before этого окна снимает отметку и заводит
// его в док. Множество, а не флаг на всё: окна рисуются по очереди, и флаг
// пришлось бы гасить в неизвестный момент «после последнего».
std::set<std::string> g_returnRequested;

bool Floating(const ImGuiWindow* w) {
    if (!w || !w->WasActive || w->Viewport == nullptr) return false;
    return w->Viewport->ID != ImGui::GetMainViewport()->ID || w->DockId == 0;
}

// Окно в отдельном окне системы, которое не видно ни на одном мониторе: второй
// монитор отключили или рабочий стол перестроили. Человеку его не достать, а
// ImGui само его на экран не вернёт. Видимой считаем полосу заголовка — за неё
// окно можно взять; пол-окна за краем не повод его трогать.
bool LostOffDesktop(const ImGuiWindow* w) {
    if (!w || w->Viewport == nullptr || w->Viewport->ID == ImGui::GetMainViewport()->ID) return false;
    const ImVector<ImGuiPlatformMonitor>& monitors = ImGui::GetPlatformIO().Monitors;
    if (monitors.Size == 0) return false;   // бэкенд монитор не сообщил — не гадаем
    const ImGuiViewport* vp = w->Viewport;
    const ImRect strip(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + 24.0f));
    for (const ImGuiPlatformMonitor& m : monitors) {
        const ImVec2 pos = m.WorkSize.x > 0.0f ? m.WorkPos : m.MainPos;
        const ImVec2 size = m.WorkSize.x > 0.0f ? m.WorkSize : m.MainSize;
        const ImRect desk(pos, ImVec2(pos.x + size.x, pos.y + size.y));
        const ImRect hit(ImVec2(std::max(strip.Min.x, desk.Min.x), std::max(strip.Min.y, desk.Min.y)),
                         ImVec2(std::min(strip.Max.x, desk.Max.x), std::min(strip.Max.y, desk.Max.y)));
        if (hit.GetWidth() >= 48.0f && hit.GetHeight() >= 12.0f) return false;
    }
    return true;
}

} // namespace

void SetHomeDock(ImGuiID dockspace) { g_homeDock = dockspace; }

bool IsFloating(const char* id) { return Floating(ImGui::FindWindowByName(id)); }

void RequestReturn(const char* id) { g_returnRequested.insert(id); }

void Before(const char* id) {
    // Класс ставится КАЖДЫЙ кадр, а не при отрыве: ImGui решает судьбу вьюпорта
    // заново в каждом кадре, и пропущенный флаг слил бы окно с главным при
    // первом же наезде на него.
    ImGuiWindowClass cls;
    cls.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
    ImGui::SetNextWindowClass(&cls);

    const bool requested = g_returnRequested.erase(id) > 0;
    if (g_homeDock == 0) return;
    // Пока окно тащат мышью, оно временно живёт отдельным вьюпортом, и это ещё
    // не потеря: человек как раз выбирает, куда его поставить.
    if (ImGui::GetCurrentContext()->MovingWindow != nullptr) return;

    const ImGuiWindow* w = ImGui::FindWindowByName(id);
    if (!w || !w->WasActive) return;
    if ((requested && Floating(w)) || LostOffDesktop(w))
        ImGui::SetNextWindowDockID(g_homeDock, ImGuiCond_Always);
}

} // namespace panelwindows
