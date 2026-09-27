#include "sage/scripting/lua/LuaInternal.h"

#include <algorithm>

#include "sage/core/Log.h"
#include "sage/scripting/ScriptTween.h"

// ---------------------------------------------------------------------------
// ТВИНЫ ИЗ LUA.
//
//     Tween.to(self, "position", Vector3.new(5, 2, 0), 1.0, Ease.Out)
//     Tween.to(button, "opacity", 0, 0.25)
//     Tween.to(self, {position = p, scale = 1.2}, 0.5, Ease.OutBack)
//         :then_to("scale", 1.0, 0.2)       -- затем
//         :with("rotation", Vector3.new(0, 180, 0), 0.2)   -- вместе с ним
//         :wait(0.3)                        -- пауза
//         :on_complete(function() Debug.log("done") end)
//
// Всё это собирает ОДИН TweenClip — то же описание, которое правит окно Tween
// редактора, — и отдаёт его проигрывателю сцены. Цепочка дописывает шаги в
// уже запущенный твин: сборка идёт в том же кадре, до его первого шага.
// ---------------------------------------------------------------------------
namespace sage::scripting::lua {

namespace {

namespace tw = sage::scripting::tween;
using anim::TweenClip;
using anim::TweenTrack;

// Твин в руках скрипта: номер в проигрывателе и где продолжать цепочку.
struct TweenRef {
    anim::TweenHandle Id = anim::kNoTween;
    GameObject Target;
    float Cursor = 0.0f;     // где начнётся следующий шаг :then_to
    float LastStart = 0.0f;  // где начался последний шаг (для :with)
};

// Вызов через двоеточие (`Tween:to(...)`): первым приходит сама таблица
// Tween — её узнают по метке, а не по «это таблица»: self скрипта тоже таблица.
size_t Skip(const sol::variadic_args& va) {
    if (va.size() == 0 || va[0].get_type() != sol::type::table) return 0;
    sol::object mark = va[0].as<sol::table>().raw_get<sol::object>("__sage_tween");
    return mark.valid() && mark.get_type() == sol::type::boolean ? 1u : 0u;
}

// Кого двигать: объект, self скрипта (у него есть gameObject) или прежний
// GameObject старых скриптов.
GameObject TargetOf(const sol::object& o) {
    if (o.is<ObjectRef>()) return o.as<ObjectRef>().Obj;
    if (o.is<GameObject>()) return o.as<GameObject>();
    if (o.get_type() == sol::type::table) {
        sol::object go = o.as<sol::table>()["gameObject"];
        if (go.is<ObjectRef>()) return go.as<ObjectRef>().Obj;
    }
    return GameObject();
}

Scene& SceneOf(Backend& b, const char* who) {
    Scene* s = b.ScenePtr();
    if (!s) throw std::runtime_error(std::string(who) + ": сцена не привязана");
    return *s;
}

float Seconds(const sol::object& o, const char* who) {
    if (o.get_type() != sol::type::number)
        throw std::runtime_error(std::string(who) + ": длительность — число секунд");
    return std::max(o.as<float>(), 0.0f);
}

// Дорожка на одно свойство. Непонятное имя или значение — ошибка с именем:
// «ничего не произошло» при опечатке в «postion» не найти глазами.
TweenTrack MakeTrack(const GameObject& target, const std::string& name, const sol::object& value, float start,
                     float duration, const anim::Ease& ease, bool from, const char* who) {
    const std::string id = tw::ResolveProperty(*target.Registry(), target.Entity(), name);
    if (id.empty())
        throw std::runtime_error(std::string(who) + ": у объекта '" + target.Name() + "' нет свойства '" + name +
                                 "' (position, rotation, scale, color, opacity, size, intensity, fov, volume…)");
    const anim::PropertyType* p = anim::FindProperty(id);
    TweenTrack t;
    t.Property = id;
    glm::vec4 v(0.0f);
    if (!tw::ValueFrom(value, p ? p->Components : 1, v))
        throw std::runtime_error(std::string(who) + ": значение для '" + name + "' — число, Vector3 или Color");
    if (from) {
        t.From = v;
        t.FromCurrent = false;
        t.ToCurrent = true;
    } else {
        t.To = v;
    }
    t.Start = start;
    t.Duration = std::max(duration, 1e-4f);
    t.Curve = ease;
    return t;
}

// Шаг: одно свойство ("position", value, dur, ease) или несколько
// ({position = …, scale = …}, dur, ease). Все свойства шага — с одного
// момента. Возвращает длительность шага.
float AddStep(TweenClip& clip, const GameObject& target, sol::variadic_args va, size_t b, float start,
              bool from, const char* who) {
    if (b >= va.size()) throw std::runtime_error(std::string(who) + ": не указано свойство");
    sol::object what = va[b];
    const anim::Ease fallback = anim::Ease::Make(anim::EaseShape::Quad, anim::EaseMode::Out);
    if (what.get_type() == sol::type::string) {
        if (b + 2 >= va.size()) throw std::runtime_error(std::string(who) + ": нужны значение и длительность");
        const float dur = Seconds(va[b + 2], who);
        const anim::Ease ease = tw::EaseFrom(b + 3 < va.size() ? sol::object(va[b + 3]) : sol::object(), fallback);
        clip.Tracks.push_back(MakeTrack(target, what.as<std::string>(), va[b + 1], start, dur, ease, from, who));
        return dur;
    }
    if (what.get_type() == sol::type::table) {
        if (b + 1 >= va.size()) throw std::runtime_error(std::string(who) + ": нужна длительность");
        const float dur = Seconds(va[b + 1], who);
        const anim::Ease ease = tw::EaseFrom(b + 2 < va.size() ? sol::object(va[b + 2]) : sol::object(), fallback);
        // Порядок — по имени: у таблицы Lua порядка нет, а дорожки в
        // редакторе и повтор запуска должны выглядеть одинаково.
        std::vector<std::pair<std::string, sol::object>> props;
        for (auto& [k, v] : what.as<sol::table>())
            if (k.get_type() == sol::type::string) props.emplace_back(k.as<std::string>(), v);
        std::sort(props.begin(), props.end(), [](const auto& a, const auto& c) { return a.first < c.first; });
        for (auto& [name, value] : props)
            clip.Tracks.push_back(MakeTrack(target, name, value, start, dur, ease, from, who));
        return dur;
    }
    throw std::runtime_error(std::string(who) + ": свойство — имя (\"position\") или таблица {position = …}");
}

anim::TweenPlayer& PlayerOf(Backend& b) { return SceneOf(b, "Tween").Tweens; }

} // namespace

void Backend::SetTweenCallback(uint32_t tween, const sol::protected_function& fn) {
    if (!m_tweenCallbacks.valid()) m_tweenCallbacks = m_lua->create_table();
    m_tweenCallbacks[tween] = fn;
    Scene* scene = ScenePtr();
    if (!scene) return;
    std::weak_ptr<Backend*> token = m_tapToken;
    scene->Tweens.SetOnComplete(tween, [token, tween]() {
        std::shared_ptr<Backend*> alive = token.lock();
        if (alive && *alive) (*alive)->FireTweenCallback(tween);
    });
}

void Backend::FireTweenCallback(uint32_t tween) {
    if (!m_tweenCallbacks.valid()) return;
    sol::object fn = m_tweenCallbacks[tween];
    m_tweenCallbacks[tween] = sol::lua_nil;
    if (fn.get_type() != sol::type::function) return;
    sol::protected_function call = fn.as<sol::protected_function>();
    sol::protected_function_result r = call();
    if (r.valid()) return;
    ScriptError err;
    Explain(r, std::string(), err);
    LOG_ERROR("Lua") << err.Format() << " (Tween on_complete)";
}

void RegisterTweens(Backend& backend) {
    sol::state& lua = backend.Lua();
    Backend* self = &backend;

    // Ease — строки, общие с прежним API (см. ScriptTween.h).
    lua["Ease"] = tw::MakeEaseTable(lua);

    // --- Твин в руках скрипта: цепочка и управление ------------------------------
    sol::usertype<TweenRef> rt = lua.new_usertype<TweenRef>("SageTween", sol::no_constructor);
    auto clipOf = [self](TweenRef& r, const char* who) -> TweenClip& {
        TweenClip* c = PlayerOf(*self).Clip(r.Id);
        if (!c) throw std::runtime_error(std::string(who) + ": твин уже закончился или снят");
        return *c;
    };
    // Затем: шаг начинается там, где кончился предыдущий.
    auto thenTo = [self, clipOf](TweenRef& r, sol::variadic_args va) -> TweenRef& {
        TweenClip& clip = clipOf(r, "then_to");
        const float start = r.Cursor;
        const float dur = AddStep(clip, r.Target, va, 0, start, false, "then_to");
        r.LastStart = start;
        r.Cursor = start + dur;
        return r;
    };
    rt["then_to"] = thenTo;
    rt["to"] = thenTo;
    // Вместе: шаг начинается одновременно с предыдущим.
    rt["with"] = [self, clipOf](TweenRef& r, sol::variadic_args va) -> TweenRef& {
        TweenClip& clip = clipOf(r, "with");
        const float dur = AddStep(clip, r.Target, va, 0, r.LastStart, false, "with");
        r.Cursor = std::max(r.Cursor, r.LastStart + dur);
        return r;
    };
    // Пауза в последовательности. Пустая дорожка — «Delay» и в окне Tween:
    // пауза видна на шкале и держит длину твина (важно для повтора).
    rt["wait"] = [clipOf](TweenRef& r, float seconds) -> TweenRef& {
        TweenClip& clip = clipOf(r, "wait");
        TweenTrack gap;
        gap.Start = r.Cursor;
        gap.Duration = std::max(seconds, 0.0f);
        clip.Tracks.push_back(gap);
        r.LastStart = r.Cursor;
        r.Cursor += gap.Duration;
        return r;
    };
    rt["delay"] = [clipOf](TweenRef& r, float seconds) -> TweenRef& {
        clipOf(r, "delay").Delay = std::max(seconds, 0.0f);
        return r;
    };
    rt["loop"] = [clipOf](TweenRef& r, sol::optional<std::string> mode) -> TweenRef& {
        TweenClip& clip = clipOf(r, "loop");
        clip.Loop = anim::TweenLoop::Loop;
        if (mode && !anim::ParseLoop(*mode, clip.Loop))
            throw std::runtime_error("loop: режим — \"loop\", \"pingpong\" или \"once\"");
        return r;
    };
    rt["ping_pong"] = [clipOf](TweenRef& r) -> TweenRef& {
        clipOf(r, "ping_pong").Loop = anim::TweenLoop::PingPong;
        return r;
    };
    rt["speed"] = [clipOf](TweenRef& r, float k) -> TweenRef& {
        clipOf(r, "speed").Speed = std::max(k, 0.0f);
        return r;
    };
    rt["reverse"] = [clipOf](TweenRef& r, sol::optional<bool> on) -> TweenRef& {
        clipOf(r, "reverse").Reverse = on.value_or(true);
        return r;
    };
    // Кривая всех шагов разом: Tween.to(...):ease(Ease.OutBack).
    rt["ease"] = [clipOf](TweenRef& r, sol::object e) -> TweenRef& {
        TweenClip& clip = clipOf(r, "ease");
        for (TweenTrack& t : clip.Tracks) t.Curve = tw::EaseFrom(e, t.Curve);
        return r;
    };
    rt["on_complete"] = [self](TweenRef& r, sol::protected_function fn) -> TweenRef& {
        self->SetTweenCallback(r.Id, fn);
        return r;
    };
    rt["pause"] = [self](TweenRef& r) { return PlayerOf(*self).Pause(r.Id); };
    rt["resume"] = [self](TweenRef& r) { return PlayerOf(*self).Resume(r.Id); };
    rt["cancel"] = [self](TweenRef& r) { return PlayerOf(*self).Stop(r.Id); };
    rt["is_playing"] = [self](TweenRef& r) { return PlayerOf(*self).IsPlaying(r.Id); };
    rt["time"] = [self](TweenRef& r) { return PlayerOf(*self).TimeOf(r.Id); };
    rt["duration"] = [self](TweenRef& r) {
        const TweenClip* c = PlayerOf(*self).Clip(r.Id);
        return c ? c->Length() : 0.0f;
    };
    rt[sol::meta_function::to_string] = [](const TweenRef& r) { return "Tween #" + std::to_string(r.Id); };

    // --- Tween.* -----------------------------------------------------------------
    sol::table t = lua.create_table();
    t["__sage_tween"] = true;
    auto start = [self](sol::variadic_args va, bool from, const char* who) {
        const size_t b = Skip(va);
        if (b >= va.size()) throw std::runtime_error(std::string(who) + ": не указан объект");
        const GameObject target = TargetOf(va[b]);
        if (!target.Valid()) throw std::runtime_error(std::string(who) + ": первым — объект или self");
        Scene& scene = SceneOf(*self, who);
        TweenClip clip;
        clip.Name = "lua";
        TweenRef r;
        r.Target = target;
        r.Cursor = AddStep(clip, target, va, b + 1, 0.0f, from, who);
        r.Id = scene.Tweens.Play(scene.Registry(), target.Entity(), clip);
        return r;
    };
    t.set_function("to", [start](sol::variadic_args va) { return start(va, false, "Tween.to"); });
    t.set_function("from", [start](sol::variadic_args va) { return start(va, true, "Tween.from"); });
    // Пустая последовательность: шаги — цепочкой :then_to / :with / :wait.
    t.set_function("sequence", [self](sol::object target) {
        const GameObject o = TargetOf(target);
        if (!o.Valid()) throw std::runtime_error("Tween.sequence: первым — объект или self");
        Scene& scene = SceneOf(*self, "Tween.sequence");
        TweenClip clip;
        clip.Name = "lua";
        TweenRef r;
        r.Target = o;
        r.Id = scene.Tweens.Play(scene.Registry(), o.Entity(), clip);
        return r;
    });
    // Твин, собранный в окне Tween редактора (TweenComponent объекта).
    t.set_function("play", [self](sol::object target, const std::string& name) -> sol::object {
        const GameObject o = TargetOf(target);
        if (!o.Valid()) throw std::runtime_error("Tween.play: первым — объект или self");
        Scene& scene = SceneOf(*self, "Tween.play");
        const anim::TweenHandle h = anim::PlayNamed(scene, o.Entity(), name);
        if (h == anim::kNoTween) return sol::lua_nil;
        const TweenClip* c = scene.Tweens.Clip(h);
        TweenRef r;
        r.Id = h;
        r.Target = o;
        r.Cursor = c ? c->Length() : 0.0f;
        return sol::make_object(self->Lua(), r);
    });
    // Управление — твином или объектом (всеми его твинами; со свойством —
    // только теми, что ведут это свойство).
    t.set_function("cancel", [self](sol::object what, sol::optional<std::string> prop) {
        anim::TweenPlayer& p = PlayerOf(*self);
        if (what.is<TweenRef>()) return p.Stop(what.as<TweenRef>().Id) ? 1 : 0;
        const GameObject o = TargetOf(what);
        if (!o.Valid()) return 0;
        std::string id;
        if (prop) {
            id = tw::ResolveProperty(*o.Registry(), o.Entity(), *prop);
            if (id.empty()) return 0;
        }
        return p.StopFor(o.Entity(), id);
    });
    t.set_function("pause", [self](sol::object what) {
        anim::TweenPlayer& p = PlayerOf(*self);
        if (what.is<TweenRef>()) return p.Pause(what.as<TweenRef>().Id) ? 1 : 0;
        const GameObject o = TargetOf(what);
        return o.Valid() ? p.PauseFor(o.Entity(), true) : 0;
    });
    t.set_function("resume", [self](sol::object what) {
        anim::TweenPlayer& p = PlayerOf(*self);
        if (what.is<TweenRef>()) return p.Resume(what.as<TweenRef>().Id) ? 1 : 0;
        const GameObject o = TargetOf(what);
        return o.Valid() ? p.PauseFor(o.Entity(), false) : 0;
    });
    t.set_function("is_playing", [self](sol::object what) {
        anim::TweenPlayer& p = PlayerOf(*self);
        if (what.is<TweenRef>()) return p.IsPlaying(what.as<TweenRef>().Id);
        const GameObject o = TargetOf(what);
        return o.Valid() && p.AnyFor(o.Entity());
    });
    t.set_function("count", [self]() { return PlayerOf(*self).Count(); });
    lua["Tween"] = t;
}

} // namespace sage::scripting::lua
