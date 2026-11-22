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
#include "../InspectorControls.h"
#include "../EditorTheme.h"
#include "../Localization.h"
#include "../PanelWindowId.h"
#include "../Project.h"
#include "../ui/UI.h"
#include "sage/anim/AnimProperty.h"
#include "sage/anim/ClipFile.h"
#include "sage/anim/PropertyAnimator.h"
#include "sage/core/Log.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include <glm/gtc/quaternion.hpp>

namespace fs = std::filesystem;
namespace anim = sage::anim;
namespace ictl = sage::editor::ictl;

namespace animpanel_detail {

constexpr float kTrackHeight = 22.0f;
constexpr float kKeyRadius = 5.0f;
// Высота шкалы времени — не меньше строки с кнопкой «+ Дорожка» над списком:
// иначе строки списка и строки ключей разъезжались на разницу высот.
inline float RulerHeight() { return std::max(22.0f, ImGui::GetFrameHeight() + 2.0f); }

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

// Каналы кости в скелетном клипе — свойства "joint.translation/rotation/scale"
// (так их раскладывает OpenClip): кость — Target дорожки.
inline bool IsBoneChannel(const std::string& prop) { return prop.rfind("joint.", 0) == 0; }
inline const char* BoneChannelTitle(const std::string& prop) {
    if (prop == "joint.translation") return T("Position");
    if (prop == "joint.rotation") return T("Rotation");
    return T("Scale");
}
// Значение ключа канала из TRS кости: поворот — кватернион (x, y, z, w), как
// в .sageanim.
inline glm::vec4 BoneValue(const std::string& prop, const glm::vec3& t, const glm::quat& r, const glm::vec3& s) {
    if (prop == "joint.translation") return glm::vec4(t, 0.0f);
    if (prop == "joint.rotation") return glm::vec4(r.x, r.y, r.z, r.w);
    return glm::vec4(s, 0.0f);
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
