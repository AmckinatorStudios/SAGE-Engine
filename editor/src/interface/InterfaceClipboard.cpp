#include "InterfaceClipboard.h"

#include "imgui.h"

#include "sage/core/Log.h"
#include "sage/scene/Scene.h"
#include "sage/ui/Interface.h"

namespace sage::editor::uiclipboard {
namespace {

// Признак «в буфере наш интерфейс». Ключ формата ресурса — он там есть
// всегда, а разбирать чужой текст ради ответа «можно ли вставить» незачем:
// вопрос задаётся каждый кадр, пока открыто меню.
constexpr const char* kMark = "\"sage_interface_version\"";

} // namespace

int Copy(const Scene& scene, const std::vector<entt::entity>& roots) {
    const sage::ui::Interface ui = sage::ui::Capture(scene, roots);
    if (ui.Roots.empty()) return 0;
    ImGui::SetClipboardText(ui.ToJsonString().c_str());
    return (int)ui.Roots.size();
}

bool HasInterface() {
    const char* text = ImGui::GetClipboardText();
    return text && std::string(text).find(kMark) != std::string::npos;
}

std::vector<entt::entity> Paste(Scene& scene, entt::entity parent) {
    const char* text = ImGui::GetClipboardText();
    if (!text) return {};
    sage::ui::Interface ui;
    std::string err;
    if (!sage::ui::Interface::FromJsonString(text, ui, err)) {
        // В буфере не интерфейс — это НОРМА, а не ошибка: там мог лежать путь,
        // кусок кода или что угодно ещё. Молчим, иначе Ctrl+V в дереве писал бы
        // в консоль при каждом промахе.
        return {};
    }
    return sage::ui::Instantiate(scene, ui, parent);
}

} // namespace sage::editor::uiclipboard
