#include "sage/scripting/lua/LuaInternal.h"

#include <cmath>

#include "sage/audio/AudioEngine.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"

// ---------------------------------------------------------------------------
// КОМПОНЕНТЫ ГЛАЗАМИ СКРИПТА: CharacterController, Animation, Audio, Camera.
//
//     local character = self:GetCharacterController()
//     character:Move(direction * speed)
//
// Каждый прокси — УНИВЕРСАЛЬНЫЙ: он умеет то, что вообще бывает у такого
// компонента, и ничего из того, что значит это в конкретной игре.
// `character:Move(...)` для движка — перемещение тела с разбором столкновений,
// а не «персонаж идёт»; что это значит, решает скрипт.
// ---------------------------------------------------------------------------
namespace sage::scripting::lua {

namespace {

template <typename C>
C* Comp(const GameObject& o) {
    return o.Valid() ? o.Registry()->try_get<C>(o.Entity()) : nullptr;
}

template <typename C>
C& Need(const GameObject& o, const char* who) {
    C* c = Comp<C>(o);
    if (!c) throw std::runtime_error(std::string(who) + ": компонента больше нет");
    return *c;
}

} // namespace

void RegisterComponents(Backend& backend) {
    sol::state& lua = backend.Lua();
    Backend* self = &backend;

    // --- CharacterController -------------------------------------------------
    sol::usertype<CharacterRef> ch = lua.new_usertype<CharacterRef>("SageCharacterController");

    // Move принимает СКОРОСТЬ (ед/с), а не смещение за кадр: умножать на dt в
    // каждом скрипте — значит однажды забыть и получить движение, зависящее от
    // частоты кадров. Вертикаль контроллер ведёт сам (тяготение, прыжок).
    ch["Move"] = [](CharacterRef& r, const glm::vec3& velocity) {
        CharacterControllerComponent& c = Need<CharacterControllerComponent>(r.Obj, "Move");
        c.DesiredVelocity = glm::vec3(velocity.x, 0.0f, velocity.z);
        c.HasRequest = true;
        c.Managed = true;
    };
    // Полная скорость, вертикаль включительно: полёт, плавание, отдача.
    ch["MoveVelocity"] = [](CharacterRef& r, const glm::vec3& velocity) {
        CharacterControllerComponent& c = Need<CharacterControllerComponent>(r.Obj, "MoveVelocity");
        c.DesiredVelocity = glm::vec3(velocity.x, 0.0f, velocity.z);
        c.VerticalVelocity = velocity.y;
        c.HasRequest = true;
        c.Managed = true;
    };
    // Прыжок задаётся ВЫСОТОЙ, а не скоростью: высота — то, что автор видит в
    // игре и может подобрать, а скорость зависит ещё и от тяготения.
    ch["Jump"] = [](CharacterRef& r, float height) {
        CharacterControllerComponent& c = Need<CharacterControllerComponent>(r.Obj, "Jump");
        const float g = std::abs(c.Gravity) > 1e-4f ? std::abs(c.Gravity) : 9.81f;
        c.VerticalVelocity = std::sqrt(2.0f * g * std::max(0.0f, height));
        c.Managed = true;
    };
    // Та же вертикальная скорость, но задаётся напрямую — для тех, кто считает
    // её сам (двойной прыжок с затуханием, отбрасывание взрывом).
    ch["SetJumpVelocity"] = [](CharacterRef& r, float velocity) {
        Need<CharacterControllerComponent>(r.Obj, "SetJumpVelocity").VerticalVelocity = velocity;
    };
    ch["SetGravity"] = [](CharacterRef& r, float gravity) {
        Need<CharacterControllerComponent>(r.Obj, "SetGravity").Gravity = gravity;
    };
    ch["GetGravity"] = [](CharacterRef& r) {
        return Need<CharacterControllerComponent>(r.Obj, "GetGravity").Gravity;
    };
    ch["IsGrounded"] = [](CharacterRef& r) {
        return Need<CharacterControllerComponent>(r.Obj, "IsGrounded").Grounded;
    };
    ch["GetVelocity"] = [](CharacterRef& r) {
        return Need<CharacterControllerComponent>(r.Obj, "GetVelocity").Velocity;
    };
    ch["GetGroundNormal"] = [](CharacterRef& r) {
        return Need<CharacterControllerComponent>(r.Obj, "GetGroundNormal").GroundNormal;
    };
    // Край шага — «приземлился», «оторвался», «упёрся». Считает контроллер, а
    // не игра по разнице флагов: край, вычисленный опросом раз в кадр,
    // теряется ровно на длинном кадре — и звук шага пропадает на просадке.
    ch["Landed"] = [](CharacterRef& r) {
        return Need<CharacterControllerComponent>(r.Obj, "Landed").Landed;
    };
    ch["LeftGround"] = [](CharacterRef& r) {
        return Need<CharacterControllerComponent>(r.Obj, "LeftGround").LeftGround;
    };
    ch["IsBlocked"] = [](CharacterRef& r) {
        return Need<CharacterControllerComponent>(r.Obj, "IsBlocked").Blocked;
    };
    ch["SetRadius"] = [](CharacterRef& r, float v) {
        Need<CharacterControllerComponent>(r.Obj, "SetRadius").Radius = v;
    };
    ch["SetHeight"] = [](CharacterRef& r, float v) {
        Need<CharacterControllerComponent>(r.Obj, "SetHeight").Height = v;
    };
    ch["SetSlopeLimit"] = [](CharacterRef& r, float v) {
        Need<CharacterControllerComponent>(r.Obj, "SetSlopeLimit").SlopeLimit = v;
    };
    ch["SetStepOffset"] = [](CharacterRef& r, float v) {
        Need<CharacterControllerComponent>(r.Obj, "SetStepOffset").StepOffset = v;
    };
    ch["Teleport"] = [](CharacterRef& r, const glm::vec3& position) {
        // Перенос мимо симуляции: контроллер и Transform обязаны оказаться в
        // одной точке, иначе следующий шаг вернёт персонажа обратно.
        CharacterControllerComponent& c = Need<CharacterControllerComponent>(r.Obj, "Teleport");
        if (Transform* tr = Comp<Transform>(r.Obj)) tr->Position = position;
        if (c.Motor) c.Motor->SetPosition(position + c.Center);
        c.Runtime = sage::physics::kInvalidCharacter; // пересоздать в нужном месте
        c.VerticalVelocity = 0.0f;
    };

    // --- Animation -----------------------------------------------------------
    sol::usertype<AnimatorRef> an = lua.new_usertype<AnimatorRef>("SageAnimation");
    an["Play"] = [](AnimatorRef& r, const std::string& clip, sol::optional<bool> loop) {
        AnimationComponent& a = Need<AnimationComponent>(r.Obj, "Play");
        a.Playing = true;
        a.Loop = loop.value_or(true);
        // Плавный переход, а не рывок: смена клипа рывком видна всегда, а
        // длительность перехода у компонента уже есть (BlendTime).
        if (a.BlendTime > 0.0f) return a.Anim.CrossFade(clip, a.BlendTime, a.Loop);
        return a.Anim.Play(clip, a.Loop);
    };
    an["Stop"] = [](AnimatorRef& r) {
        AnimationComponent& a = Need<AnimationComponent>(r.Obj, "Stop");
        a.Playing = false;
        a.Anim.Stop();
    };
    an["Resume"] = [](AnimatorRef& r) { Need<AnimationComponent>(r.Obj, "Resume").Playing = true; };
    an["SetSpeed"] = [](AnimatorRef& r, float s) {
        AnimationComponent& a = Need<AnimationComponent>(r.Obj, "SetSpeed");
        a.Speed = s;
        a.Anim.SetSpeed(s);
    };
    an["GetCurrentAnimation"] = [](AnimatorRef& r) {
        AnimationComponent& a = Need<AnimationComponent>(r.Obj, "GetCurrentAnimation");
        const int clip = a.Anim.CurrentClip();
        return clip >= 0 ? a.Anim.ClipName(clip) : std::string();
    };
    an["IsPlaying"] = [](AnimatorRef& r, sol::optional<std::string> clip) {
        AnimationComponent& a = Need<AnimationComponent>(r.Obj, "IsPlaying");
        if (!clip) return a.Playing;
        const int current = a.Anim.CurrentClip();
        return a.Playing && current >= 0 && a.Anim.ClipName(current) == *clip;
    };
    an["SetFloat"] = [](AnimatorRef& r, const std::string& name, float value) {
        Need<AnimationComponent>(r.Obj, "SetFloat").Params.Floats[name] = value;
    };
    an["GetFloat"] = [](AnimatorRef& r, const std::string& name) {
        AnimationComponent& a = Need<AnimationComponent>(r.Obj, "GetFloat");
        auto it = a.Params.Floats.find(name);
        return it == a.Params.Floats.end() ? 0.0f : it->second;
    };
    an["SetBool"] = [](AnimatorRef& r, const std::string& name, bool value) {
        Need<AnimationComponent>(r.Obj, "SetBool").Params.Bools[name] = value;
    };
    an["GetBool"] = [](AnimatorRef& r, const std::string& name) {
        AnimationComponent& a = Need<AnimationComponent>(r.Obj, "GetBool");
        auto it = a.Params.Bools.find(name);
        return it != a.Params.Bools.end() && it->second;
    };
    an["SetTrigger"] = [](AnimatorRef& r, const std::string& name) {
        Need<AnimationComponent>(r.Obj, "SetTrigger").Params.Triggers[name] = true;
    };
    // Прочитал — потребил: иначе «прыгнул» остаётся истиной навсегда.
    an["ConsumeTrigger"] = [](AnimatorRef& r, const std::string& name) {
        AnimationComponent& a = Need<AnimationComponent>(r.Obj, "ConsumeTrigger");
        auto it = a.Params.Triggers.find(name);
        if (it == a.Params.Triggers.end() || !it->second) return false;
        it->second = false;
        return true;
    };
    an["SetBlend"] = [](AnimatorRef& r, const std::string& name, float value) {
        Need<AnimationComponent>(r.Obj, "SetBlend").Params.Floats[name] = value;
    };
    an["SetLayerWeight"] = [](AnimatorRef& r, const std::string& layer, float weight) {
        Need<AnimationComponent>(r.Obj, "SetLayerWeight").Params.Layers[layer] = weight;
    };
    an["GetLayerWeight"] = [](AnimatorRef& r, const std::string& layer) {
        AnimationComponent& a = Need<AnimationComponent>(r.Obj, "GetLayerWeight");
        auto it = a.Params.Layers.find(layer);
        return it == a.Params.Layers.end() ? 0.0f : it->second;
    };

    // --- Audio ---------------------------------------------------------------
    sol::usertype<AudioRef> au = lua.new_usertype<AudioRef>("SageAudioSource");
    au["Play"] = [](AudioRef& r) { Need<AudioSourceComponent>(r.Obj, "Play").Play(); };
    au["Stop"] = [](AudioRef& r) { Need<AudioSourceComponent>(r.Obj, "Stop").Stop(); };
    au["Pause"] = [](AudioRef& r) { Need<AudioSourceComponent>(r.Obj, "Pause").Pause(); };
    au["Resume"] = [](AudioRef& r) { Need<AudioSourceComponent>(r.Obj, "Resume").Resume(); };
    au["IsPlaying"] = [](AudioRef& r) {
        const AudioSourceComponent& a = Need<AudioSourceComponent>(r.Obj, "IsPlaying");
        return a.Playing && !a.Paused;
    };
    au["SetVolume"] = [](AudioRef& r, float v) {
        Need<AudioSourceComponent>(r.Obj, "SetVolume").Volume = v;
    };
    au["SetPitch"] = [](AudioRef& r, float v) {
        Need<AudioSourceComponent>(r.Obj, "SetPitch").Pitch = v;
    };
    au["SetLoop"] = [](AudioRef& r, bool v) {
        Need<AudioSourceComponent>(r.Obj, "SetLoop").Loop = v;
    };
    au["SetClip"] = [](AudioRef& r, const std::string& clip) {
        Need<AudioSourceComponent>(r.Obj, "SetClip").Clip = clip;
    };
    // Разовый звук ПОВЕРХ текущего: шаг, щелчок, попадание. Не трогает clip
    // источника — иначе «сыграть один раз» обрывало бы фоновый шум.
    au["PlayOneShot"] = [self](AudioRef& r, const std::string& clip, sol::optional<float> volume) {
        const AudioSourceComponent& a = Need<AudioSourceComponent>(r.Obj, "PlayOneShot");
        AudioEngine* engine = self->Services().Audio;
        if (!engine) throw std::runtime_error("PlayOneShot: звук не привязан");
        const float v = volume.value_or(a.Volume);
        if (a.Spatial && r.Obj.Valid()) {
            Scene* scene = self->ScenePtr();
            const glm::vec3 pos = scene ? glm::vec3(scene->WorldMatrix(r.Obj.Entity())[3])
                                        : glm::vec3(0.0f);
            engine->PlaySound3D(clip, pos, v);
        } else {
            engine->PlaySound2D(clip, v);
        }
    };

    // --- Camera --------------------------------------------------------------
    sol::usertype<CameraRef> cam = lua.new_usertype<CameraRef>("SageCamera");
    cam["SetFOV"] = [](CameraRef& r, float fov) {
        Need<CameraComponent>(r.Obj, "SetFOV").Fov = fov;
    };
    cam["GetFOV"] = [](CameraRef& r) {
        return Need<CameraComponent>(r.Obj, "GetFOV").Fov;
    };
    cam["SetNearClip"] = [](CameraRef& r, float v) {
        Need<CameraComponent>(r.Obj, "SetNearClip").NearClip = v;
    };
    cam["SetFarClip"] = [](CameraRef& r, float v) {
        Need<CameraComponent>(r.Obj, "SetFarClip").FarClip = v;
    };
    // «Главная» камера сцены: с неё рисуется кадр. Не «включена/выключена» —
    // камера без Primary просто не выбрана, и это разные вещи.
    cam["SetPrimary"] = [](CameraRef& r, bool on) {
        Need<CameraComponent>(r.Obj, "SetPrimary").Primary = on;
    };
    cam["transform"] = sol::property([](CameraRef& r) { return TransformRef(r.Obj); });
}

// Имя компонента → прокси. ОДИН список на весь бэкенд: и GetComponent, и
// публичное поле field.component("...") ходят сюда, поэтому «Animation» значит
// одно и то же в обоих местах — и остаётся значить после добавления нового
// компонента.
sol::object Backend::ComponentOf(GameObject object, const std::string& component) {
    if (!object.Valid()) return sol::nil;
    entt::registry& reg = *object.Registry();
    const entt::entity e = object.Entity();

    if (component == "Transform") {
        return reg.all_of<Transform>(e) ? sol::make_object(*m_lua, TransformRef(object)) : sol::nil;
    }
    if (component == "CharacterController") {
        return reg.all_of<CharacterControllerComponent>(e)
                   ? sol::make_object(*m_lua, CharacterRef(object))
                   : sol::nil;
    }
    if (component == "Animation" || component == "Animator") {
        return reg.all_of<AnimationComponent>(e) ? sol::make_object(*m_lua, AnimatorRef(object))
                                                 : sol::nil;
    }
    if (component == "Audio" || component == "AudioSource") {
        return reg.all_of<AudioSourceComponent>(e) ? sol::make_object(*m_lua, AudioRef(object))
                                                   : sol::nil;
    }
    if (component == "Camera") {
        return reg.all_of<CameraComponent>(e) ? sol::make_object(*m_lua, CameraRef(object))
                                              : sol::nil;
    }
    if (component == "Script") return ScriptOf(object);
    // Неизвестное имя — не ошибка, а nil: список компонентов растёт, и скрипт,
    // спросивший то, чего в этой сборке нет, должен уметь это проверить.
    return sol::nil;
}

sol::object Backend::ComponentOf(const sage::vars::EntityRef& ref, const std::string& component) {
    Scene* scene = m_services.ScenePtr;
    if (!scene || !ref.Valid()) return sol::nil;
    GameObject object = scene->Get(ref.Id);
    if (!object.Valid()) return sol::nil;
    // Пустое уточнение — сам объект (field.entity()).
    if (component.empty()) return Wrap(object);
    return ComponentOf(object, component);
}

} // namespace sage::scripting::lua
