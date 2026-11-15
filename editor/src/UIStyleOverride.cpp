// ---------------------------------------------------------------------------
// ЛОКАЛЬНЫЕ ПЕРЕОПРЕДЕЛЕНИЯ СТИЛЯ в инспекторе.
//
// У элемента со стилем правка поля вида запоминается как переопределение
// (Element::StyleOverrides): правка общего стиля дальше обновляет остальное
// и это поле не трогает. Рядом с таким полем — кнопка «вернуть как в стиле».
// ---------------------------------------------------------------------------
#include "UIElementProperties.h"

#include <algorithm>

#include "imgui.h"

#include "EditorHost.h"
#include "EditorIcons.h"
#include "Localization.h"

#include "sage/scene/Scene.h"
#include "sage/ui/UI.h"
#include "sage/ui/UIPart.h"

namespace sage::editor {

void NoteStyleOverride(EditorHost& host, entt::entity e, const sage::ui::PartType& part,
                       const sage::ui::PartField& f, bool edited) {
    entt::registry& reg = host.CurrentScene().Registry();
    sage::ui::Element* el = reg.try_get<sage::ui::Element>(e);
    // Содержимое (текст, значение) стиль не задаёт вовсе — и переопределять
    // там нечего.
    if (!el || el->Style.empty() || !part.Id || !f.Key || f.Content) return;
    const std::string key = std::string(part.Id) + "." + f.Key;
    if (edited && !el->Overrides(key)) el->StyleOverrides.push_back(key);
    if (!el->Overrides(key)) return;
    ImGui::SameLine();
    ImGui::PushID(key.c_str());
    if (EditorIcons::IconOnlyButton("undo", T("Overrides the style. Click to take the value from the style again"))) {
        host.PushUndoSnapshot();
        auto& o = el->StyleOverrides;
        o.erase(std::remove(o.begin(), o.end(), key), o.end());
        el->StyleVersion = -1;   // стиль ляжет заново на ближайшем кадре
    }
    ImGui::PopID();
}

} // namespace sage::editor
