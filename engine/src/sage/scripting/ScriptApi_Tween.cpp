#include "ScriptEngine.h"

#include "sage/scene/Scene.h"
#include "sage/scripting/ScriptTween.h"

#include "sage/core/Log.h"

// ---------------------------------------------------------------------------
// Твины: sage.tween.*
//
// Часть Lua-API движка. Раньше ВСЕ привязки жили в одном ScriptEngine.cpp на
// 1800 строк: 126 функций, восемнадцать областей, и чтобы дописать одну
// строчку про анимацию, приходилось листать интерфейс, физику и таймеры.
// Определения разъехались по файлам ScriptApi_*.cpp — по файлу на область;
// объявления методов остались в ScriptEngine.h, поэтому порядок регистрации
// по-прежнему записан в одном месте (RegisterEngineApi) и не зависит от того,
// в каком файле лежит тело.
// ---------------------------------------------------------------------------

void ScriptEngine::RegisterTweenApi() {
    // --- Прежние твины (TweenMove и др.) --------------------------------------
    //
    // Работают через ТОТ ЖЕ проигрыватель сцены, что и Tween.to и окно Tween
    // редактора (Scene::Tweens, sage/anim/Tween.h): отдельной системы
    // интерполяции у прежнего API больше нет. Возвращают номер твина (для
    // TweenCancel); уничтоженный в полёте объект просто выпадает из твина.
    //
    // Ease — общая таблица кривых (строки): прежние имена (Ease.QuadOut,
    // Ease.BackOut) в ней есть, и числа прежнего перечисления тоже понимаются.
    m_lua["Ease"] = sage::scripting::tween::MakeEaseTable(m_lua);

    auto play = [this](GameObject obj, const char* property, const glm::vec4& to, float dur,
                       const sol::object& easeArg, sage::anim::Ease fallback) -> uint64_t {
        if (!obj.Valid() || !m_scene) return 0;
        sage::anim::TweenClip clip;
        clip.Name = property;
        sage::anim::TweenTrack t;
        t.Property = property;
        t.To = to;
        t.Duration = dur > 1e-4f ? dur : 1e-4f;
        t.Curve = sage::scripting::tween::EaseFrom(easeArg, fallback);
        clip.Tracks.push_back(t);
        return m_scene->Tweens.Play(m_scene->Registry(), obj.Entity(), clip);
    };
    using sage::anim::Ease;
    using sage::anim::EaseMode;
    using sage::anim::EaseShape;
    const Ease quadOut = Ease::Make(EaseShape::Quad, EaseMode::Out);

    Bind("tween", "Move", "TweenMove", [play, quadOut](GameObject obj, glm::vec3 to, float dur, sol::object ease) {
        return play(obj, "object.position", glm::vec4(to, 0.0f), dur, ease, quadOut);
    });
    Bind("tween", "Scale", "TweenScale", [play, quadOut](GameObject obj, glm::vec3 to, float dur, sol::object ease) {
        return play(obj, "object.scale", glm::vec4(to, 0.0f), dur, ease, quadOut);
    });
    Bind("tween", "Rotate", "TweenRotate", [play](GameObject obj, glm::vec3 toEulerDeg, float dur, sol::object ease) {
        return play(obj, "object.rotation", glm::vec4(toEulerDeg, 0.0f), dur, ease,
                    Ease::Make(EaseShape::Quad, EaseMode::InOut));
    });
    Bind("tween", "Color", "TweenColor", [play](GameObject obj, glm::vec3 to, float dur, sol::object ease) {
        if (obj.Valid()) obj.ColorRef();   // цвет у объекта без MeshRenderer — как было: заводится
        return play(obj, "material.color", glm::vec4(to, 1.0f), dur, ease, Ease::Linear());
    });
    // Полоса/значение UI (0..1) — например плавная убыль здоровья.
    Bind("tween", "UIValue", "TweenUIValue", [play, quadOut](GameObject obj, float to, float dur, sol::object ease) {
        return play(obj, "bar.value", glm::vec4(to, 0, 0, 0), dur, ease, quadOut);
    });
    Bind("tween", "Cancel", "TweenCancel", [this](uint64_t id) {
        if (m_scene) m_scene->Tweens.Stop((sage::anim::TweenHandle)id);
    });
    Bind("tween", "CancelAll", "TweenCancelAll", [this]() {
        if (m_scene) m_scene->Tweens.StopAll();
    });
    Bind("tween", "Active", "ActiveTweens", [this]() { return m_scene ? m_scene->Tweens.Count() : 0; });
}
