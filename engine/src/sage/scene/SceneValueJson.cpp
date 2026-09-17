#include "sage/scene/SceneValueJson.h"

#include "sage/scene/SceneJson.h"

#include <string>

using json = nlohmann::json;

namespace sage::scene {

json ValueToJson(const sage::vars::Value& v) {
    using K = sage::vars::Kind;
    json j;
    // ВИД ПИШЕТСЯ ИМЕНЕМ, а не номером: номер сломался бы от вставки нового
    // вида в середину перечисления, и старые сцены прочитались бы наизнанку.
    j["kind"] = sage::vars::KindId(v.Type());
    switch (v.Type()) {
        case K::Bool: j["value"] = v.AsBool(); break;
        case K::Int: j["value"] = v.AsInt(); break;
        case K::Float: j["value"] = v.AsFloat(); break;
        case K::String: j["value"] = v.AsString(); break;
        case K::Vec2: j["value"] = Vec2ToJson(v.AsVec2()); break;
        case K::Vec3: j["value"] = Vec3ToJson(v.AsVec3()); break;
        case K::Color: j["value"] = Vec4ToJson(v.AsVec4()); break;
        case K::Entity: j["value"] = v.AsEntity().Id; break;
        case K::Asset: j["value"] = v.AsAsset().Path; break;
    }
    return j;
}

sage::vars::Value ValueFromJson(const json& j) {
    using K = sage::vars::Kind;
    K kind = K::Bool;
    if (!j.is_object() || !j.contains("kind") || !j["kind"].is_string() ||
        !sage::vars::ParseKind(j["kind"].get<std::string>(), kind)) {
        return sage::vars::Value();
    }
    const json v = j.value("value", json());
    switch (kind) {
        case K::Bool: return sage::vars::Value(v.is_boolean() ? v.get<bool>() : false);
        case K::Int: return sage::vars::Value(v.is_number() ? v.get<int>() : 0);
        case K::Float: return sage::vars::Value(v.is_number() ? v.get<float>() : 0.0f);
        case K::String:
            return sage::vars::Value(v.is_string() ? v.get<std::string>() : std::string());
        case K::Vec2: return sage::vars::Value(Vec2FromJson(v, glm::vec2(0.0f)));
        case K::Vec3: return sage::vars::Value(v.is_object() ? Vec3FromJson(v) : glm::vec3(0.0f));
        case K::Color: return sage::vars::Value(Vec4FromJson(v, glm::vec4(1.0f)));
        case K::Entity:
            return sage::vars::Value(sage::vars::EntityRef{v.is_number() ? v.get<int>() : 0});
        case K::Asset:
            return sage::vars::Value(
                sage::vars::AssetRef{v.is_string() ? v.get<std::string>() : std::string()});
    }
    return sage::vars::Value();
}

json VarsToJson(const sage::vars::Table& table) {
    // МАССИВ, а не объект: порядок переменных — это порядок в инспекторе, и
    // объект JSON его не обещает.
    json out = json::array();
    for (const sage::vars::Var& var : table.All()) {
        json j = ValueToJson(var.Data);
        j["name"] = var.Name;
        // Описание пишется, только если оно есть: у переменной, заведённой
        // руками, его нет, и пустые ключи в файле — это шум.
        if (!var.Label.empty()) j["label"] = var.Label;
        if (!var.Tooltip.empty()) j["tooltip"] = var.Tooltip;
        if (var.Min != var.Max) { j["min"] = var.Min; j["max"] = var.Max; }
        if (var.Declared) j["declared"] = true;
        out.push_back(std::move(j));
    }
    return out;
}

void VarsFromJson(const json& in, sage::vars::Table& table) {
    table.Clear();
    if (!in.is_array()) return;
    for (const json& j : in) {
        if (!j.is_object() || !j.contains("name") || !j["name"].is_string()) continue;
        sage::vars::Var var;
        var.Name = j["name"].get<std::string>();
        var.Data = ValueFromJson(j);
        var.Label = j.value("label", std::string());
        var.Tooltip = j.value("tooltip", std::string());
        var.Min = j.value("min", 0.0f);
        var.Max = j.value("max", 0.0f);
        var.Declared = j.value("declared", false);
        table.Put(var);
    }
}

json BindingsToJson(const sage::events::Bindings& bindings) {
    json out = json::array();
    for (const sage::events::Binding& b : bindings) {
        json j;
        j["trigger"] = b.Trigger;
        if (!b.Event.empty()) j["event"] = b.Event;
        if (b.Target.Valid()) j["target"] = b.Target.Id;
        if (!b.Method.empty()) j["method"] = b.Method;
        j["arg"] = ValueToJson(b.Arg);
        if (!b.Enabled) j["enabled"] = false;
        out.push_back(std::move(j));
    }
    return out;
}

void BindingsFromJson(const json& in, sage::events::Bindings& out) {
    out.clear();
    if (!in.is_array()) return;
    for (const json& j : in) {
        if (!j.is_object()) continue;
        sage::events::Binding b;
        b.Trigger = j.value("trigger", std::string());
        b.Event = j.value("event", std::string());
        b.Target.Id = j.value("target", 0);
        b.Method = j.value("method", std::string());
        if (j.contains("arg")) b.Arg = ValueFromJson(j["arg"]);
        b.Enabled = j.value("enabled", true);
        out.push_back(std::move(b));
    }
}

// Одно поле части -> json.

} // namespace sage::scene
