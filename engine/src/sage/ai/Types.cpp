#include "sage/ai/Types.h"

namespace sage::ai {

using vars::Kind;
using vars::Value;

namespace {

bool Numeric(const std::string& t) { return t == type::Int || t == type::Float || t == type::Bool; }
bool Vector(const std::string& t) {
    return t == type::Vec2 || t == type::Vec3 || t == type::Color;
}

// Вид хранения встроенного типа.
bool KindOf(const std::string& t, Kind& k) {
    static const std::map<std::string, Kind> kinds = {
        {type::Bool, Kind::Bool},       {type::Int, Kind::Int},         {type::Float, Kind::Float},
        {type::String, Kind::String},   {type::Vec2, Kind::Vec2},       {type::Vec3, Kind::Vec3},
        {type::Color, Kind::Color},     {type::Rotation, Kind::Rotation}, {type::Object, Kind::Entity},
        {type::Asset, Kind::Asset},     {type::Array, Kind::Array},     {type::Struct, Kind::Struct},
        {type::Task, Kind::Int},
    };
    auto it = kinds.find(t);
    if (it == kinds.end()) return false;
    k = it->second;
    return true;
}

} // namespace

TypeRegistry::TypeRegistry() {
    auto add = [this](const char* id, const char* title, Value def, uint32_t color) {
        TypeInfo t;
        t.Id = id;
        t.Title = title;
        t.Default = std::move(def);
        t.Color = color;
        m_types[id] = std::move(t);
    };
    add(type::Any, "Any", Value(), 0xFFB0B0B0);
    add(type::Exec, "Flow", Value(), 0xFFFFFFFF);
    add(type::Bool, "Boolean", Value(false), 0xFF4040D0);
    add(type::Int, "Integer", Value(0), 0xFFA0D040);
    add(type::Float, "Float", Value(0.0f), 0xFF60D090);
    add(type::String, "String", Value(std::string()), 0xFFD060D0);
    add(type::Vec2, "Vector2", Value(glm::vec2(0.0f)), 0xFF40C0F0);
    add(type::Vec3, "Vector3", Value(glm::vec3(0.0f)), 0xFF30A0F0);
    add(type::Color, "Color", Value(glm::vec4(1.0f)), 0xFF80E0F0);
    add(type::Rotation, "Rotation", Value(glm::quat(1.0f, 0.0f, 0.0f, 0.0f)), 0xFFE0A060);
    add(type::Object, "Object Reference", Value(vars::EntityRef{}), 0xFFE07030);
    add(type::Asset, "Asset", Value(vars::AssetRef{}), 0xFF8080E0);
    add(type::Array, "Array", Value::Array(), 0xFF60E0E0);
    add(type::Struct, "Struct", Value::Struct(""), 0xFFE0E060);
    add(type::Task, "Task", Value(0), 0xFF40A0FF);

    // Transform — структура: позиция, поворот, масштаб. Структурой, а не
    // отдельным видом значения, — с ней работают те же struct.make/break.
    TypeInfo tr;
    tr.Id = type::Transform;
    tr.Title = "Transform";
    tr.Fields = {{"position", Value(glm::vec3(0.0f))},
                 {"rotation", Value(glm::quat(1.0f, 0.0f, 0.0f, 0.0f))},
                 {"scale", Value(glm::vec3(1.0f))}};
    tr.Color = 0xFFF09060;
    Register(std::move(tr));
}

void TypeRegistry::Register(TypeInfo info) {
    if (info.Id.empty()) return;
    if (!info.Fields.empty()) {
        std::vector<vars::Field> fields;
        for (const Param& p : info.Fields) fields.emplace_back(p.Name, p.Default);
        info.Default = Value::Struct(info.Id, std::move(fields));
    }
    m_types[info.Id] = std::move(info);
    ++m_version;
}

const TypeInfo* TypeRegistry::Find(const std::string& id) const {
    auto it = m_types.find(id);
    return it == m_types.end() ? nullptr : &it->second;
}

std::vector<const TypeInfo*> TypeRegistry::All() const {
    std::vector<const TypeInfo*> out;
    for (const auto& [_, t] : m_types) out.push_back(&t);
    return out;
}

bool TypeRegistry::IsStruct(const std::string& id) const {
    const TypeInfo* t = Find(id);
    return id == type::Struct || (t && !t->Fields.empty());
}

bool TypeRegistry::CanConnect(const std::string& from, const std::string& to, std::string* why) const {
    auto deny = [&](const std::string& reason) {
        if (why) *why = reason;
        return false;
    };
    for (const ConnectRule& r : m_connect) {
        const int verdict = r(from, to);
        if (verdict == 1) return true;
        if (verdict == 0) return deny("правило проекта запрещает " + from + " -> " + to);
    }
    const bool fromExec = from == type::Exec, toExec = to == type::Exec;
    if (fromExec || toExec) {
        if (fromExec && toExec) return true;
        return deny("поток выполнения соединяется только с потоком");
    }
    if (from == to || from == type::Any || to == type::Any) return true;
    if (Numeric(from) && Numeric(to)) return true;
    if (to == type::String) return true;   // показать можно что угодно
    if (Vector(from) && Vector(to)) return true;
    if (from == type::Vec3 && to == type::Rotation) return true;
    if (from == type::Rotation && to == type::Vec3) return true;
    if (to == type::Array) return true;     // одно значение — список из одного
    if (IsStruct(from) && IsStruct(to) && (from == type::Struct || to == type::Struct)) return true;
    if (from == type::Int && to == type::Object) return true;   // номер объекта
    if (from == type::Int && to == type::Task) return true;
    return deny("тип " + from + " не подходит ко входу " + to);
}

Value TypeRegistry::Convert(const Value& value, const std::string& to) const {
    for (const ConvertRule& r : m_convert) {
        Value out;
        if (r(value, to, out)) return out;
    }
    if (to == type::Any || to == type::Exec || to.empty()) return value;
    if (const TypeInfo* t = Find(to); t && !t->Fields.empty()) {
        // Именованная структура: недостающие поля — умолчанием, лишние
        // остаются (в них может лежать то, что положил скрипт).
        Value out = value.Type() == Kind::Struct ? value : t->Default;
        if (value.Type() == Kind::Struct && out.StructType().empty()) out = Value::Struct(to, value.Fields());
        for (const Param& f : t->Fields) {
            const Value* have = out.FindField(f.Name);
            if (!have) out.SetField(f.Name, f.Default);
            else if (have->Type() != f.Default.Type()) out.SetField(f.Name, Convert(*have, Of(f.Default)));
        }
        return out;
    }
    Kind k;
    if (!KindOf(to, k)) return value;
    if (value.Type() == k) return value;
    return Value::Convert(value, k);
}

Value TypeRegistry::Default(const std::string& id) const {
    const TypeInfo* t = Find(id);
    return t ? t->Default : Value();
}

std::string TypeRegistry::Of(const Value& value) const {
    switch (value.Type()) {
        case Kind::Bool: return type::Bool;
        case Kind::Int: return type::Int;
        case Kind::Float: return type::Float;
        case Kind::String: return type::String;
        case Kind::Vec2: return type::Vec2;
        case Kind::Vec3: return type::Vec3;
        case Kind::Color: return type::Color;
        case Kind::Entity: return type::Object;
        case Kind::Asset: return type::Asset;
        case Kind::Rotation: return type::Rotation;
        case Kind::Array: return type::Array;
        case Kind::Struct: {
            const std::string& t = value.StructType();
            return (!t.empty() && Find(t)) ? t : std::string(type::Struct);
        }
    }
    return type::Any;
}

} // namespace sage::ai
