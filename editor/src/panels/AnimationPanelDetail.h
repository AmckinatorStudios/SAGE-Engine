#pragma once
// Общее для файлов AnimationPanel*.cpp. Только для них.
#include "AnimationPanel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include "../EditorHost.h"
#include "../EditorIcons.h"
#include "../HotkeyScope.h"
#include "../EditorTheme.h"
#include "../Localization.h"
#include "../PanelWindowId.h"
#include "../PanelWindows.h"
#include "../Project.h"
#include "../ui/UI.h"
#include "sage/anim/AnimProperty.h"
#include "sage/anim/ClipFile.h"
#include "sage/anim/PropertyAnimator.h"
#include "sage/core/Log.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"

namespace fs = std::filesystem;
namespace anim = sage::anim;

namespace animpanel_detail {

constexpr float kTrackHeight = 22.0f;
constexpr float kKeyRadius = 5.0f;
constexpr float kRulerHeight = 22.0f;

// Путь к объекту ОТ ВЛАДЕЛЬЦА КЛИПА. Владелец — выбранный объект: клип
// адресует не «сущность номер сорок», а «мой ребёнок по имени Icon», иначе он
// не лёг бы ни на одну вторую кнопку.
inline std::string PathFromOwner(Scene& scene, entt::entity owner, entt::entity target) {
    if (owner == target) return {};
    entt::registry& reg = scene.Registry();
    std::string path;
    entt::entity cur = target;
    while (cur != entt::null && cur != owner) {
        const NameComponent* n = reg.try_get<NameComponent>(cur);
        if (!n) return {};
        path = path.empty() ? n->Name : (n->Name + "/" + path);
        cur = scene.ParentOf(cur);
    }
    // Цель не под владельцем — дорожку класть некуда: клип, ссылающийся вверх,
    // перестанет работать на втором экземпляре.
    return cur == owner ? path : std::string();
}

inline const char* InterpTitle(anim::Interp i) {
    switch (i) {
        case anim::Interp::Constant: return T("Constant");
        case anim::Interp::Bezier: return T("Bezier");
        default: return T("Linear");
    }
}

} // namespace animpanel_detail
using namespace animpanel_detail;
