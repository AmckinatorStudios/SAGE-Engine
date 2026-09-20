#include "sage/scripting/lua/LuaInternal.h"

#include <vector>

#include "sage/audio/AudioEngine.h"
#include "sage/core/Log.h"
#include "sage/input/InputSystem.h"
#include "sage/input/Keys.h"
#include "sage/physics/PhysicsScene.h"
#include "sage/scene/Components.h"
#include "sage/scene/Prefab.h"
#include "sage/scene/Scene.h"

// ---------------------------------------------------------------------------
// РАЗДЕЛЫ API: Input, Time, Debug, Scene, Physics, Events, Audio.
//
// Не сотня глобальных функций, а несколько именованных разделов — иначе
// подсказка редактора превращается в список несвязанных имён, а игра,
// объявившая свою функцию Raycast, молча затирает движковую.
//
// ЗВАТЬ МОЖНО И ЧЕРЕЗ ТОЧКУ, И ЧЕРЕЗ ДВОЕТОЧИЕ. `Input:IsKeyDown("W")` и
// `Input.IsKeyDown("W")` — одно и то же: разница между ними в Lua есть, а в
// голове у пишущего игру её нет, и ошибка «вызвал не тем знаком» выглядит как
// «движок не работает».
// ---------------------------------------------------------------------------
namespace sage::scripting::lua {

namespace {

// Сдвиг аргументов: при вызове через двоеточие первым приходит сам раздел.
size_t Skip(const sol::variadic_args& va) {
    if (va.size() == 0) return 0;
    const sol::type t = va[0].get_type();
    return t == sol::type::table ? 1u : 0u;
}

std::string Str(const sol::variadic_args& va, size_t i) {
    if (i >= va.size()) return {};
    sol::object o = va[i];
    return o.is<std::string>() ? o.as<std::string>() : std::string();
}

float Num(const sol::variadic_args& va, size_t i, float fallback = 0.0f) {
    if (i >= va.size()) return fallback;
    sol::object o = va[i];
    return o.is<float>() ? o.as<float>() : fallback;
}

sage::input::InputSystem& NeedInput(Backend& b, const char* who) {
    sage::input::InputSystem* in = b.Services().Input;
    if (!in) throw std::runtime_error(std::string(who) + ": ввод не привязан");
    return *in;
}

} // namespace

void RegisterGlobals(Backend& backend) {
    sol::state& lua = backend.Lua();
    Backend* self = &backend;

    // =======================================================================
    //  Input
    // =======================================================================
    sol::table input = lua.create_table();
    // «Сырая» клавиша — для прототипа и отладки. Игра, которую можно
    // переназначить, спрашивает ДЕЙСТВИЕ (IsActionDown): клавиша, зашитая в
    // скрипт, не меняется ни настройками, ни геймпадом.
    input.set_function("IsKeyDown", [self](sol::variadic_args va) {
        const std::string name = Str(va, Skip(va));
        const sage::input::Key key = sage::input::ParseKey(name);
        if (key == sage::input::Key::Unknown) return false;
        return NeedInput(*self, "IsKeyDown").State().Keys().Down(key);
    });
    input.set_function("IsKeyPressed", [self](sol::variadic_args va) {
        const std::string name = Str(va, Skip(va));
        const sage::input::Key key = sage::input::ParseKey(name);
        if (key == sage::input::Key::Unknown) return false;
        return NeedInput(*self, "IsKeyPressed").State().Keys().Pressed(key);
    });
    input.set_function("IsMouseButtonDown", [self](sol::variadic_args va) {
        bool ok = false;
        const sage::input::MouseButton b = sage::input::ParseMouseButton(Str(va, Skip(va)), &ok);
        if (!ok) return false;
        return NeedInput(*self, "IsMouseButtonDown").State().MouseState().Down(b);
    });
    input.set_function("IsMouseButtonPressed", [self](sol::variadic_args va) {
        bool ok = false;
        const sage::input::MouseButton b = sage::input::ParseMouseButton(Str(va, Skip(va)), &ok);
        if (!ok) return false;
        return NeedInput(*self, "IsMouseButtonPressed").State().MouseState().Pressed(b);
    });
    input.set_function("GetMouseDelta", [self](sol::variadic_args) {
        return NeedInput(*self, "GetMouseDelta").MouseDelta();
    });
    input.set_function("GetMousePosition", [self](sol::variadic_args) {
        return NeedInput(*self, "GetMousePosition").MousePosition();
    });
    input.set_function("GetMouseWheel", [self](sol::variadic_args) {
        return NeedInput(*self, "GetMouseWheel").Wheel();
    });
    // --- Действия: смысл придумывает игра, движок только отвечает -----------
    input.set_function("IsActionDown", [self](sol::variadic_args va) {
        return NeedInput(*self, "IsActionDown").IsDown(Str(va, Skip(va)));
    });
    input.set_function("IsActionPressed", [self](sol::variadic_args va) {
        return NeedInput(*self, "IsActionPressed").WasPressed(Str(va, Skip(va)));
    });
    input.set_function("IsActionReleased", [self](sol::variadic_args va) {
        return NeedInput(*self, "IsActionReleased").WasReleased(Str(va, Skip(va)));
    });
    input.set_function("GetAxis", [self](sol::variadic_args va) {
        return NeedInput(*self, "GetAxis").Value(Str(va, Skip(va)));
    });
    input.set_function("GetVector2", [self](sol::variadic_args va) {
        return NeedInput(*self, "GetVector2").Vector(Str(va, Skip(va)));
    });
    // Раскладку объявляет САМА ИГРА: «Jump на пробеле» — правило игры, а не
    // движка, и зашивать его в C++ значило бы иметь по движку на игру.
    input.set_function("BindAction", [self](sol::variadic_args va) {
        const size_t b = Skip(va);
        sage::input::InputSystem& in = NeedInput(*self, "BindAction");
        const std::string action = Str(va, b);
        if (action.empty()) return false;
        in.Register(action);
        return in.AddBinding(action, Str(va, b + 1));
    });
    input.set_function("SetCursorCaptured", [self](sol::variadic_args va) {
        const size_t b = Skip(va);
        bool on = true;
        if (b < va.size()) on = va[b].as<bool>();
        NeedInput(*self, "SetCursorCaptured").SetCursorCaptured(on);
    });
    input.set_function("IsCursorCaptured", [self](sol::variadic_args) {
        return NeedInput(*self, "IsCursorCaptured").CursorCaptured();
    });
    lua["Input"] = input;

    // =======================================================================
    //  Time — читается как поле, а не зовётся как функция
    // =======================================================================
    sol::table time = lua.create_table();
    sol::table timeMeta = lua.create_table();
    timeMeta[sol::meta_function::index] = [self](sol::table, const std::string& key) -> sol::object {
        ScriptClock* clock = self->Clock();
        if (!clock) return sol::nil;
        sol::state& L = self->Lua();
        if (key == "deltaTime") return sol::make_object(L, clock->Delta);
        if (key == "fixedDeltaTime") return sol::make_object(L, clock->FixedDelta);
        if (key == "time") return sol::make_object(L, clock->Time);
        if (key == "unscaledTime") return sol::make_object(L, clock->Unscaled);
        if (key == "timeScale") return sol::make_object(L, clock->Scale);
        return sol::nil;
    };
    // Масштаб времени ПИШЕТСЯ: замедление — обычный приём игры, и делать ради
    // него отдельную функцию, когда есть поле, незачем.
    timeMeta[sol::meta_function::new_index] = [self](sol::table, const std::string& key,
                                                     sol::object value) {
        ScriptClock* clock = self->Clock();
        if (!clock || key != "timeScale" || !value.is<float>()) return;
        const float scale = value.as<float>();
        clock->Scale = scale < 0.0f ? 0.0f : scale;
    };
    time[sol::metatable_key] = timeMeta;
    lua["Time"] = time;

    // =======================================================================
    //  Debug
    // =======================================================================
    sol::table debug = lua.create_table();
    debug.set_function("Log", [](sol::variadic_args va) {
        LOG_INFO("Lua") << Str(va, Skip(va));
    });
    debug.set_function("Warning", [](sol::variadic_args va) {
        LOG_WARN("Lua") << Str(va, Skip(va));
    });
    debug.set_function("Error", [](sol::variadic_args va) {
        LOG_ERROR("Lua") << Str(va, Skip(va));
    });
    // Отладочные линии: рисовать их некому, пока сцена не в кадре (headless,
    // тест, сборка без рендера). Это не ошибка — вызов просто ничего не
    // делает, иначе отладочная строка роняла бы игру там, где она безобидна.
    debug.set_function("DrawLine", [](sol::variadic_args) {});
    debug.set_function("DrawRay", [](sol::variadic_args) {});
    debug.set_function("DrawSphere", [](sol::variadic_args) {});
    lua["Debug"] = debug;

    // =======================================================================
    //  Scene
    // =======================================================================
    sol::table scene = lua.create_table();
    scene.set_function("Find", [self](sol::variadic_args va) -> sol::object {
        Scene* s = SceneOrThrow(*self, "scene:Find");
        GameObject found = s->FindByName(Str(va, Skip(va)));
        return found.Valid() ? self->Wrap(found) : sol::nil;
    });
    scene.set_function("FindById", [self](sol::variadic_args va) -> sol::object {
        Scene* s = SceneOrThrow(*self, "scene:FindById");
        GameObject found = s->Get((int)Num(va, Skip(va)));
        return found.Valid() ? self->Wrap(found) : sol::nil;
    });
    scene.set_function("FindByTag", [self](sol::variadic_args va) -> sol::object {
        Scene* s = SceneOrThrow(*self, "scene:FindByTag");
        const std::string tag = Str(va, Skip(va));
        auto view = s->Registry().view<TagComponent>();
        for (auto e : view)
            if (view.get<TagComponent>(e).Tag == tag)
                return self->Wrap(GameObject(&s->Registry(), e));
        return sol::nil;
    });
    scene.set_function("FindAllByTag", [self](sol::variadic_args va) {
        Scene* s = SceneOrThrow(*self, "scene:FindAllByTag");
        const std::string tag = Str(va, Skip(va));
        sol::table list = self->Lua().create_table();
        int i = 1;
        auto view = s->Registry().view<TagComponent>();
        for (auto e : view)
            if (view.get<TagComponent>(e).Tag == tag)
                list[i++] = self->Wrap(GameObject(&s->Registry(), e));
        return list;
    });
    scene.set_function("Create", [self](sol::variadic_args va) {
        Scene* s = SceneOrThrow(*self, "scene:Create");
        const std::string name = Str(va, Skip(va));
        return self->Wrap(s->CreateObject(name.empty() ? "Object" : name));
    });
    // Ставит префаб. Точка задаётся СРАЗУ: между «поставить» и «подвинуть»
    // проходит кадр, и объект на мгновение появляется в начале координат —
    // это видно как мигание.
    scene.set_function("Instantiate", [self](sol::variadic_args va) -> sol::object {
        Scene* s = SceneOrThrow(*self, "scene:Instantiate");
        const size_t b = Skip(va);
        const std::string path = Str(va, b);
        if (path.empty()) return sol::nil;
        int id = -1;
        if (b + 1 < va.size() && va[b + 1].is<glm::vec3>())
            id = sage::scene::InstantiatePrefabAt(*s, path, va[b + 1].as<glm::vec3>());
        else
            id = sage::scene::InstantiatePrefab(*s, path);
        if (id < 0) return sol::nil;
        return self->Wrap(s->Get(id));
    });
    scene.set_function("Destroy", [self](sol::variadic_args va) {
        Scene* s = SceneOrThrow(*self, "scene:Destroy");
        const size_t b = Skip(va);
        if (b >= va.size()) return;
        sol::object target = va[b];
        if (target.is<ObjectRef>()) {
            ObjectRef& r = target.as<ObjectRef>();
            if (r.Obj.Valid()) s->RemoveObject(r.Obj.Id());
        } else if (target.is<float>()) {
            s->RemoveObject((int)target.as<float>());
        }
    });
    scene.set_function("Name", [self](sol::variadic_args) {
        return SceneOrThrow(*self, "scene:Name")->Name();
    });
    // Прежний раздел sage.scene (Load и прочее) остаётся доступен ЧЕРЕЗ ЭТОТ
    // ЖЕ объект: имя `scene` в скриптах уже занято им, и отобрать его значило
    // бы сломать все существующие игры ради одной буквы.
    sol::object previous = lua["scene"];
    if (previous.get_type() == sol::type::table) {
        sol::table fallback = previous.as<sol::table>();
        sol::table meta = lua.create_table();
        meta[sol::meta_function::index] = fallback;
        scene[sol::metatable_key] = meta;
    }
    lua["Scene"] = scene;
    lua["scene"] = scene;

    // =======================================================================
    //  Physics
    // =======================================================================
    sol::table physics = lua.create_table();
    physics.set_function("Raycast", [self](sol::variadic_args va) -> sol::object {
        PhysicsScene* ps = self->Services().Physics;
        if (!ps) throw std::runtime_error("Physics:Raycast: физика не привязана");
        const size_t b = Skip(va);
        if (b + 1 >= va.size()) return sol::nil;
        const glm::vec3 origin = va[b].as<glm::vec3>();
        const glm::vec3 dir = va[b + 1].as<glm::vec3>();
        const float maxDistance = Num(va, b + 2, 1000.0f);
        const PhysicsScene::EntityHit hit = ps->Raycast(origin, dir, maxDistance);
        if (!hit.Hit) return sol::nil;
        sol::table out = self->Lua().create_table();
        Scene* s = self->ScenePtr();
        out["object"] = s ? self->Wrap(GameObject(&s->Registry(), hit.Entity)) : sol::nil;
        out["point"] = hit.Point;
        out["normal"] = hit.Normal;
        out["distance"] = hit.Distance;
        return out;
    });
    physics.set_function("OverlapSphere", [self](sol::variadic_args va) {
        PhysicsScene* ps = self->Services().Physics;
        if (!ps) throw std::runtime_error("Physics:OverlapSphere: физика не привязана");
        const size_t b = Skip(va);
        sol::table list = self->Lua().create_table();
        if (b >= va.size()) return list;
        std::vector<entt::entity> found;
        ps->OverlapSphere(va[b].as<glm::vec3>(), Num(va, b + 1, 1.0f), found);
        Scene* s = self->ScenePtr();
        if (!s) return list;
        int i = 1;
        for (entt::entity e : found) list[i++] = self->Wrap(GameObject(&s->Registry(), e));
        return list;
    });
    physics.set_function("SetGravity", [self](sol::variadic_args va) {
        PhysicsScene* ps = self->Services().Physics;
        if (!ps) throw std::runtime_error("Physics:SetGravity: физика не привязана");
        const size_t b = Skip(va);
        if (b < va.size() && va[b].is<glm::vec3>()) ps->SetGravity(va[b].as<glm::vec3>());
    });
    lua["Physics"] = physics;

    // =======================================================================
    //  Events — одна шина на всех: кнопка интерфейса, скрипт и код на C++
    // =======================================================================
    sol::table events = lua.create_table();
    events.set_function("Emit", [self](sol::variadic_args va) {
        Scene* s = SceneOrThrow(*self, "Events:Emit");
        const size_t b = Skip(va);
        const std::string name = Str(va, b);
        if (name.empty()) return;
        sage::vars::Value arg;
        if (b + 1 < va.size()) arg = self->FromLua(va[b + 1]);
        s->Events.Emit(name, arg);
    });
    events.set_function("On", [self](sol::variadic_args va) {
        Scene* s = SceneOrThrow(*self, "Events:On");
        const size_t b = Skip(va);
        const std::string name = Str(va, b);
        if (name.empty() || b + 1 >= va.size()) return 0;
        sol::protected_function fn = va[b + 1].as<sol::protected_function>();
        Backend* backend = self;
        return s->Events.On(name, [backend, fn](const sage::events::Event& e) {
            sol::protected_function_result r = fn(backend->ToLua(e.Arg));
            if (!r.valid()) {
                sol::error err = r;
                LOG_ERROR("Lua") << "обработчик события " << e.Name << ": " << err.what();
            }
        });
    });
    events.set_function("Once", [self](sol::variadic_args va) {
        Scene* s = SceneOrThrow(*self, "Events:Once");
        const size_t b = Skip(va);
        const std::string name = Str(va, b);
        if (name.empty() || b + 1 >= va.size()) return 0;
        sol::protected_function fn = va[b + 1].as<sol::protected_function>();
        Backend* backend = self;
        return s->Events.Once(name, [backend, fn](const sage::events::Event& e) {
            sol::protected_function_result r = fn(backend->ToLua(e.Arg));
            if (!r.valid()) {
                sol::error err = r;
                LOG_ERROR("Lua") << "обработчик события " << e.Name << ": " << err.what();
            }
        });
    });
    events.set_function("Off", [self](sol::variadic_args va) {
        Scene* s = SceneOrThrow(*self, "Events:Off");
        const size_t b = Skip(va);
        s->Events.Off((int)Num(va, b));
    });
    lua["Events"] = events;

    // =======================================================================
    //  Audio — звук БЕЗ объекта: щелчок интерфейса, взрыв в точке мира
    // =======================================================================
    sol::table audio = lua.create_table();
    audio.set_function("Play", [self](sol::variadic_args va) {
        AudioEngine* engine = self->Services().Audio;
        if (!engine) throw std::runtime_error("Audio.Play: звук не привязан");
        const size_t b = Skip(va);
        engine->PlaySound2D(Str(va, b), Num(va, b + 1, 1.0f));
    });
    audio.set_function("PlayAt", [self](sol::variadic_args va) {
        AudioEngine* engine = self->Services().Audio;
        if (!engine) throw std::runtime_error("Audio.PlayAt: звук не привязан");
        const size_t b = Skip(va);
        if (b + 1 >= va.size()) return;
        engine->PlaySound3D(Str(va, b), va[b + 1].as<glm::vec3>(), Num(va, b + 2, 1.0f));
    });
    audio.set_function("SetMasterVolume", [self](sol::variadic_args va) {
        AudioEngine* engine = self->Services().Audio;
        if (!engine) throw std::runtime_error("Audio.SetMasterVolume: звук не привязан");
        engine->SetMasterVolume(Num(va, Skip(va), 1.0f));
    });
    // Имя `audio` могло быть занято прежним разделом — не трогаем его: старые
    // игры зовут audio.PlaySound, и отобрать имя значит сломать их молча.
    lua["Audio"] = audio;
}

// --- Перевод значений движка в Lua и обратно ---------------------------------
//
// Один на весь бэкенд: публичные поля, аргументы событий и вызовы чужих
// скриптов ходят одной дорогой. Три копии этого перевода разошлись бы на
// первом же новом виде значения — и молча.
sol::object Backend::ToLua(const sage::vars::Value& value) {
    using K = sage::vars::Kind;
    switch (value.Type()) {
        case K::Bool: return sol::make_object(*m_lua, value.AsBool());
        case K::Int: return sol::make_object(*m_lua, value.AsInt());
        case K::Float: return sol::make_object(*m_lua, value.AsFloat());
        case K::String: return sol::make_object(*m_lua, value.AsString());
        case K::Vec2: return sol::make_object(*m_lua, value.AsVec2());
        case K::Vec3: return sol::make_object(*m_lua, value.AsVec3());
        case K::Color: return sol::make_object(*m_lua, value.AsVec4());
        case K::Entity: {
            // Ссылка отдаётся СРАЗУ объектом, а не номером: номер сущности
            // человек не знает и знать не должен, а скрипт с номером обязан
            // искать объект вручную при каждом обращении.
            Scene* scene = m_services.ScenePtr;
            const sage::vars::EntityRef ref = value.AsEntity();
            if (!scene || !ref.Valid()) return sol::nil;
            return Wrap(scene->Get(ref.Id));
        }
        case K::Asset: return sol::make_object(*m_lua, value.AsAsset().Path);
    }
    return sol::nil;
}

sage::vars::Value Backend::FromLua(const sol::object& object) {
    using namespace sage::vars;
    if (!object.valid()) return Value();
    if (object.is<bool>()) return Value(object.as<bool>());
    if (object.is<std::string>()) return Value(object.as<std::string>());
    if (object.is<glm::vec2>()) return Value(object.as<glm::vec2>());
    if (object.is<glm::vec3>()) return Value(object.as<glm::vec3>());
    if (object.is<glm::vec4>()) return Value(object.as<glm::vec4>());
    if (object.is<ObjectRef>()) {
        const ObjectRef& r = object.as<ObjectRef>();
        EntityRef ref;
        if (r.Obj.Valid()) ref.Id = r.Obj.Id();
        return Value(ref);
    }
    if (object.is<double>()) {
        const double d = object.as<double>();
        // Целое остаётся целым: число 3, ставшее 3.0, показывается в
        // инспекторе ползунком с дробью — и человек видит не то, что записал.
        if (d == (double)(int)d) return Value((int)d);
        return Value((float)d);
    }
    return Value();
}

} // namespace sage::scripting::lua
