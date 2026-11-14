#include "InterfaceClipboard.h"

#include "imgui.h"

#include "sage/core/Log.h"
#include "sage/scene/Scene.h"
#include "sage/ui/Interface.h"
#include "sage/ui/UISceneSystem.h"

namespace sage::editor::uiclipboard {
namespace {

// Признак «в буфере наш интерфейс». Ключ формата ресурса — он там есть
// всегда, а разбирать чужой текст ради ответа «можно ли вставить» незачем:
// вопрос задаётся каждый кадр, пока открыто меню.
constexpr const char* kMark = "\"sage_interface_fragment\"";

} // namespace

int Copy(const Scene& scene, const std::vector<entt::entity>& roots) {
    // Главный контейнер в буфер не кладётся: вставить второй такой некуда —
    // он один на интерфейс (sage/ui/Interface.h).
    std::vector<entt::entity> picked;
    for (entt::entity e : roots)
        if (!sage::ui::IsMainContainer(const_cast<Scene&>(scene), e)) picked.push_back(e);
    const sage::ui::InterfaceFragment frag = sage::ui::CaptureFragment(scene, picked);
    if (frag.Nodes.empty()) return 0;
    ImGui::SetClipboardText(frag.ToJsonString().c_str());
    return (int)frag.Nodes.size();
}

bool HasInterface() {
    const char* text = ImGui::GetClipboardText();
    return text && std::string(text).find(kMark) != std::string::npos;
}

std::vector<entt::entity> Paste(Scene& scene, entt::entity parent) {
    const char* text = ImGui::GetClipboardText();
    if (!text) return {};
    sage::ui::InterfaceFragment frag;
    if (!sage::ui::InterfaceFragment::FromJsonString(text, frag)) {
        // В буфере не интерфейс — это НОРМА, а не ошибка: там мог лежать путь,
        // кусок кода или что угодно ещё. Молчим, иначе Ctrl+V в дереве писал бы
        // в консоль при каждом промахе.
        return {};
    }
    return sage::ui::InstantiateFragment(scene, frag, parent);
}

} // namespace sage::editor::uiclipboard
