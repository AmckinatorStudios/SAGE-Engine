#include "sage/anim/AnimProperty.h"

#include <algorithm>
#include <cstring>

#include "sage/scene/Transform.h"
#include "sage/ui/UI.h"
#include "sage/ui/UIPart.h"

namespace sage::anim {

namespace {

std::vector<PropertyType>& Registry() {
    static std::vector<PropertyType> reg;
    return reg;
}

// --- Свойства самой сущности -------------------------------------------------
//
// Положение, поворот и масштаб — не «часть», а сам объект, и таблицы полей у
// них нет: Transform старше реестра частей и лежит в основании всей сцены.
// Поэтому три записи руками — единственные во всём файле.
void RegisterObjectProperties() {
    auto make = [](const char* id, const char* title) {
        PropertyType p;
        p.Id = id;
        p.Title = title;
        p.Group = "Transform";
        p.Components = 3;
        p.Has = [](const entt::registry& r, entt::entity e) { return r.all_of<Transform>(e); };
        return p;
    };
    {
        PropertyType p = make("object.position", "Position");
        p.Get = [](const entt::registry& r, entt::entity e) {
            const Transform* t = r.try_get<Transform>(e);
            return t ? glm::vec4(t->Position, 0.0f) : glm::vec4(0.0f);
        };
        p.Set = [](entt::registry& r, entt::entity e, const glm::vec4& v) {
            if (Transform* t = r.try_get<Transform>(e)) t->Position = glm::vec3(v);
        };
        RegisterProperty(p);
    }
    {
        PropertyType p = make("object.rotation", "Rotation");
        p.Get = [](const entt::registry& r, entt::entity e) {
            const Transform* t = r.try_get<Transform>(e);
            return t ? glm::vec4(t->Rotation, 0.0f) : glm::vec4(0.0f);
        };
        p.Set = [](entt::registry& r, entt::entity e, const glm::vec4& v) {
            if (Transform* t = r.try_get<Transform>(e)) t->Rotation = glm::vec3(v);
        };
        RegisterProperty(p);
    }
    {
        PropertyType p = make("object.scale", "Scale");
        p.Get = [](const entt::registry& r, entt::entity e) {
            const Transform* t = r.try_get<Transform>(e);
            return t ? glm::vec4(t->Scale, 0.0f) : glm::vec4(1.0f);
        };
        p.Set = [](entt::registry& r, entt::entity e, const glm::vec4& v) {
            if (Transform* t = r.try_get<Transform>(e)) t->Scale = glm::vec3(v);
        };
        RegisterProperty(p);
    }
}

// --- Свойства элемента интерфейса -------------------------------------------
//
// БЕРУТСЯ ИЗ ТАБЛИЦ ЧАСТЕЙ, а не перечисляются здесь. Это и есть весь смысл:
// часть, объявившая у себя числовое поле, становится анимируемой сама, и ни
// редактор анимации, ни этот файл про неё не знают.
int ComponentsOf(ui::PartField::Kind kind) {
    switch (kind) {
        case ui::PartField::Kind::Float: return 1;
        case ui::PartField::Kind::Vec2: return 2;
        case ui::PartField::Kind::Color:
        case ui::PartField::Kind::Vec4: return 4;
        // Остальное анимировать нечем: у пути к файлу, галки, перечисления и
        // списка связей нет промежуточного значения. Целое число намеренно
        // тоже нет: «порядок отрисовки 2.5» не значит ничего.
        default: return 0;
    }
}

// Доступ к полю части по её Id и ключу поля. Ищется каждый раз заново, потому
// что реестр частей игра вправе подменить: закэшированный указатель на поле
// пережил бы подмену и писал бы в чужую память.
struct FieldRef {
    const ui::PartType* Part = nullptr;
    const ui::PartField* Field = nullptr;
};

FieldRef ResolveField(const std::string& partId, const std::string& fieldKey) {
    FieldRef ref;
    ref.Part = ui::FindPart(partId);
    if (!ref.Part || !ref.Part->Fields) return {};
    for (const ui::PartField& f : *ref.Part->Fields) {
        if (f.Key && fieldKey == f.Key) { ref.Field = &f; break; }
    }
    if (!ref.Field) return {};
    return ref;
}

glm::vec4 ReadField(const void* data, const ui::PartField& f) {
    const char* p = static_cast<const char*>(data) + f.Offset;
    switch (f.Type) {
        case ui::PartField::Kind::Float: return glm::vec4(*reinterpret_cast<const float*>(p), 0, 0, 0);
        case ui::PartField::Kind::Vec2: {
            const glm::vec2 v = *reinterpret_cast<const glm::vec2*>(p);
            return glm::vec4(v.x, v.y, 0.0f, 0.0f);
        }
        case ui::PartField::Kind::Color:
        case ui::PartField::Kind::Vec4: return *reinterpret_cast<const glm::vec4*>(p);
        default: return glm::vec4(0.0f);
    }
}

void WriteField(void* data, const ui::PartField& f, const glm::vec4& v) {
    char* p = static_cast<char*>(data) + f.Offset;
    switch (f.Type) {
        case ui::PartField::Kind::Float: *reinterpret_cast<float*>(p) = v.x; break;
        case ui::PartField::Kind::Vec2: *reinterpret_cast<glm::vec2*>(p) = glm::vec2(v); break;
        case ui::PartField::Kind::Color:
        case ui::PartField::Kind::Vec4: *reinterpret_cast<glm::vec4*>(p) = v; break;
        default: break;
    }
}

// Ключ свойства разбирается на части при КАЖДОМ обращении, а не запоминается в
// лямбде: лямбда живёт в реестре, а строки, из которых её собрали, — во
// временных переменных цикла регистрации.
void SplitId(const std::string& id, std::string& part, std::string& field) {
    const size_t dot = id.find('.');
    if (dot == std::string::npos) { part = id; field.clear(); return; }
    part = id.substr(0, dot);
    field = id.substr(dot + 1);
}

void RegisterUIProperties() {
    for (const ui::PartType& part : ui::Parts()) {
        if (!part.Fields || !part.Id) continue;
        for (const ui::PartField& f : *part.Fields) {
            const int comps = ComponentsOf(f.Type);
            if (comps == 0 || !f.Key) continue;
            PropertyType p;
            p.Id = std::string(part.Id) + "." + f.Key;
            p.Title = f.Label ? f.Label : f.Key;
            p.Group = part.Title ? part.Title : part.Id;
            p.Components = comps;
            // Has/Get/Set НЕ ставятся: доступ к полю части идёт по её таблице
            // (см. ReadProperty). Захватывающая лямбда сюда не влезла бы —
            // указателю на функцию захват не положен, — а заводить ради этого
            // std::function на каждое поле значит платить аллокацией за то,
            // что и так однозначно выводится из ключа.
            RegisterProperty(p);
        }
    }
}

bool& Ready() {
    static bool ready = false;
    return ready;
}

void EnsureBuiltIn() {
    if (Ready()) return;
    Ready() = true;
    RegisterObjectProperties();
    RegisterUIProperties();
}

} // namespace

void RegisterProperty(const PropertyType& type) {
    std::vector<PropertyType>& reg = Registry();
    for (PropertyType& p : reg) {
        if (p.Id == type.Id) { p = type; return; }
    }
    reg.push_back(type);
}

const std::vector<PropertyType>& Properties() {
    EnsureBuiltIn();
    return Registry();
}

const PropertyType* FindProperty(std::string_view id) {
    for (const PropertyType& p : Properties()) {
        if (p.Id == id) return &p;
    }
    return nullptr;
}

// --- Чтение и запись по ключу ------------------------------------------------
//
// Общая часть для свойств интерфейса: реестр хранит только описание, а сам
// доступ идёт через таблицу части. Свойства объекта отвечают своими Get/Set —
// у них таблицы нет.
bool ReadProperty(const PropertyType& p, const entt::registry& reg, entt::entity e,
                  glm::vec4& out) {
    if (p.Get) { out = p.Get(reg, e); return true; }
    std::string partId, fieldKey;
    SplitId(p.Id, partId, fieldKey);
    const FieldRef ref = ResolveField(partId, fieldKey);
    if (!ref.Part || !ref.Field || !ref.Part->Has || !ref.Part->Has(reg, e)) return false;
    const void* data = ref.Part->Get(reg, e);
    if (!data) return false;
    out = ReadField(data, *ref.Field);
    return true;
}

bool WriteProperty(const PropertyType& p, entt::registry& reg, entt::entity e,
                   const glm::vec4& value) {
    if (p.Set) { p.Set(reg, e, value); return true; }
    std::string partId, fieldKey;
    SplitId(p.Id, partId, fieldKey);
    const FieldRef ref = ResolveField(partId, fieldKey);
    if (!ref.Part || !ref.Field || !ref.Part->Has || !ref.Part->Has(reg, e)) return false;
    void* data = ref.Part->GetMutable(reg, e);
    if (!data) return false;
    WriteField(data, *ref.Field, value);
    return true;
}

bool HasProperty(const PropertyType& p, const entt::registry& reg, entt::entity e) {
    if (p.Get) return p.Has ? p.Has(reg, e) : true;
    std::string partId, fieldKey;
    SplitId(p.Id, partId, fieldKey);
    const FieldRef ref = ResolveField(partId, fieldKey);
    return ref.Part && ref.Field && ref.Part->Has && ref.Part->Has(reg, e);
}

std::vector<const PropertyType*> PropertiesFor(const entt::registry& reg, entt::entity e) {
    std::vector<const PropertyType*> out;
    for (const PropertyType& p : Properties()) {
        if (HasProperty(p, reg, e)) out.push_back(&p);
    }
    return out;
}

} // namespace sage::anim
