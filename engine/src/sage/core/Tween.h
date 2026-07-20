#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace sage {

// ---------------------------------------------------------------------------
// Tween — система интерполяции значений во времени (inbetweening). Плавно ведёт
// значение из A в B за duration секунд по кривой сглаживания (easing), вызывая
// сеттер каждый апдейт: анимации UI, движения/повороты/масштаб объектов, фейды
// цвета, ходы камеры — без ручного лерпа в каждом OnUpdate.
//
// TweenManager владеет активными твинами и шагает их Update(dt). В движке им
// владеет ScriptEngine (твины — геймплей: тикают со скриптами, замирают на
// паузе, умирают на Stop), а игры/редактор могут завести и свой. Из Lua —
// TweenMove/TweenScale/TweenRotate/TweenColor/TweenUIValue + enum Ease.
//
// Значения интерполируются glm::mix (скаляры/векторы) или slerp (кватернионы);
// кривые Back/Elastic сознательно выходят за [0,1] (overshoot) — glm::mix
// экстраполирует, давая «пружину».
// ---------------------------------------------------------------------------

enum class Easing {
    Linear,
    QuadIn, QuadOut, QuadInOut,
    CubicIn, CubicOut, CubicInOut,
    SineIn, SineOut, SineInOut,
    ExpoOut,
    BackOut,
    ElasticOut,
    BounceOut,
};

// Сглаживание параметра t∈[0,1] -> eased (может выходить за [0,1] у Back/Elastic).
float Ease(Easing e, float t);

// Режим повторения: None — один раз; Restart — заново с A; PingPong — туда-обратно.
enum class TweenLoop { None, Restart, PingPong };

using TweenId = uint64_t;
constexpr TweenId kInvalidTween = 0;

// Интерполяция значения твина: по умолчанию glm::mix, для кватернионов — slerp.
template <class T> inline T TweenLerp(const T& a, const T& b, float t) { return glm::mix(a, b, t); }
template <> inline float TweenLerp<float>(const float& a, const float& b, float t) { return a + (b - a) * t; }
template <> inline glm::quat TweenLerp<glm::quat>(const glm::quat& a, const glm::quat& b, float t) {
    return glm::slerp(a, b, t);
}

class TweenManager {
public:
    // Запускает твин значения from->to за duration с кривой ease; setter(value)
    // зовётся каждый апдейт (значение уже сглажено), onComplete — по завершении
    // (у нелупящихся). delay — задержка старта, loop — повтор. Возвращает id.
    template <class T>
    TweenId To(const T& from, const T& to, float duration, Easing ease,
               std::function<void(const T&)> setter,
               std::function<void()> onComplete = {},
               float delay = 0.0f, TweenLoop loop = TweenLoop::None) {
        auto impl = std::make_unique<Impl<T>>();
        impl->From = from;
        impl->To = to;
        impl->Duration = duration > 1e-6f ? duration : 1e-6f;
        impl->EaseFn = ease;
        impl->Setter = std::move(setter);
        impl->OnComplete = std::move(onComplete);
        impl->Delay = delay > 0.0f ? delay : 0.0f;
        impl->Loop = loop;
        return Add(std::move(impl));
    }

    void Update(float dt);   // шагает все, зовёт коллбеки, удаляет завершённые
    void Cancel(TweenId id); // остановить конкретный (без onComplete)
    void CancelAll();
    int ActiveCount() const { return (int)(m_tweens.size() + m_pending.size()); }
    bool IsActive(TweenId id) const;

private:
    struct Active {
        virtual ~Active() = default;
        virtual bool Step(float dt) = 0; // true -> завершён/отменён (удалить)
        TweenId Id = 0;
        bool Cancelled = false;
    };

    template <class T>
    struct Impl : Active {
        T From{}, To{};
        float Duration = 1.0f, Delay = 0.0f, Elapsed = 0.0f;
        Easing EaseFn = Easing::Linear;
        TweenLoop Loop = TweenLoop::None;
        std::function<void(const T&)> Setter;
        std::function<void()> OnComplete;

        bool Step(float dt) override {
            if (Cancelled) return true;
            Elapsed += dt;
            float local = Elapsed - Delay;
            if (local < 0.0f) return false; // ещё ждём delay
            bool finished = local >= Duration;
            float t = finished ? 1.0f : local / Duration;
            if (Setter) Setter(TweenLerp(From, To, Ease(EaseFn, t)));
            if (!finished) return false;
            if (Loop == TweenLoop::Restart) { Elapsed = Delay; return false; }
            if (Loop == TweenLoop::PingPong) { std::swap(From, To); Elapsed = Delay; return false; }
            if (OnComplete) OnComplete();
            return true;
        }
    };

    TweenId Add(std::unique_ptr<Active> a);
    std::vector<std::unique_ptr<Active>> m_tweens;
    // Новые твины, добавленные ВО ВРЕМЯ Update (из сеттера/onComplete), копятся
    // тут и вливаются в начале следующего Update — иначе push_back в m_tweens
    // мог бы реаллоцировать вектор и подвесить this у исполняющегося Step.
    std::vector<std::unique_ptr<Active>> m_pending;
    bool m_updating = false;
    TweenId m_next = 1;
};

} // namespace sage
