#include "sage/core/Tween.h"

#include <algorithm>
#include <cmath>

namespace sage {

namespace {
constexpr float kPi = 3.14159265358979323846f;
}

float Ease(Easing e, float t) {
    // Клампим ВХОД в [0,1] (шагающий код и так не выходит за пределы, но защита
    // от dt-выбросов не помешает); ВЫХОД у Back/Elastic намеренно за пределами.
    t = std::clamp(t, 0.0f, 1.0f);
    switch (e) {
        case Easing::Linear:    return t;
        case Easing::QuadIn:    return t * t;
        case Easing::QuadOut:   return 1.0f - (1.0f - t) * (1.0f - t);
        case Easing::QuadInOut: return t < 0.5f ? 2.0f * t * t
                                                : 1.0f - std::pow(-2.0f * t + 2.0f, 2.0f) / 2.0f;
        case Easing::CubicIn:    return t * t * t;
        case Easing::CubicOut:   return 1.0f - std::pow(1.0f - t, 3.0f);
        case Easing::CubicInOut: return t < 0.5f ? 4.0f * t * t * t
                                                 : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) / 2.0f;
        case Easing::SineIn:    return 1.0f - std::cos((t * kPi) / 2.0f);
        case Easing::SineOut:   return std::sin((t * kPi) / 2.0f);
        case Easing::SineInOut: return -(std::cos(kPi * t) - 1.0f) / 2.0f;
        case Easing::ExpoOut:   return t >= 1.0f ? 1.0f : 1.0f - std::pow(2.0f, -10.0f * t);
        case Easing::BackOut: {
            const float c1 = 1.70158f, c3 = c1 + 1.0f;
            float p = t - 1.0f;
            return 1.0f + c3 * p * p * p + c1 * p * p;
        }
        case Easing::ElasticOut: {
            if (t == 0.0f || t == 1.0f) return t;
            const float c4 = (2.0f * kPi) / 3.0f;
            return std::pow(2.0f, -10.0f * t) * std::sin((t * 10.0f - 0.75f) * c4) + 1.0f;
        }
        case Easing::BounceOut: {
            const float n1 = 7.5625f, d1 = 2.75f;
            if (t < 1.0f / d1) return n1 * t * t;
            if (t < 2.0f / d1) { t -= 1.5f / d1; return n1 * t * t + 0.75f; }
            if (t < 2.5f / d1) { t -= 2.25f / d1; return n1 * t * t + 0.9375f; }
            t -= 2.625f / d1;
            return n1 * t * t + 0.984375f;
        }
    }
    return t;
}

TweenId TweenManager::Add(std::unique_ptr<Active> a) {
    TweenId id = m_next++;
    a->Id = id;
    // Во время Update кладём в pending (влиётся в начале следующего Update) —
    // не трогаем m_tweens, по которому идёт цикл. Вне Update — сразу в m_tweens.
    (m_updating ? m_pending : m_tweens).push_back(std::move(a));
    return id;
}

void TweenManager::Update(float dt) {
    // Вливаем твины, добавленные с прошлого кадра (в т.ч. из чужих Update).
    for (auto& p : m_pending) m_tweens.push_back(std::move(p));
    m_pending.clear();

    m_updating = true; // новые твины отсюда -> в m_pending (стабильность вектора)
    size_t n = m_tweens.size();
    for (size_t i = 0; i < n; ++i) {
        if (m_tweens[i]->Step(dt)) m_tweens[i]->Cancelled = true; // на удаление
    }
    m_updating = false;

    m_tweens.erase(std::remove_if(m_tweens.begin(), m_tweens.end(),
                                  [](const std::unique_ptr<Active>& a) { return a->Cancelled; }),
                   m_tweens.end());
}

void TweenManager::Cancel(TweenId id) {
    for (auto& a : m_tweens)
        if (a->Id == id) { a->Cancelled = true; return; }
    for (auto& a : m_pending)
        if (a->Id == id) { a->Cancelled = true; return; }
}

void TweenManager::CancelAll() { m_tweens.clear(); m_pending.clear(); }

bool TweenManager::IsActive(TweenId id) const {
    for (const auto& a : m_tweens)
        if (a->Id == id && !a->Cancelled) return true;
    for (const auto& a : m_pending)
        if (a->Id == id && !a->Cancelled) return true;
    return false;
}

} // namespace sage
