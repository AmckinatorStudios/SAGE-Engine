// ---------------------------------------------------------------------------
// ИСТОЧНИК КАРТИНКИ: файл проекта или Render Texture.
//
// Render Texture — не отдельный элемент, а источник Image: та же картинка с
// теми же режимами (вписать, растянуть, девятина), только пиксели рисует
// камера сцены («rt:<имя>», sage/render/RenderTexture.h). Здесь — выбор
// источника и имени; путь файла по-прежнему выбирается слотом.
// ---------------------------------------------------------------------------
#include "UIElementProperties.h"

#include <algorithm>
#include <set>

#include "imgui.h"

#include "EditorHost.h"
#include "EditorTheme.h"
#include "Localization.h"

#include "sage/render/RenderTexture.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"

namespace sage::editor {

namespace {
constexpr const char* kRtPrefix = "rt:";
bool IsRenderTexture(const std::string& path) { return path.rfind(kRtPrefix, 0) == 0; }
} // namespace

bool DrawImageSource(EditorHost& host, const char* id, std::string& path, bool& changed) {
    ImGui::PushID(id);
    const char* kSources[] = {T("File"), T("Render Texture")};
    int source = IsRenderTexture(path) ? 1 : 0;
    if (ImGui::Combo(T("Source"), &source, kSources, 2)) {
        host.PushUndoSnapshot();
        path = source == 1 ? std::string(kRtPrefix) : std::string();
        changed = true;
    }
    if (source == 1) {
        // Имена — из камер уровня (их Render Texture) и уже заведённых
        // картинок: те, что завёл скрипт, видны после первого запуска.
        std::set<std::string> names;
        for (const std::string& n : sage::render::RenderTextureRegistry::Instance().Names()) names.insert(n);
        Scene& level = host.LevelScene();
        for (auto e : level.Registry().view<RenderTextureComponent>()) {
            const std::string& n = level.Registry().get<RenderTextureComponent>(e).Target;
            if (!n.empty()) names.insert(n);
        }
        const std::string current = path.substr(3);
        if (ImGui::BeginCombo(T("Render Texture"), current.empty() ? T("(choose)") : current.c_str())) {
            for (const std::string& n : names) {
                if (ImGui::Selectable(n.c_str(), n == current)) {
                    host.PushUndoSnapshot();
                    path = kRtPrefix + n;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        if (names.empty())
            EditorTheme::Hint(T("No Render Texture yet: add the component to a camera object in the scene"));
    }
    ImGui::PopID();
    return source == 1;
}

} // namespace sage::editor
