#include "ScriptApiCommon.h"

#include "sage/core/Log.h"
#include "sage/scene/Scene.h"
#include "sage/scripting/ScriptComponent.h"
#include "sage/vars/VarsComponent.h"

// ---------------------------------------------------------------------------
// Публичные переменные и ссылки в скриптах: obj.Vars.speed, obj.Vars.target
//
// ЧЕМ ЭТО ОТЛИЧАЕТСЯ ОТ ПРОСТО ПОЛЯ В СКРИПТЕ. Число, вписанное в скрипт,
// одинаково у всех объектов с этим скриптом: две двери с разной скоростью —
// это два скрипта. Публичная переменная принадлежит ОБЪЕКТУ: скрипт один, а
// значение у каждой двери своё, видно в инспекторе и лежит в сцене.
//
// Ссылка (Vars.target) отдаётся сразу ОБЪЕКТОМ, а не номером: скрипт пишет
// `Vars.target:SetPosition(...)`, а не ищет сущность по номеру у сцены. Ищет
// прокси — и, что важнее, ссылка держится за Id, а не за имя: переименование
// объекта в редакторе больше не ломает уровень молча.
//
// ДОСТУП ИМЕННО ТАБЛИЦЕЙ, а не функциями Get/Set. Разница не в красоте:
// `Vars.speed = Vars.speed + 1` читается сразу, а
// `SetVar(o, "speed", GetVar(o, "speed") + 1)` — нет, и во второй записи легко
// перепутать объект в одном из двух вызовов.
// ---------------------------------------------------------------------------
namespace {

// Дескриптор «переменные вот этого объекта». Хранит сущность, а не указатель
// на таблицу: компонент могут добавить и удалить, а вектор внутри — переехать
// при добавлении переменной, и сохранённый указатель протух бы молча.
struct VarsProxy {
    entt::registry* Reg = nullptr;
    entt::entity Entity = entt::null;
    Scene* Owner = nullptr;

    bool Valid() const { return Reg && Reg->valid(Entity); }

    // ДВА ХРАНИЛИЩА, ОДИН ВИД СНАРУЖИ.
    //
    // Переменные, ОБЪЯВЛЕННЫЕ скриптом, живут в самом компоненте скрипта: они
    // принадлежат паре «объект + скрипт» и исчезают вместе с ним. Переменные,
    // заведённые человеком объекту БЕЗ скрипта (точка появления, зона,
    // кнопка), живут в VarsComponent. Для скрипта это одно и то же имя, и
    // разделять их в его глазах значило бы заставить помнить, кто где.
    sage::vars::Table* Fields() const {
        if (!Valid()) return nullptr;
        ScriptComponent* sc = Reg->try_get<ScriptComponent>(Entity);
        return sc ? &sc->Fields : nullptr;
    }
    sage::vars::Table* Own(bool create) const {
        if (!Valid()) return nullptr;
        if (create) return &Reg->get_or_emplace<VarsComponent>(Entity).Values;
        VarsComponent* c = Reg->try_get<VarsComponent>(Entity);
        return c ? &c->Values : nullptr;
    }
    // Где лежит ЭТА переменная. Объявленная скриптом — сильнее: инспектор
    // показывает её рядом с файлом, и писать её надо туда же, иначе значение
    // из скрипта и значение в инспекторе разойдутся молча.
    sage::vars::Table* For(const std::string& name, bool create) const {
        if (sage::vars::Table* f = Fields())
            if (f->Find(name)) return f;
        if (sage::vars::Table* o = Own(false))
            if (o->Find(name)) return o;
        return create ? Own(true) : nullptr;
    }
};

} // namespace

// Значение движка -> значение Lua. Ссылка на объект превращается в САМ объект:
// отдавать номер значило бы заставить каждый скрипт искать по нему сущность.
sol::object ScriptEngine::ValueToLua(const sage::vars::Value& v) {
    using K = sage::vars::Kind;
    switch (v.Type()) {
        case K::Bool: return sol::make_object(m_lua, v.AsBool());
        case K::Int: return sol::make_object(m_lua, v.AsInt());
        case K::Float: return sol::make_object(m_lua, v.AsFloat());
        case K::String: return sol::make_object(m_lua, v.AsString());
        case K::Vec2: return sol::make_object(m_lua, v.AsVec2());
        case K::Vec3: return sol::make_object(m_lua, v.AsVec3());
        case K::Color: return sol::make_object(m_lua, v.AsVec4());
        case K::Asset: return sol::make_object(m_lua, v.AsAsset().Path);
        case K::Entity: {
            const int id = v.AsEntity().Id;
            if (!m_scene || id <= 0) return sol::nil;
            GameObject obj = m_scene->Get(id);
            // Ссылка на удалённый объект — nil, а не «объект, у которого всё
            // падает»: скрипт обязан уметь проверить её обычным `if`.
            if (!obj.Valid()) return sol::nil;
            return sol::make_object(m_lua, obj);
        }
    }
    return sol::nil;
}

// Значение Lua -> значение движка. Вид берётся у СУЩЕСТВУЮЩЕЙ переменной, если
// она есть: скрипт, присвоивший «5» переменной-цвету, ошибся, и менять из-за
// этого тип переменной в инспекторе — значит спрятать ошибку.
sage::vars::Value ScriptEngine::ValueFromLua(const sol::object& o) {
    if (!o.valid() || o == sol::nil) return sage::vars::Value();
    if (o.is<bool>()) return sage::vars::Value(o.as<bool>());
    if (o.is<GameObject>()) {
        GameObject obj = o.as<GameObject>();
        return sage::vars::Value(sage::vars::EntityRef{obj.Valid() ? obj.Id() : 0});
    }
    if (o.is<glm::vec2>()) return sage::vars::Value(o.as<glm::vec2>());
    if (o.is<glm::vec3>()) return sage::vars::Value(o.as<glm::vec3>());
    if (o.is<glm::vec4>()) return sage::vars::Value(o.as<glm::vec4>());
    if (o.is<std::string>()) return sage::vars::Value(o.as<std::string>());
    if (o.is<int>() && o.get_type() == sol::type::number) {
        // Lua не различает целое и дробное; целым считаем только то, что и
        // правда целое, — иначе 0.5 превратился бы в ноль.
        const double d = o.as<double>();
        if (d == (double)(int)d) return sage::vars::Value((int)d);
    }
    if (o.is<float>()) return sage::vars::Value(o.as<float>());
    return sage::vars::Value();
}

void ScriptEngine::RegisterVarsApi() {
    m_lua.new_usertype<VarsProxy>(
        "VarsProxy", sol::no_constructor,
        sol::meta_function::index,
        [this](VarsProxy& p, const std::string& name) -> sol::object {
            const sage::vars::Table* t = p.For(name, false);
            if (!t) return sol::nil;
            const sage::vars::Var* v = t->Find(name);
            return v ? ValueToLua(v->Data) : sol::nil;
        },
        sol::meta_function::new_index,
        [this](VarsProxy& p, const std::string& name, sol::object value) {
            sage::vars::Table* t = p.For(name, true);
            if (!t) return;
            t->Set(name, ValueFromLua(value));
        },
        sol::meta_function::length,
        [](VarsProxy& p) -> int {
            const sage::vars::Table* fields = p.Fields();
            const sage::vars::Table* own = p.Own(false);
            return (fields ? (int)fields->Size() : 0) + (own ? (int)own->Size() : 0);
        });

    // Точка входа: obj:Vars(). Метод, а не поле, потому что поле usertype
    // отдавалось бы копией — и присваивание в неё молча пропадало.
    sol::usertype<GameObject> go = m_lua["GameObject"];
    go["Vars"] = [this](GameObject& o) -> VarsProxy {
        if (!o.Valid()) throw std::runtime_error("Vars: сущность недействительна");
        return VarsProxy{o.Registry(), o.Entity(), m_scene};
    };

    // Список имён — для инструментов и отладки: «какие настройки у этого
    // объекта вообще есть» иначе можно узнать только чтением сцены глазами.
    go["VarNames"] = [this](GameObject& o) -> sol::table {
        sol::table out = m_lua.create_table();
        if (!o.Valid()) return out;
        int i = 1;
        if (const ScriptComponent* sc = o.Registry()->try_get<ScriptComponent>(o.Entity()))
            for (const sage::vars::Var& v : sc->Fields.All()) out[i++] = v.Name;
        if (const VarsComponent* c = o.Registry()->try_get<VarsComponent>(o.Entity()))
            for (const sage::vars::Var& v : c->Values.All()) out[i++] = v.Name;
        return out;
    };

    // Есть ли переменная — отдельно от чтения: nil как значение законен, и
    // отличить «нет переменной» от «переменная равна nil» иначе нечем.
    go["HasVar"] = [](GameObject& o, const std::string& name) -> bool {
        if (!o.Valid()) return false;
        if (const ScriptComponent* sc = o.Registry()->try_get<ScriptComponent>(o.Entity()))
            if (sc->Fields.Find(name)) return true;
        const VarsComponent* c = o.Registry()->try_get<VarsComponent>(o.Entity());
        return c && c->Values.Find(name) != nullptr;
    };
}
