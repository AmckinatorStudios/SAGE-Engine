#include "sage/vars/Value.h"

#include <cstdlib>

namespace sage::vars {

namespace {
// Читать число из строки НАДО: значение приходит и из файла, и из поля ввода,
// и «12» в строковой переменной — это двенадцать, а не ноль.
float NumberOf(const std::string& s, float fallback) {
    try {
        size_t used = 0;
        const float v = std::stof(s, &used);
        return used > 0 ? v : fallback;
    } catch (...) {
        return fallback;
    }
}
} // namespace

bool Value::AsBool(bool fallback) const {
    if (const bool* v = Get<bool>()) return *v;
    if (const int* v = Get<int>()) return *v != 0;
    if (const float* v = Get<float>()) return *v != 0.0f;
    if (const std::string* v = Get<std::string>()) return *v == "true" || *v == "1";
    if (const EntityRef* v = Get<EntityRef>()) return v->Valid();
    if (const AssetRef* v = Get<AssetRef>()) return v->Valid();
    return fallback;
}

int Value::AsInt(int fallback) const {
    if (const int* v = Get<int>()) return *v;
    if (const bool* v = Get<bool>()) return *v ? 1 : 0;
    if (const float* v = Get<float>()) return (int)*v;
    if (const std::string* v = Get<std::string>()) return (int)NumberOf(*v, (float)fallback);
    if (const EntityRef* v = Get<EntityRef>()) return v->Id;
    return fallback;
}

float Value::AsFloat(float fallback) const {
    if (const float* v = Get<float>()) return *v;
    if (const int* v = Get<int>()) return (float)*v;
    if (const bool* v = Get<bool>()) return *v ? 1.0f : 0.0f;
    if (const std::string* v = Get<std::string>()) return NumberOf(*v, fallback);
    return fallback;
}

std::string Value::AsString(const std::string& fallback) const {
    if (const std::string* v = Get<std::string>()) return *v;
    if (const AssetRef* v = Get<AssetRef>()) return v->Path;
    if (const bool* v = Get<bool>()) return *v ? "true" : "false";
    if (const int* v = Get<int>()) return std::to_string(*v);
    if (const float* v = Get<float>()) return std::to_string(*v);
    if (const EntityRef* v = Get<EntityRef>()) return std::to_string(v->Id);
    return fallback;
}

glm::vec2 Value::AsVec2(glm::vec2 fallback) const {
    if (const glm::vec2* v = Get<glm::vec2>()) return *v;
    if (const glm::vec3* v = Get<glm::vec3>()) return glm::vec2(*v);
    if (const glm::vec4* v = Get<glm::vec4>()) return glm::vec2(*v);
    return fallback;
}

glm::vec3 Value::AsVec3(glm::vec3 fallback) const {
    if (const glm::vec3* v = Get<glm::vec3>()) return *v;
    if (const glm::vec4* v = Get<glm::vec4>()) return glm::vec3(*v);
    if (const glm::vec2* v = Get<glm::vec2>()) return glm::vec3(*v, 0.0f);
    return fallback;
}

glm::vec4 Value::AsVec4(glm::vec4 fallback) const {
    if (const glm::vec4* v = Get<glm::vec4>()) return *v;
    // Цвет из тройки — с непрозрачной альфой: прозрачный по умолчанию значил бы
    // «переменная есть, а на экране ничего», и искать причину пришлось бы долго.
    if (const glm::vec3* v = Get<glm::vec3>()) return glm::vec4(*v, 1.0f);
    if (const glm::vec2* v = Get<glm::vec2>()) return glm::vec4(*v, 0.0f, 1.0f);
    return fallback;
}

EntityRef Value::AsEntity() const {
    if (const EntityRef* v = Get<EntityRef>()) return *v;
    // Число как ссылка — законно: скрипт хранит Id и присваивает его напрямую.
    if (const int* v = Get<int>()) return EntityRef{*v};
    return {};
}

AssetRef Value::AsAsset() const {
    if (const AssetRef* v = Get<AssetRef>()) return *v;
    if (const std::string* v = Get<std::string>()) return AssetRef{*v};
    return {};
}

Value Value::Default(Kind kind) {
    switch (kind) {
        case Kind::Bool: return Value(false);
        case Kind::Int: return Value(0);
        case Kind::Float: return Value(0.0f);
        case Kind::String: return Value(std::string());
        case Kind::Vec2: return Value(glm::vec2(0.0f));
        case Kind::Vec3: return Value(glm::vec3(0.0f));
        case Kind::Color: return Value(glm::vec4(1.0f));
        case Kind::Entity: return Value(EntityRef{});
        case Kind::Asset: return Value(AssetRef{});
    }
    return Value();
}

Value Value::Convert(const Value& from, Kind to) {
    if (from.Type() == to) return from;
    switch (to) {
        case Kind::Bool: return Value(from.AsBool());
        case Kind::Int: return Value(from.AsInt());
        case Kind::Float: return Value(from.AsFloat());
        case Kind::String: return Value(from.AsString());
        case Kind::Vec2: return Value(from.AsVec2());
        case Kind::Vec3: return Value(from.AsVec3());
        case Kind::Color: return Value(from.AsVec4());
        case Kind::Entity: return Value(from.AsEntity());
        case Kind::Asset: return Value(from.AsAsset());
    }
    return Value::Default(to);
}

const char* KindId(Kind kind) {
    switch (kind) {
        case Kind::Bool: return "bool";
        case Kind::Int: return "int";
        case Kind::Float: return "float";
        case Kind::String: return "string";
        case Kind::Vec2: return "vec2";
        case Kind::Vec3: return "vec3";
        case Kind::Color: return "color";
        case Kind::Entity: return "entity";
        case Kind::Asset: return "asset";
    }
    return "bool";
}

bool ParseKind(const std::string& id, Kind& out) {
    for (Kind k : AllKinds()) {
        if (id == KindId(k)) { out = k; return true; }
    }
    return false;
}

const std::vector<Kind>& AllKinds() {
    static const std::vector<Kind> all = {Kind::Bool,  Kind::Int,   Kind::Float,
                                          Kind::String, Kind::Vec2,  Kind::Vec3,
                                          Kind::Color,  Kind::Entity, Kind::Asset};
    return all;
}

} // namespace sage::vars
