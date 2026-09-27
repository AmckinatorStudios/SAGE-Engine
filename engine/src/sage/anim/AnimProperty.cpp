#include "sage/anim/AnimProperty.h"

#include <algorithm>
#include <cstring>
#include <memory>

#include "sage/audio/AudioComponents.h"
#include "sage/ecs/CameraLightComponents.h"
#include "sage/ecs/RenderComponents.h"
#include "sage/render/Material.h"
#include "sage/scene/Transform.h"
#include "sage/ui/Element.h"
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

// --- Свойства компонентов сцены ---------------------------------------------
//
// Материал, свет, камера, звук: у них нет таблицы полей, как у частей
// интерфейса, поэтому доступ — руками, по записи на свойство. Каждая запись —
// четыре строки, и добавить новое свойство значит дописать ещё одну здесь:
// ни твины, ни анимация, ни редакторы о нём больше ничего знать не должны.
// Материал объекта ОБЩИЙ для всех, у кого тот же файл. Твин цвета одной двери
// не должен перекрасить все двери уровня, поэтому первая запись заводит
// объекту СВОЮ копию материала (текстуры в ней общие — копируется только
// описание). Копия живёт в памяти, в сцену не пишется: сохраняется путь.
Material* OwnMaterial(entt::registry& r, entt::entity e) {
    MeshRendererComponent* mr = r.try_get<MeshRendererComponent>(e);
    if (!mr || !mr->MaterialPtr) return nullptr;
    if (mr->MaterialPtr.get() != mr->OwnedMaterial) {
        mr->MaterialPtr = std::make_shared<Material>(*mr->MaterialPtr);
        mr->OwnedMaterial = mr->MaterialPtr.get();
    }
    return mr->MaterialPtr.get();
}

void RegisterSceneProperties() {
    {
        // Цвет: у объекта с материалом — базовый цвет материала, без
        // материала — цвет самого объекта. Одно свойство «цвет» для человека.
        PropertyType p;
        p.Id = "material.color";
        p.Title = "Colour";
        p.Group = "Material";
        p.Components = 3;
        p.Has = [](const entt::registry& r, entt::entity e) { return r.all_of<MeshRendererComponent>(e); };
        p.Get = [](const entt::registry& r, entt::entity e) {
            const MeshRendererComponent* mr = r.try_get<MeshRendererComponent>(e);
            if (!mr) return glm::vec4(1.0f);
            return glm::vec4(mr->MaterialPtr ? mr->MaterialPtr->Albedo : mr->Color, 1.0f);
        };
        p.Set = [](entt::registry& r, entt::entity e, const glm::vec4& v) {
            if (Material* m = OwnMaterial(r, e)) m->Albedo = glm::vec3(v);
            else if (MeshRendererComponent* mr = r.try_get<MeshRendererComponent>(e)) mr->Color = glm::vec3(v);
        };
        RegisterProperty(p);
    }
    {
        PropertyType p;
        p.Id = "material.opacity";
        p.Title = "Opacity";
        p.Group = "Material";
        p.Components = 1;
        p.Has = [](const entt::registry& r, entt::entity e) {
            const MeshRendererComponent* mr = r.try_get<MeshRendererComponent>(e);
            return mr && mr->MaterialPtr;
        };
        p.Get = [](const entt::registry& r, entt::entity e) {
            const MeshRendererComponent* mr = r.try_get<MeshRendererComponent>(e);
            return glm::vec4(mr && mr->MaterialPtr ? mr->MaterialPtr->Opacity : 1.0f, 0, 0, 0);
        };
        p.Set = [](entt::registry& r, entt::entity e, const glm::vec4& v) {
            if (Material* m = OwnMaterial(r, e)) m->Opacity = glm::clamp(v.x, 0.0f, 1.0f);
        };
        RegisterProperty(p);
    }
    {
        PropertyType p;
        p.Id = "light.intensity";
        p.Title = "Intensity";
        p.Group = "Light";
        p.Has = [](const entt::registry& r, entt::entity e) { return r.all_of<LightComponent>(e); };
        p.Get = [](const entt::registry& r, entt::entity e) {
            const LightComponent* l = r.try_get<LightComponent>(e);
            return glm::vec4(l ? l->Intensity : 0.0f, 0, 0, 0);
        };
        p.Set = [](entt::registry& r, entt::entity e, const glm::vec4& v) {
            if (LightComponent* l = r.try_get<LightComponent>(e)) l->Intensity = std::max(v.x, 0.0f);
        };
        RegisterProperty(p);
    }
    {
        PropertyType p;
        p.Id = "light.color";
        p.Title = "Colour";
        p.Group = "Light";
        p.Components = 3;
        p.Has = [](const entt::registry& r, entt::entity e) { return r.all_of<LightComponent>(e); };
        p.Get = [](const entt::registry& r, entt::entity e) {
            const LightComponent* l = r.try_get<LightComponent>(e);
            return glm::vec4(l ? l->Color : glm::vec3(1.0f), 1.0f);
        };
        p.Set = [](entt::registry& r, entt::entity e, const glm::vec4& v) {
            if (LightComponent* l = r.try_get<LightComponent>(e)) l->Color = glm::vec3(v);
        };
        RegisterProperty(p);
    }
    {
        PropertyType p;
        p.Id = "light.range";
        p.Title = "Range";
        p.Group = "Light";
        p.Has = [](const entt::registry& r, entt::entity e) { return r.all_of<LightComponent>(e); };
        p.Get = [](const entt::registry& r, entt::entity e) {
            const LightComponent* l = r.try_get<LightComponent>(e);
            return glm::vec4(l ? l->Range : 0.0f, 0, 0, 0);
        };
        p.Set = [](entt::registry& r, entt::entity e, const glm::vec4& v) {
            if (LightComponent* l = r.try_get<LightComponent>(e)) l->Range = std::max(v.x, 0.0f);
        };
        RegisterProperty(p);
    }
    {
        PropertyType p;
        p.Id = "camera.fov";
        p.Title = "Field of view";
        p.Group = "Camera";
        p.Has = [](const entt::registry& r, entt::entity e) { return r.all_of<CameraComponent>(e); };
        p.Get = [](const entt::registry& r, entt::entity e) {
            const CameraComponent* c = r.try_get<CameraComponent>(e);
            return glm::vec4(c ? c->Fov : 60.0f, 0, 0, 0);
        };
        p.Set = [](entt::registry& r, entt::entity e, const glm::vec4& v) {
            if (CameraComponent* c = r.try_get<CameraComponent>(e)) c->Fov = glm::clamp(v.x, 1.0f, 179.0f);
        };
        RegisterProperty(p);
    }
    {
        PropertyType p;
        p.Id = "audio.volume";
        p.Title = "Volume";
        p.Group = "Audio";
        p.Has = [](const entt::registry& r, entt::entity e) { return r.all_of<AudioSourceComponent>(e); };
        p.Get = [](const entt::registry& r, entt::entity e) {
            const AudioSourceComponent* a = r.try_get<AudioSourceComponent>(e);
            return glm::vec4(a ? a->Volume : 0.0f, 0, 0, 0);
        };
        p.Set = [](entt::registry& r, entt::entity e, const glm::vec4& v) {
            if (AudioSourceComponent* a = r.try_get<AudioSourceComponent>(e)) a->Volume = glm::clamp(v.x, 0.0f, 1.0f);
        };
        RegisterProperty(p);
    }
    {
        PropertyType p;
        p.Id = "audio.pitch";
        p.Title = "Pitch";
        p.Group = "Audio";
        p.Has = [](const entt::registry& r, entt::entity e) { return r.all_of<AudioSourceComponent>(e); };
        p.Get = [](const entt::registry& r, entt::entity e) {
            const AudioSourceComponent* a = r.try_get<AudioSourceComponent>(e);
            return glm::vec4(a ? a->Pitch : 1.0f, 0, 0, 0);
        };
        p.Set = [](entt::registry& r, entt::entity e, const glm::vec4& v) {
            if (AudioSourceComponent* a = r.try_get<AudioSourceComponent>(e)) a->Pitch = std::max(v.x, 0.01f);
        };
        RegisterProperty(p);
    }
}

// --- Сам элемент интерфейса ------------------------------------------------
//
// Положение, размер, поворот, масштаб и непрозрачность лежат в Element, а не в
// части с таблицей полей: элемент — основание, на котором части стоят.
void RegisterElementProperties() {
    auto make = [](const char* id, const char* title, int comps) {
        PropertyType p;
        p.Id = id;
        p.Title = title;
        p.Group = "Element";
        p.Components = comps;
        p.Has = [](const entt::registry& r, entt::entity e) { return r.all_of<ui::Element>(e); };
        return p;
    };
    {
        PropertyType p = make("element.position", "Position", 2);
        p.Get = [](const entt::registry& r, entt::entity e) {
            const ui::Element* el = r.try_get<ui::Element>(e);
            return el ? glm::vec4(el->Position, 0, 0) : glm::vec4(0.0f);
        };
        p.Set = [](entt::registry& r, entt::entity e, const glm::vec4& v) {
            if (ui::Element* el = r.try_get<ui::Element>(e)) el->Position = glm::vec2(v);
        };
        RegisterProperty(p);
    }
    {
        PropertyType p = make("element.size", "Size", 2);
        p.Get = [](const entt::registry& r, entt::entity e) {
            const ui::Element* el = r.try_get<ui::Element>(e);
            return el ? glm::vec4(el->Size, 0, 0) : glm::vec4(0.0f);
        };
        p.Set = [](entt::registry& r, entt::entity e, const glm::vec4& v) {
            if (ui::Element* el = r.try_get<ui::Element>(e)) el->Size = glm::max(glm::vec2(v), glm::vec2(0.0f));
        };
        RegisterProperty(p);
    }
    {
        PropertyType p = make("element.rotation", "Rotation", 1);
        p.Get = [](const entt::registry& r, entt::entity e) {
            const ui::Element* el = r.try_get<ui::Element>(e);
            return glm::vec4(el ? el->Rotation : 0.0f, 0, 0, 0);
        };
        p.Set = [](entt::registry& r, entt::entity e, const glm::vec4& v) {
            if (ui::Element* el = r.try_get<ui::Element>(e)) el->Rotation = v.x;
        };
        RegisterProperty(p);
    }
    {
        PropertyType p = make("element.scale", "Scale", 1);
        p.Get = [](const entt::registry& r, entt::entity e) {
            const ui::Element* el = r.try_get<ui::Element>(e);
            return glm::vec4(el ? el->Scale : 1.0f, 0, 0, 0);
        };
        p.Set = [](entt::registry& r, entt::entity e, const glm::vec4& v) {
            if (ui::Element* el = r.try_get<ui::Element>(e)) el->Scale = std::max(v.x, 0.0f);
        };
        RegisterProperty(p);
    }
    {
        PropertyType p = make("element.opacity", "Opacity", 1);
        p.Get = [](const entt::registry& r, entt::entity e) {
            const ui::Element* el = r.try_get<ui::Element>(e);
            return glm::vec4(el ? el->Opacity : 1.0f, 0, 0, 0);
        };
        p.Set = [](entt::registry& r, entt::entity e, const glm::vec4& v) {
            if (ui::Element* el = r.try_get<ui::Element>(e)) el->Opacity = glm::clamp(v.x, 0.0f, 1.0f);
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
    RegisterSceneProperties();
    RegisterElementProperties();
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

PropertyAccess ResolveAccess(const PropertyType& p) {
    PropertyAccess a;
    a.Type = &p;
    if (p.Get) return a;
    std::string partId, fieldKey;
    SplitId(p.Id, partId, fieldKey);
    const FieldRef ref = ResolveField(partId, fieldKey);
    a.Part = ref.Part;
    a.Field = ref.Field;
    if (!a.Part || !a.Field) a.Type = nullptr;   // ключ части не нашёлся — писать некуда
    return a;
}

bool Read(const PropertyAccess& a, const entt::registry& reg, entt::entity e, glm::vec4& out) {
    if (!a.Type || !reg.valid(e)) return false;
    if (a.Type->Get) {
        if (a.Type->Has && !a.Type->Has(reg, e)) return false;
        out = a.Type->Get(reg, e);
        return true;
    }
    if (!a.Part->Has || !a.Part->Has(reg, e)) return false;
    const void* data = a.Part->Get(reg, e);
    if (!data) return false;
    out = ReadField(data, *a.Field);
    return true;
}

bool Write(const PropertyAccess& a, entt::registry& reg, entt::entity e, const glm::vec4& value) {
    if (!a.Type || !reg.valid(e)) return false;
    if (a.Type->Set) {
        if (a.Type->Has && !a.Type->Has(reg, e)) return false;
        a.Type->Set(reg, e, value);
        return true;
    }
    if (!a.Part->Has || !a.Part->Has(reg, e)) return false;
    void* data = a.Part->GetMutable(reg, e);
    if (!data) return false;
    WriteField(data, *a.Field, value);
    return true;
}

std::vector<const PropertyType*> PropertiesFor(const entt::registry& reg, entt::entity e) {
    std::vector<const PropertyType*> out;
    for (const PropertyType& p : Properties()) {
        if (HasProperty(p, reg, e)) out.push_back(&p);
    }
    return out;
}

} // namespace sage::anim
