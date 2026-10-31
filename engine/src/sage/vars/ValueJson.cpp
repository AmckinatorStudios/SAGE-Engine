#include "sage/vars/ValueJson.h"

#include <string>
#include <vector>

using json = nlohmann::json;

namespace sage::vars {

namespace {
// Те же ключи x/y/z/w, что у sage/scene/SceneJson.h: формат файлов сцены не
// меняется от того, где лежит перевод.
json V2(const glm::vec2& v) { return json{{"x", v.x}, {"y", v.y}}; }
json V3(const glm::vec3& v) { return json{{"x", v.x}, {"y", v.y}, {"z", v.z}}; }
json V4(const glm::vec4& v) { return json{{"x", v.x}, {"y", v.y}, {"z", v.z}, {"w", v.w}}; }
glm::vec2 V2From(const json& j, const glm::vec2& fallback) {
    if (!j.is_object()) return fallback;
    return {j.value("x", fallback.x), j.value("y", fallback.y)};
}
glm::vec3 V3From(const json& j) { return glm::vec3(j.value("x", 0.0f), j.value("y", 0.0f), j.value("z", 0.0f)); }
glm::vec4 V4From(const json& j, const glm::vec4& def = glm::vec4(1.0f)) {
    if (!j.is_object()) return def;
    return glm::vec4(j.value("x", def.x), j.value("y", def.y), j.value("z", def.z), j.value("w", def.w));
}
} // namespace

json ValueToJson(const Value& v) {
    using K = Kind;
    json j;
    // ВИД ПИШЕТСЯ ИМЕНЕМ, а не номером: номер сломался бы от вставки нового
    // вида в середину перечисления, и старые сцены прочитались бы наизнанку.
    j["kind"] = KindId(v.Type());
    switch (v.Type()) {
        case K::Bool: j["value"] = v.AsBool(); break;
        case K::Int: j["value"] = v.AsInt(); break;
        case K::Float: j["value"] = v.AsFloat(); break;
        case K::String: j["value"] = v.AsString(); break;
        case K::Vec2: j["value"] = V2(v.AsVec2()); break;
        case K::Vec3: j["value"] = V3(v.AsVec3()); break;
        case K::Color: j["value"] = V4(v.AsVec4()); break;
        case K::Entity: j["value"] = v.AsEntity().Id; break;
        case K::Asset: j["value"] = v.AsAsset().Path; break;
        case K::Rotation: j["value"] = V4(v.AsVec4()); break;
        case K::Array: {
            json items = json::array();
            for (const Value& x : v.Items()) items.push_back(ValueToJson(x));
            j["value"] = std::move(items);
            break;
        }
        case K::Struct: {
            // Поля — массивом: порядок полей — порядок в редакторе графа.
            json fields = json::array();
            for (const Field& f : v.Fields()) {
                json fj = ValueToJson(f.second);
                fj["name"] = f.first;
                fields.push_back(std::move(fj));
            }
            j["type"] = v.StructType();
            j["value"] = std::move(fields);
            break;
        }
    }
    return j;
}

Value ValueFromJson(const json& j) {
    using K = Kind;
    K kind = K::Bool;
    if (!j.is_object() || !j.contains("kind") || !j["kind"].is_string() ||
        !ParseKind(j["kind"].get<std::string>(), kind)) {
        return Value();
    }
    const json v = j.value("value", json());
    switch (kind) {
        case K::Bool: return Value(v.is_boolean() ? v.get<bool>() : false);
        case K::Int: return Value(v.is_number() ? v.get<int>() : 0);
        case K::Float: return Value(v.is_number() ? v.get<float>() : 0.0f);
        case K::String:
            return Value(v.is_string() ? v.get<std::string>() : std::string());
        case K::Vec2: return Value(V2From(v, glm::vec2(0.0f)));
        case K::Vec3: return Value(v.is_object() ? V3From(v) : glm::vec3(0.0f));
        case K::Color: return Value(V4From(v, glm::vec4(1.0f)));
        case K::Entity:
            return Value(EntityRef{v.is_number() ? v.get<int>() : 0});
        case K::Asset:
            return Value(
                AssetRef{v.is_string() ? v.get<std::string>() : std::string()});
        case K::Rotation: {
            const glm::vec4 q = V4From(v, glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
            return Value(glm::quat(q.w, q.x, q.y, q.z));
        }
        case K::Array: {
            std::vector<Value> items;
            if (v.is_array())
                for (const json& x : v) items.push_back(ValueFromJson(x));
            return Value::Array(std::move(items));
        }
        case K::Struct: {
            std::vector<Field> fields;
            if (v.is_array())
                for (const json& f : v)
                    if (f.is_object() && f.contains("name") && f["name"].is_string())
                        fields.emplace_back(f["name"].get<std::string>(), ValueFromJson(f));
            return Value::Struct(j.value("type", std::string()), std::move(fields));
        }
    }
    return Value();
}

} // namespace sage::vars
