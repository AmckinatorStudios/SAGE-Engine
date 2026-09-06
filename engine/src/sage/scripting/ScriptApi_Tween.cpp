#include "ScriptEngine.h"

#include "sage/ui/UIFramework.h"
#include "sage/ui/scene/UIScene.h"

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
    // --- Твины (интерполяция значений) --------------------------------------
    // Плавно ведут поля сущности к цели за duration секунд по кривой ease.
    // Тикают со скриптами (UpdateAll). Возвращают id (для TweenCancel), сеттеры
    // проверяют Valid() — уничтоженная в полёте сущность безопасно пропускается.
    m_lua.new_enum<sage::Easing>("Ease", {
        {"Linear", sage::Easing::Linear},
        {"QuadIn", sage::Easing::QuadIn}, {"QuadOut", sage::Easing::QuadOut}, {"QuadInOut", sage::Easing::QuadInOut},
        {"CubicIn", sage::Easing::CubicIn}, {"CubicOut", sage::Easing::CubicOut}, {"CubicInOut", sage::Easing::CubicInOut},
        {"SineIn", sage::Easing::SineIn}, {"SineOut", sage::Easing::SineOut}, {"SineInOut", sage::Easing::SineInOut},
        {"ExpoOut", sage::Easing::ExpoOut}, {"BackOut", sage::Easing::BackOut},
        {"ElasticOut", sage::Easing::ElasticOut}, {"BounceOut", sage::Easing::BounceOut},
    });

    auto easeOr = [](sol::optional<sage::Easing> e, sage::Easing def) { return e.value_or(def); };

    Bind("tween", "Move", "TweenMove", [this, easeOr](GameObject obj, glm::vec3 to, float dur,
                                                   sol::optional<sage::Easing> ease) -> uint64_t {
        if (!obj.Valid()) return 0;
        return m_tweens.To<glm::vec3>(obj.GetTransform().Position, to, dur, easeOr(ease, sage::Easing::QuadOut),
            [obj](const glm::vec3& v) mutable { if (obj.Valid()) obj.GetTransform().Position = v; });
    });
    Bind("tween", "Scale", "TweenScale", [this, easeOr](GameObject obj, glm::vec3 to, float dur,
                                                    sol::optional<sage::Easing> ease) -> uint64_t {
        if (!obj.Valid()) return 0;
        return m_tweens.To<glm::vec3>(obj.GetTransform().Scale, to, dur, easeOr(ease, sage::Easing::QuadOut),
            [obj](const glm::vec3& v) mutable { if (obj.Valid()) obj.GetTransform().Scale = v; });
    });
    Bind("tween", "Rotate", "TweenRotate", [this, easeOr](GameObject obj, glm::vec3 toEulerDeg, float dur,
                                                     sol::optional<sage::Easing> ease) -> uint64_t {
        if (!obj.Valid()) return 0;
        return m_tweens.To<glm::vec3>(obj.GetTransform().Rotation, toEulerDeg, dur, easeOr(ease, sage::Easing::QuadInOut),
            [obj](const glm::vec3& v) mutable { if (obj.Valid()) obj.GetTransform().Rotation = v; });
    });
    Bind("tween", "Color", "TweenColor", [this, easeOr](GameObject obj, glm::vec3 to, float dur,
                                                    sol::optional<sage::Easing> ease) -> uint64_t {
        if (!obj.Valid()) return 0;
        return m_tweens.To<glm::vec3>(obj.ColorRef(), to, dur, easeOr(ease, sage::Easing::Linear),
            [obj](const glm::vec3& v) mutable { if (obj.Valid()) obj.ColorRef() = v; });
    });
    // Значение интерфейса (полоса, ползунок) — по АДРЕСУ СВОЙСТВА в документе:
    //
    //   sage.tween.UIValue("assets/ui/hud.uidoc", "Health/Fill.progress.Value", 0.3, 0.4)
    //
    // Не по объекту сцены: интерфейс живёт в документе, и объект — не он.
    Bind("tween", "UIValue", "TweenUIValue",
         [this, easeOr](const std::string& document, const std::string& property, float to,
                        float dur, sol::optional<sage::Easing> ease) -> uint64_t {
             sage::ui::UIRuntime* rt = sage::ui::UIDocuments::Instance().Find(document);
             if (!rt) return 0;
             auto binding = std::make_shared<sage::ui::UIPropertyBinding>();
             if (!binding->Bind(rt->Doc(), property)) return 0;
             float from = 0.0f;
             binding->Get(from);
             // Значение живёт в документе, а не в анимации: держать его копию
             // здесь значило бы потерять чужую правку посреди анимации.
             return m_tweens.To<float>(from, to, dur, easeOr(ease, sage::Easing::QuadOut),
                                       [binding](const float& v) { binding->Set(v); });
         });
    Bind("tween", "Cancel", "TweenCancel", [this](uint64_t id) { m_tweens.Cancel(id); });
    Bind("tween", "CancelAll", "TweenCancelAll", [this]() { m_tweens.CancelAll(); });
    Bind("tween", "Active", "ActiveTweens", [this]() { return m_tweens.ActiveCount(); });
}

