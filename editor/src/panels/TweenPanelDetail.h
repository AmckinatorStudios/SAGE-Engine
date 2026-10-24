#pragma once
// Общее для файлов TweenPanel*.cpp. Только для них.
#include "TweenPanel.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include "imgui.h"
#include "imgui_internal.h"   // PushMultiItemsWidths: поля осей ровно по ширине колонки
#include "../EditorHost.h"
#include "../EditorIcons.h"
#include "../EditorTheme.h"
#include "../Localization.h"
#include "../ObjectSlot.h"
#include "../PanelWindowId.h"
#include "../PanelWindows.h"
#include "../ui/ColorPicker.h"
#include "../ui/UI.h"
#include "sage/anim/AnimProperty.h"
#include "sage/scene/Scene.h"

namespace anim = sage::anim;

namespace tweenpanel_detail {

// Свойства, которые оживляют чаще всего, — со своим разделом меню и понятным
// названием. У интерфейса «Цвет» есть у заливки, картинки, текста и значка:
// в одном списке четыре «Цвета» не различить, поэтому здесь они названы.
struct Known {
    const char* Id;
    const char* Section;
    const char* Title;
};
inline const Known kKnown[] = {
    {"object.position", "Transform", "Position"},
    {"object.rotation", "Transform", "Rotation"},
    {"object.scale", "Transform", "Scale"},
    {"material.color", "Rendering", "Colour"},
    {"material.opacity", "Rendering", "Opacity"},
    {"light.color", "Rendering", "Light colour"},
    {"light.intensity", "Rendering", "Light brightness"},
    {"light.range", "Rendering", "Light range"},
    {"element.position", "Interface", "Position"},
    {"element.size", "Interface", "Size"},
    {"element.rotation", "Interface", "Rotation"},
    {"element.scale", "Interface", "Scale"},
    {"element.opacity", "Interface", "Opacity"},
    {"fill.color", "Interface", "Background colour"},
    {"image.tint", "Interface", "Image colour"},
    {"label.color", "Interface", "Text colour"},
    {"label.scale", "Interface", "Text size"},
    {"icon.color", "Interface", "Icon colour"},
    {"fill.rounding", "Interface", "Corner rounding"},
    {"bar.value", "Interface", "Bar fill"},
    {"range.value", "Interface", "Slider value"},
    {"camera.fov", "Camera", "Field of view"},
    {"audio.volume", "Audio", "Volume"},
    {"audio.pitch", "Audio", "Pitch"},
};
inline const char* const kSections[] = {"Transform", "Rendering", "Interface", "Camera", "Audio"};

inline const Known* FindKnown(const std::string& id) {
    for (const Known& k : kKnown)
        if (id == k.Id) return &k;
    return nullptr;
}

// Внутренности, которые плавно менять незачем: кадр спрайта, девятина,
// размер запекания шрифта, опорное разрешение холста, границы ползунка.
// В меню их нет вовсе — это настройка, а не движение.
inline bool Technical(const std::string& id) {
    static const char* const kSkip[] = {"sprite", "slice", "pixelScale", "fontPixelHeight", "canvas.",
                                        "scroll.speed", "range.min", "range.max", "range.step", "caret",
                                        "placeholder", "Brightness", "disabledAlpha", "pressedOffset",
                                        "smoothing", "repeat"};
    for (const char* s : kSkip)
        if (id.find(s) != std::string::npos) return true;
    return false;
}

// Готовые кривые — плитками. Имена говорят, КАК ДВИЖЕТСЯ, а не как
// называется формула: «Cubic Out» ищут в справочнике, «Плавное замедление»
// понятно сразу. Формулы целиком — в «Профессиональных».
struct Preset {
    const char* Code;
    const char* Name;
};
inline const Preset kPresets[] = {
    {"linear", "Steady"},          {"quad-out", "Smooth stop"},     {"quad-in", "Smooth start"},
    {"quad-inout", "Smooth both ends"},      {"sine-inout", "Gentle"},        {"cubic-out", "Soft landing"},
    {"expo-out", "Fast"},          {"expo-in", "Sharp start"},      {"back-out", "Overshoot"},
    {"back-in", "Wind-up"},        {"elastic-out", "Springy"},      {"bounce-out", "Bouncy"},
};

inline anim::Ease PresetEase(const Preset& p) {
    anim::Ease e;
    anim::Parse(p.Code, e);
    return e;
}

// Имена форм в «Профессиональных» не переводятся: это общий язык движков,
// по нему ищут примеры.
inline const char* const kShapeNames[] = {"Linear", "Quad", "Cubic", "Quart", "Quint", "Sine",
                                   "Expo",   "Circ", "Back",  "Elastic", "Bounce"};

inline const char* ModeLabel(anim::EaseMode m) {
    switch (m) {
        case anim::EaseMode::In: return T("Ease in");
        case anim::EaseMode::Out: return T("Ease out");
        default: return T("Ease in-out");
    }
}

inline bool IsColor(const anim::PropertyType* p) {
    if (!p || p->Components < 3) return false;
    const std::string& id = p->Id;
    return id.find("color") != std::string::npos || id.find("colour") != std::string::npos ||
           id.find("tint") != std::string::npos;
}

// График кривой в прямоугольнике [a, b]; y с запасом на перелёт (Back,
// Elastic): 0 и 1 — на margin от краёв.
inline void PlotEase(ImDrawList* dl, ImVec2 a, ImVec2 b, const anim::Ease& e, ImU32 color, float thickness,
              float margin = 0.0f) {
    ImVec2 pts[41];
    const float w = b.x - a.x;
    const float y0 = b.y - margin, y1 = a.y + margin;
    for (int i = 0; i <= 40; ++i) {
        const float t = i / 40.0f;
        const float v = anim::Evaluate(e, t);
        pts[i] = ImVec2(a.x + t * w, y0 + (y1 - y0) * v);
    }
    dl->PushClipRect(a, b, true);
    dl->AddPolyline(pts, 41, color, ImDrawFlags_None, thickness);
    dl->PopClipRect();
}

inline ImU32 Col(EditorTheme::Role r) { return ImGui::GetColorU32(EditorTheme::Color(r)); }

inline bool Has(const std::string& id, const char* part) { return id.find(part) != std::string::npos; }

// Значок свойства — тот же, что у этого действия в остальном редакторе
// (стрелки — сдвиг, круг — поворот): карточку находят по форме, до чтения.
inline const char* PropIcon(const std::string& id) {
    if (id.empty()) return "clock";
    if (Has(id, "position")) return "move";
    if (Has(id, "rotation")) return "rotate";
    if (Has(id, "scale") || Has(id, "size")) return "scale";
    if (Has(id, "color") || Has(id, "tint")) return "color";
    if (Has(id, "opacity")) return "eye";
    if (id.rfind("light.", 0) == 0) return "light";
    if (id.rfind("camera.", 0) == 0) return "camera";
    if (id.rfind("audio.", 0) == 0) return "audio";
    if (id.rfind("bar.", 0) == 0) return "ui-bar";
    if (id.rfind("range.", 0) == 0) return "ui-slider";
    if (id.rfind("fill.", 0) == 0) return "ui-panel";
    return "anim";
}

// В чём меряется значение. Без этого «1» в поле — загадка: метр? градус?
// процент? Пусто — единиц у свойства нет (цвет).
inline const char* PropUnit(const std::string& id) {
    if (id == "object.position" || id == "light.range") return T("metres");
    if (id.rfind("element.", 0) == 0 && (Has(id, "position") || Has(id, "size"))) return T("screen points");
    if (Has(id, "rotation") || id == "camera.fov") return T("degrees");
    if (Has(id, "scale")) return T("times original size");
    if (Has(id, "opacity") || id == "audio.volume") return T("0 = none, 1 = full");
    if (id == "bar.value") return T("0 = empty, 1 = full");
    if (id == "audio.pitch") return T("1 = normal");
    return "";
}

} // namespace tweenpanel_detail
using namespace tweenpanel_detail;
