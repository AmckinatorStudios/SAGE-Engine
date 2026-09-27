#include "sage/scripting/ScriptTween.h"

#include <cctype>

#include "sage/ui/Element.h"

namespace sage::scripting::tween {

namespace {

// Порядок прежнего перечисления sage::Easing: числа в сценах и скриптах
// старых игр означают именно эти кривые.
const char* const kLegacyOrder[] = {
    "linear",   "quad-in",   "quad-out",   "quad-inout", "cubic-in",  "cubic-out", "cubic-inout",
    "sine-in",  "sine-out",  "sine-inout", "expo-out",   "back-out",  "elastic-out", "bounce-out",
};

std::string Normalize(const std::string& name) {
    std::string out;
    for (char c : name) {
        if (c == '_' || c == '-' || c == ' ') continue;
        out += (char)std::tolower((unsigned char)c);
    }
    return out;
}

bool Has(const entt::registry& reg, entt::entity e, const char* id) {
    const anim::PropertyType* p = anim::FindProperty(id);
    return p && anim::HasProperty(*p, reg, e);
}

} // namespace

sol::table MakeEaseTable(sol::state_view lua) {
    sol::table t = lua.create_table();
    t["Linear"] = "linear";
    // Короткие: Ease.In/Out/InOut — квадратичный разгон, самый частый случай.
    t["In"] = "quad-in";
    t["Out"] = "quad-out";
    t["InOut"] = "quad-inout";
    static const char* kShapes[] = {"Quad", "Cubic", "Quart", "Quint", "Sine", "Expo",
                                    "Circ", "Back",  "Elastic", "Bounce"};
    for (const char* shape : kShapes) {
        std::string lower = shape;
        for (char& c : lower) c = (char)std::tolower((unsigned char)c);
        // Оба порядка слов: Ease.OutBack (как в описании кривых) и
        // Ease.BackOut (как было в движке).
        for (const char* mode : {"In", "Out", "InOut"}) {
            std::string m = mode;
            for (char& c : m) c = (char)std::tolower((unsigned char)c);
            const std::string value = lower + "-" + m;
            t[std::string(mode) + shape] = value;
            t[std::string(shape) + mode] = value;
        }
    }
    return t;
}

anim::Ease EaseFrom(const sol::object& o, const anim::Ease& fallback) {
    if (!o.valid() || o.get_type() == sol::type::lua_nil) return fallback;
    anim::Ease e = fallback;
    if (o.get_type() == sol::type::string) {
        if (anim::Parse(o.as<std::string>(), e)) return e;
        return fallback;
    }
    if (o.get_type() == sol::type::number) {
        const int i = o.as<int>();
        if (i >= 0 && i < (int)(sizeof(kLegacyOrder) / sizeof(kLegacyOrder[0])) && anim::Parse(kLegacyOrder[i], e))
            return e;
        return fallback;
    }
    if (o.get_type() == sol::type::table) {
        sol::table t = o.as<sol::table>();
        e.Shape = anim::EaseShape::Curve;
        e.Bezier = glm::vec4(t.get_or(1, 0.25f), t.get_or(2, 0.1f), t.get_or(3, 0.25f), t.get_or(4, 1.0f));
        return e;
    }
    return fallback;
}

std::string ResolveProperty(const entt::registry& reg, entt::entity e, const std::string& nameIn) {
    if (!reg.valid(e) || nameIn.empty()) return {};
    // Полный ключ реестра — как есть.
    if (nameIn.find('.') != std::string::npos) return Has(reg, e, nameIn.c_str()) ? nameIn : std::string();
    const std::string n = Normalize(nameIn);
    auto first = [&](std::initializer_list<const char*> ids) -> std::string {
        for (const char* id : ids)
            if (Has(reg, e, id)) return id;
        return {};
    };
    const bool ui = reg.all_of<ui::Element>(e);
    if (ui) {
        if (n == "position") return first({"element.position"});
        if (n == "size") return first({"element.size"});
        if (n == "rotation") return first({"element.rotation"});
        if (n == "scale") return first({"element.scale"});
        if (n == "opacity" || n == "alpha") return first({"element.opacity"});
        if (n == "color" || n == "colour") return first({"fill.color", "image.tint", "label.color", "icon.color"});
        if (n == "textsize" || n == "fontsize") return first({"label.scale"});
        if (n == "textcolor" || n == "textcolour") return first({"label.color"});
        if (n == "cornerradius" || n == "rounding") return first({"fill.rounding", "mask.rounding"});
        if (n == "value") return first({"bar.value", "range.value"});
    }
    if (n == "position") return first({"object.position"});
    if (n == "rotation") return first({"object.rotation"});
    if (n == "scale") return first({"object.scale"});
    if (n == "color" || n == "colour") return first({"material.color", "light.color"});
    if (n == "opacity" || n == "alpha") return first({"material.opacity"});
    if (n == "intensity") return first({"light.intensity"});
    if (n == "lightcolor" || n == "lightcolour") return first({"light.color"});
    if (n == "range") return first({"light.range"});
    if (n == "fov" || n == "fieldofview") return first({"camera.fov"});
    if (n == "volume") return first({"audio.volume"});
    if (n == "pitch") return first({"audio.pitch"});
    return {};
}

bool ValueFrom(const sol::object& o, int components, glm::vec4& out) {
    if (o.get_type() == sol::type::number) {
        const float v = o.as<float>();
        out = glm::vec4(v);
        if (components < 4) out.w = components == 1 ? 0.0f : out.w;
        return true;
    }
    if (o.is<glm::vec3>()) {
        const glm::vec3 v = o.as<glm::vec3>();
        // Цвет RGB в свойство RGBA — непрозрачный.
        out = glm::vec4(v, components == 4 ? 1.0f : 0.0f);
        return true;
    }
    if (o.is<glm::vec4>()) {
        out = o.as<glm::vec4>();
        return true;
    }
    if (o.is<glm::vec2>()) {
        const glm::vec2 v = o.as<glm::vec2>();
        out = glm::vec4(v, 0.0f, 0.0f);
        return true;
    }
    if (o.get_type() == sol::type::table) {
        sol::table t = o.as<sol::table>();
        out = glm::vec4(t.get_or(1, t.get_or("x", 0.0f)), t.get_or(2, t.get_or("y", 0.0f)),
                        t.get_or(3, t.get_or("z", 0.0f)), t.get_or(4, t.get_or("w", 1.0f)));
        return true;
    }
    return false;
}

} // namespace sage::scripting::tween
