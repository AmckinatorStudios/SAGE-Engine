#include "sage/vars/Value.h"

#include <cstdlib>

#include <glm/gtc/quaternion.hpp>

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

Value Value::Array(std::vector<Value> items) {
    Value v;
    auto box = std::make_shared<ArrayBox>();
    box->Items = std::move(items);
    v.m_data = std::move(box);
    return v;
}

Value Value::Struct(std::string type, std::vector<Field> fields) {
    Value v;
    auto box = std::make_shared<StructBox>();
    box->Type = std::move(type);
    box->Fields = std::move(fields);
    v.m_data = std::move(box);
    return v;
}

const std::vector<Value>& Value::Items() const {
    static const std::vector<Value> empty;
    const auto* box = Get<std::shared_ptr<ArrayBox>>();
    return (box && *box) ? (*box)->Items : empty;
}

std::vector<Value>& Value::MutableItems() {
    auto* box = Get<std::shared_ptr<ArrayBox>>();
    if (!box || !*box) {
        m_data = std::make_shared<ArrayBox>();
        box = Get<std::shared_ptr<ArrayBox>>();
    } else if (box->use_count() > 1) {
        *box = std::make_shared<ArrayBox>(**box);   // копия при записи
    }
    return (*box)->Items;
}

const std::string& Value::StructType() const {
    static const std::string empty;
    const auto* box = Get<std::shared_ptr<StructBox>>();
    return (box && *box) ? (*box)->Type : empty;
}

const std::vector<Field>& Value::Fields() const {
    static const std::vector<Field> empty;
    const auto* box = Get<std::shared_ptr<StructBox>>();
    return (box && *box) ? (*box)->Fields : empty;
}

const Value* Value::FindField(const std::string& name) const {
    for (const Field& f : Fields())
        if (f.first == name) return &f.second;
    return nullptr;
}

void Value::SetField(const std::string& name, const Value& value) {
    auto* box = Get<std::shared_ptr<StructBox>>();
    if (!box || !*box) {
        m_data = std::make_shared<StructBox>();
        box = Get<std::shared_ptr<StructBox>>();
    } else if (box->use_count() > 1) {
        *box = std::make_shared<StructBox>(**box);
    }
    for (Field& f : (*box)->Fields) {
        if (f.first == name) {
            f.second = value;
            return;
        }
    }
    (*box)->Fields.emplace_back(name, value);
}

bool Value::operator==(const Value& o) const {
    if (Type() != o.Type()) return false;
    switch (Type()) {
        case Kind::Array: return Items() == o.Items();
        case Kind::Struct: return StructType() == o.StructType() && Fields() == o.Fields();
        default: return m_data == o.m_data;
    }
}

glm::quat Value::AsRotation() const {
    if (const glm::quat* v = Get<glm::quat>()) return *v;
    if (const glm::vec3* v = Get<glm::vec3>()) return glm::quat(glm::radians(*v));
    if (const glm::vec4* v = Get<glm::vec4>()) return glm::quat(v->w, v->x, v->y, v->z);
    return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
}

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
    // Составные — строкой для отладчика графа и журнала: «[1, 2, 3]».
    if (Type() == Kind::Array) {
        std::string out = "[";
        for (size_t i = 0; i < Items().size(); ++i) out += (i ? ", " : "") + Items()[i].AsString();
        return out + "]";
    }
    if (Type() == Kind::Struct) {
        std::string out = StructType() + "{";
        bool first = true;
        for (const Field& f : Fields()) {
            out += (first ? "" : ", ") + f.first + " = " + f.second.AsString();
            first = false;
        }
        return out + "}";
    }
    if (const glm::quat* q = Get<glm::quat>()) {
        const glm::vec3 e = glm::degrees(glm::eulerAngles(*q));
        return "(" + std::to_string(e.x) + ", " + std::to_string(e.y) + ", " + std::to_string(e.z) + ")";
    }
    if (const glm::vec2* v = Get<glm::vec2>())
        return "(" + std::to_string(v->x) + ", " + std::to_string(v->y) + ")";
    if (const glm::vec3* v = Get<glm::vec3>())
        return "(" + std::to_string(v->x) + ", " + std::to_string(v->y) + ", " + std::to_string(v->z) + ")";
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
    // Поворот как три числа — углы Эйлера в градусах, обратное AsRotation.
    if (const glm::quat* q = Get<glm::quat>()) return glm::degrees(glm::eulerAngles(*q));
    return fallback;
}

glm::vec4 Value::AsVec4(glm::vec4 fallback) const {
    if (const glm::vec4* v = Get<glm::vec4>()) return *v;
    // Цвет из тройки — с непрозрачной альфой: прозрачный по умолчанию значил бы
    // «переменная есть, а на экране ничего», и искать причину пришлось бы долго.
    if (const glm::vec3* v = Get<glm::vec3>()) return glm::vec4(*v, 1.0f);
    if (const glm::vec2* v = Get<glm::vec2>()) return glm::vec4(*v, 0.0f, 1.0f);
    if (const glm::quat* q = Get<glm::quat>()) return glm::vec4(q->x, q->y, q->z, q->w);
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
        case Kind::Rotation: return Value(glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
        case Kind::Array: return Value::Array();
        case Kind::Struct: return Value::Struct("");
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
        case Kind::Rotation: return Value(from.AsRotation());
        // Одно значение в список — список из одного: провод «цель» в вход
        // «цели» — законное желание, а не ошибка.
        case Kind::Array: return Value::Array({from});
        case Kind::Struct: return Value::Struct("");
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
        case Kind::Rotation: return "rotation";
        case Kind::Array: return "array";
        case Kind::Struct: return "struct";
    }
    return "bool";
}

bool ParseKind(const std::string& id, Kind& out) {
    for (int i = 0; i <= (int)Kind::Struct; ++i) {
        if (id == KindId((Kind)i)) { out = (Kind)i; return true; }
    }
    return false;
}

const std::vector<Kind>& AllKinds() {
    static const std::vector<Kind> all = {Kind::Bool,  Kind::Int,   Kind::Float,
                                          Kind::String, Kind::Vec2,  Kind::Vec3,
                                          Kind::Color,  Kind::Entity, Kind::Asset,
                                          Kind::Rotation};
    return all;
}

} // namespace sage::vars
