#include "ScriptEngine.h"

#include "sage/ui/UI.h"

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
    // Полоса/значение UI (0..1) — например плавная убыль здоровья.
    Bind("tween", "UIValue", "TweenUIValue", [this, easeOr](GameObject obj, float to, float dur,
                                                      sol::optional<sage::Easing> ease) -> uint64_t {
        if (!obj.Valid()) return 0;
        auto* ui = obj.Registry()->try_get<sage::ui::Bar>(obj.Entity());
        if (!ui) return 0;
        return m_tweens.To<float>(ui->Value, to, dur, easeOr(ease, sage::Easing::QuadOut),
            [obj](const float& v) mutable {
                if (!obj.Valid()) return;
                if (auto* u = obj.Registry()->try_get<sage::ui::Bar>(obj.Entity())) u->Value = v;
            });
    });
    Bind("tween", "Cancel", "TweenCancel", [this](uint64_t id) { m_tweens.Cancel(id); });
    Bind("tween", "CancelAll", "TweenCancelAll", [this]() { m_tweens.CancelAll(); });
    Bind("tween", "Active", "ActiveTweens", [this]() { return m_tweens.ActiveCount(); });
}

