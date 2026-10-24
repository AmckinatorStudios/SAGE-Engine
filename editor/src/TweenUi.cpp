#include "TweenUi.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include "imgui.h"
#include "imgui_internal.h"   // PushMultiItemsWidths: поля осей ровно по ширине колонки
#include "EditorHost.h"
#include "EditorIcons.h"
#include "EditorTheme.h"
#include "Localization.h"
#include "ObjectSlot.h"
#include "PanelWindowId.h"
#include "PanelWindows.h"
#include "ui/ColorPicker.h"
#include "ui/UI.h"
#include "sage/anim/AnimProperty.h"
#include "sage/scene/Scene.h"
#include "panels/TweenPanelDetail.h"

namespace anim = sage::anim;

namespace tweenui {

std::vector<Choice> Choices(const entt::registry& reg, entt::entity e) {
    std::vector<Choice> out;
    if (!reg.valid(e)) return out;
    const std::vector<const anim::PropertyType*> has = anim::PropertiesFor(reg, e);
    auto present = [&](const std::string& id) -> const anim::PropertyType* {
        for (const anim::PropertyType* p : has)
            if (p->Id == id) return p;
        return nullptr;
    };
    // У элемента интерфейса положение, размер и масштаб — свои (в точках
    // экрана); координаты объекта сцены у него тоже есть, но на экране ничего
    // не двигают — в меню они были бы ловушкой.
    const bool ui = present("element.position") != nullptr;
    auto hidden = [&](const std::string& id) { return ui && id.rfind("object.", 0) == 0; };
    // Сначала частые — в порядке разделов, потом прочие под «Ещё».
    for (const char* section : kSections)
        for (const Known& k : kKnown)
            if (std::strcmp(k.Section, section) == 0 && !hidden(k.Id))
                if (const anim::PropertyType* p = present(k.Id)) out.push_back({p, k.Section, T(k.Title)});
    for (const anim::PropertyType* p : has) {
        if (FindKnown(p->Id) || Technical(p->Id) || hidden(p->Id)) continue;
        out.push_back({p, "", std::string(T(p->Group.c_str())) + " · " + T(p->Title.c_str())});
    }
    return out;
}

std::string TitleOf(const std::string& id) {
    if (id.empty()) return T("Pause step");
    if (const Known* k = FindKnown(id)) return T(k->Title);
    if (const anim::PropertyType* p = anim::FindProperty(id))
        return std::string(T(p->Group.c_str())) + " · " + T(p->Title.c_str());
    return id;
}

std::string EaseName(const anim::Ease& e) {
    for (const Preset& p : kPresets)
        if (PresetEase(p) == e) return T(p.Name);
    if (e.Shape == anim::EaseShape::Curve) return T("Custom curve");
    if (e.Shape == anim::EaseShape::Linear) return T("Steady");
    return std::string(kShapeNames[(int)e.Shape]) + " · " + ModeLabel(e.Mode);
}

glm::vec4 SuggestedEnd(const std::string& id, const glm::vec4& current) {
    // Конец, равный началу, — твин, который ничего не делает: человек ставил
    // «Позицию», жал Play, и объект стоял на месте. Поэтому сразу видимое
    // движение, а не копия нынешнего значения; править его — одно число.
    glm::vec4 v = current;
    if (id == "object.position") v.y += 1.0f;
    else if (id == "element.position") v.x += 50.0f;
    else if (id == "object.rotation") v.y += 90.0f;
    else if (id == "element.rotation") v.x += 45.0f;
    else if (Has(id, "scale") || id == "element.size") v = glm::vec4(glm::vec3(v) * 1.5f, v.w);
    else if (Has(id, "opacity") || id == "audio.volume" || id == "bar.value" || id == "range.value")
        v.x = current.x > 0.5f ? 0.0f : 1.0f;
    else if (id == "light.intensity" || id == "light.range") v.x = std::max(current.x * 2.0f, 1.0f);
    else if (id == "camera.fov") v.x = current.x > 30.0f ? current.x - 15.0f : current.x + 15.0f;
    else if (id == "audio.pitch") v.x = current.x * 1.5f;
    else if (id == "fill.rounding") v.x += 10.0f;
    return v;
}

std::string Summary(const anim::TweenClip& clip) {
    int n = 0;
    for (const anim::TweenTrack& t : clip.Tracks)
        if (!t.Property.empty()) ++n;
    if (n == 0) return T("empty");
    // Русское число: 1 свойство, 2 свойства, 5 свойств.
    const int d = n % 10, h = n % 100;
    const char* word = d == 1 && h != 11                             ? T("property")
                       : d >= 2 && d <= 4 && (h < 12 || h > 14) ? T("properties (2-4)")
                                                                   : T("properties");
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%d %s · %.2g %s", n, word, clip.Length(), T("s"));
    return buf;
}

} // namespace tweenui
