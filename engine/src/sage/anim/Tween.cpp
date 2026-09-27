#include "sage/anim/Tween.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "sage/scene/Scene.h"

namespace sage::anim {

// =============================================================================
//  Кривые
// =============================================================================

namespace {

constexpr float kPi = 3.14159265358979f;

// Разгон «In» формы: 0 → 1, медленно в начале. Out и InOut выводятся из него
// одинаково для всех форм — отдельной формулы на каждое направление нет.
float In(EaseShape s, float t) {
    switch (s) {
        case EaseShape::Linear: return t;
        case EaseShape::Quad: return t * t;
        case EaseShape::Cubic: return t * t * t;
        case EaseShape::Quart: return t * t * t * t;
        case EaseShape::Quint: return t * t * t * t * t;
        case EaseShape::Sine: return 1.0f - std::cos(t * kPi * 0.5f);
        case EaseShape::Expo: return t <= 0.0f ? 0.0f : std::pow(2.0f, 10.0f * t - 10.0f);
        case EaseShape::Circ: return 1.0f - std::sqrt(std::max(0.0f, 1.0f - t * t));
        case EaseShape::Back: {
            const float c1 = 1.70158f, c3 = c1 + 1.0f;
            return c3 * t * t * t - c1 * t * t;
        }
        case EaseShape::Elastic: {
            if (t <= 0.0f || t >= 1.0f) return t <= 0.0f ? 0.0f : 1.0f;
            const float c4 = (2.0f * kPi) / 3.0f;
            return -std::pow(2.0f, 10.0f * t - 10.0f) * std::sin((t * 10.0f - 10.75f) * c4);
        }
        case EaseShape::Bounce: {
            // Bounce «In» — отражение «Out»: отскоки в начале.
            float x = 1.0f - t;
            const float n1 = 7.5625f, d1 = 2.75f;
            float out;
            if (x < 1.0f / d1) out = n1 * x * x;
            else if (x < 2.0f / d1) { x -= 1.5f / d1; out = n1 * x * x + 0.75f; }
            else if (x < 2.5f / d1) { x -= 2.25f / d1; out = n1 * x * x + 0.9375f; }
            else { x -= 2.625f / d1; out = n1 * x * x + 0.984375f; }
            return 1.0f - out;
        }
        default: return t;
    }
}

// Кубическая Безье через (0,0), (x1,y1), (x2,y2), (1,1): по x найти параметр
// (Ньютон, при неудаче — деление пополам), вернуть y. Точность — тысячная,
// больше глазу не видно.
float Bezier(const glm::vec4& c, float x) {
    auto coord = [](float a, float b, float u) {
        const float v = 1.0f - u;
        return 3.0f * v * v * u * a + 3.0f * v * u * u * b + u * u * u;
    };
    auto slope = [](float a, float b, float u) {
        const float v = 1.0f - u;
        return 3.0f * v * v * a + 6.0f * v * u * (b - a) + 3.0f * u * u * (1.0f - b);
    };
    const float x1 = std::clamp(c.x, 0.0f, 1.0f), x2 = std::clamp(c.z, 0.0f, 1.0f);
    float u = x;
    for (int i = 0; i < 8; ++i) {
        const float err = coord(x1, x2, u) - x;
        if (std::fabs(err) < 1e-5f) return coord(c.y, c.w, u);
        const float d = slope(x1, x2, u);
        if (std::fabs(d) < 1e-6f) break;
        u -= err / d;
    }
    float lo = 0.0f, hi = 1.0f;
    u = x;
    for (int i = 0; i < 24; ++i) {
        const float v = coord(x1, x2, u);
        if (std::fabs(v - x) < 1e-5f) break;
        if (v < x) lo = u; else hi = u;
        u = (lo + hi) * 0.5f;
    }
    return coord(c.y, c.w, u);
}

std::string Lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

} // namespace

float Evaluate(const Ease& e, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    if (e.Shape == EaseShape::Curve) return Bezier(e.Bezier, t);
    if (e.Shape == EaseShape::Linear) return t;
    switch (e.Mode) {
        case EaseMode::In: return In(e.Shape, t);
        case EaseMode::Out: return 1.0f - In(e.Shape, 1.0f - t);
        case EaseMode::InOut:
            return t < 0.5f ? In(e.Shape, t * 2.0f) * 0.5f : 1.0f - In(e.Shape, (1.0f - t) * 2.0f) * 0.5f;
        default: return t;
    }
}

const char* ShapeName(EaseShape s) {
    static const char* kNames[] = {"linear", "quad", "cubic", "quart", "quint", "sine",
                                   "expo",   "circ", "back",  "elastic", "bounce", "curve"};
    return (size_t)s < sizeof(kNames) / sizeof(kNames[0]) ? kNames[(size_t)s] : "linear";
}

const char* ModeName(EaseMode m) {
    switch (m) {
        case EaseMode::In: return "in";
        case EaseMode::Out: return "out";
        case EaseMode::InOut: return "inout";
        default: return "out";
    }
}

std::string ToString(const Ease& e) {
    if (e.Shape == EaseShape::Linear) return "linear";
    if (e.Shape == EaseShape::Curve) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "curve(%g,%g,%g,%g)", e.Bezier.x, e.Bezier.y, e.Bezier.z, e.Bezier.w);
        return buf;
    }
    return std::string(ShapeName(e.Shape)) + "-" + ModeName(e.Mode);
}

bool Parse(const std::string& textIn, Ease& out) {
    std::string text = Lower(textIn);
    text.erase(std::remove_if(text.begin(), text.end(), [](char c) { return c == ' ' || c == '_'; }),
               text.end());
    if (text.empty()) return false;
    if (text.rfind("curve(", 0) == 0) {
        glm::vec4 c;
        if (std::sscanf(text.c_str(), "curve(%f,%f,%f,%f)", &c.x, &c.y, &c.z, &c.w) != 4) return false;
        out.Shape = EaseShape::Curve;
        out.Bezier = c;
        return true;
    }
    if (text == "linear") { out = Ease::Linear(); return true; }
    // Направление: «in», «out», «inout» — отдельно (= Quad) или с формой в
    // любом порядке и написании: «back-out», «outback», «BackOut», «EaseOutBack».
    if (text.rfind("ease", 0) == 0) text = text.substr(4);
    text.erase(std::remove(text.begin(), text.end(), '-'), text.end());
    EaseMode mode = EaseMode::Out;
    bool haveMode = false;
    for (const char* m : {"inout", "in", "out"}) {
        const std::string ms = m;
        if (text.size() >= ms.size() && text.compare(0, ms.size(), ms) == 0) {
            mode = ms == "in" ? EaseMode::In : ms == "out" ? EaseMode::Out : EaseMode::InOut;
            text = text.substr(ms.size());
            haveMode = true;
            break;
        }
        if (text.size() >= ms.size() && text.compare(text.size() - ms.size(), ms.size(), ms) == 0) {
            // «inout» в конце проверяется раньше «out» — порядок списка выше.
            mode = ms == "in" ? EaseMode::In : ms == "out" ? EaseMode::Out : EaseMode::InOut;
            text = text.substr(0, text.size() - ms.size());
            haveMode = true;
            break;
        }
    }
    EaseShape shape = EaseShape::Quad;
    if (!text.empty()) {
        bool found = false;
        for (int i = 0; i < (int)EaseShape::Curve; ++i) {
            if (text == ShapeName((EaseShape)i)) { shape = (EaseShape)i; found = true; break; }
        }
        if (!found) return false;
    } else if (!haveMode) {
        return false;
    }
    out = Ease::Make(shape, mode);
    return true;
}

glm::vec4 BezierFor(const Ease& e) {
    if (e.Shape == EaseShape::Curve) return e.Bezier;
    if (e.Shape == EaseShape::Linear) return {0.0f, 0.0f, 1.0f, 1.0f};
    // Сила разгона: чем «круче» форма, тем ближе ручки к углу.
    float k;
    switch (e.Shape) {
        case EaseShape::Sine: k = 0.37f; break;
        case EaseShape::Quad: k = 0.45f; break;
        case EaseShape::Cubic: k = 0.6f; break;
        case EaseShape::Quart: k = 0.7f; break;
        case EaseShape::Quint: k = 0.8f; break;
        case EaseShape::Expo: k = 0.9f; break;
        case EaseShape::Circ: k = 0.85f; break;
        default: k = 0.55f; break;
    }
    switch (e.Mode) {
        case EaseMode::In: return {k, 0.0f, 1.0f, 1.0f};
        case EaseMode::Out: return {0.0f, 0.0f, 1.0f - k, 1.0f};
        default: return {k, 0.0f, 1.0f - k, 1.0f};
    }
}

const char* LoopName(TweenLoop l) {
    switch (l) {
        case TweenLoop::Loop: return "loop";
        case TweenLoop::PingPong: return "pingpong";
        default: return "once";
    }
}

bool ParseLoop(const std::string& textIn, TweenLoop& out) {
    std::string t = Lower(textIn);
    t.erase(std::remove_if(t.begin(), t.end(), [](char c) { return c == '-' || c == '_' || c == ' '; }),
            t.end());
    if (t == "once" || t == "none") { out = TweenLoop::Once; return true; }
    if (t == "loop" || t == "restart" || t == "repeat") { out = TweenLoop::Loop; return true; }
    if (t == "pingpong" || t == "yoyo") { out = TweenLoop::PingPong; return true; }
    return false;
}

// =============================================================================
//  Данные
// =============================================================================

float TweenClip::Length() const {
    float end = 0.0f;
    for (const TweenTrack& t : Tracks) end = std::max(end, t.End());
    return end;
}

void TweenClip::Stretch(float length) {
    const float old = Length();
    if (old <= 1e-6f || length <= 1e-6f) return;
    const float k = length / old;
    for (TweenTrack& t : Tracks) {
        t.Start *= k;
        t.Duration *= k;
    }
}

size_t TweenClip::AppendAfter(TweenTrack track, float gap) {
    track.Start = Length() + std::max(gap, 0.0f);
    Tracks.push_back(std::move(track));
    return Tracks.size() - 1;
}

size_t TweenClip::AppendWith(TweenTrack track) {
    track.Start = Tracks.empty() ? 0.0f : Tracks.back().Start;
    Tracks.push_back(std::move(track));
    return Tracks.size() - 1;
}

void TweenClip::ArrangeSequence() {
    float at = 0.0f;
    for (TweenTrack& t : Tracks) {
        t.Start = at;
        at += t.Duration;
    }
}

void TweenClip::ArrangeParallel() {
    for (TweenTrack& t : Tracks) t.Start = 0.0f;
}

// =============================================================================
//  Проигрыватель
// =============================================================================

TweenPlayer::Active* TweenPlayer::Find(TweenHandle h) {
    for (Active& a : m_active)
        if (a.Id == h && !a.Done) return &a;
    return nullptr;
}

const TweenPlayer::Active* TweenPlayer::Find(TweenHandle h) const {
    for (const Active& a : m_active)
        if (a.Id == h && !a.Done) return &a;
    return nullptr;
}

TweenHandle TweenPlayer::Play(entt::registry& reg, entt::entity target, const TweenClip& clip, entt::entity owner) {
    if (!reg.valid(target)) return kNoTween;
    // ПОСЛЕДНИЙ ЗАПУЩЕННЫЙ ГЛАВНЫЙ. Два твина одного свойства одного объекта
    // писали бы по очереди, и объект дёргался бы между ними. Новый отнимает
    // свойство у прежних (прочие их дорожки идут дальше).
    for (Active& a : m_active) {
        if (a.Done || a.Target != target || a.Reg != &reg) continue;
        for (size_t k = 0; k < a.Clip.Tracks.size() && k < a.Channels.size(); ++k)
            for (const TweenTrack& t : clip.Tracks)
                if (t.Property == a.Clip.Tracks[k].Property) a.Channels[k].Missing = true;
    }
    Active a;
    a.Id = m_next++;
    if (m_next == kNoTween) m_next = 1;
    a.Reg = &reg;
    a.Target = target;
    a.Owner = owner;
    a.Clip = clip;
    Sync(a);
    m_active.push_back(std::move(a));
    return m_active.back().Id;
}

void TweenPlayer::Sync(Active& a) {
    while (a.Channels.size() < a.Clip.Tracks.size()) {
        const TweenTrack& t = a.Clip.Tracks[a.Channels.size()];
        Channel ch;
        if (const PropertyType* p = FindProperty(t.Property)) {
            ch.Access = ResolveAccess(*p);
            ch.Missing = !ch.Access.Valid() || !HasProperty(*p, *a.Reg, a.Target);
        } else {
            ch.Missing = true;
        }
        a.Channels.push_back(ch);
    }
}

bool TweenPlayer::Apply(Active& a, float local) {
    const TweenClip& clip = a.Clip;
    const float length = std::max(clip.Length(), 1e-6f);
    float t = local;
    bool finished = false;
    switch (clip.Loop) {
        case TweenLoop::Once:
            if (local >= length) { t = length; finished = true; }
            break;
        case TweenLoop::Loop:
            t = std::fmod(local, length);
            break;
        case TweenLoop::PingPong: {
            const float cycle = std::floor(local / length);
            t = local - cycle * length;
            if (((long long)cycle) % 2 == 1) t = length - t;
            break;
        }
    }
    if (clip.Reverse) t = length - t;

    for (size_t k = 0; k < clip.Tracks.size() && k < a.Channels.size(); ++k) {
        const TweenTrack& tr = clip.Tracks[k];
        Channel& ch = a.Channels[k];
        if (ch.Missing) continue;
        const float dur = std::max(tr.Duration, 1e-6f);
        const float tt = (t - tr.Start) / dur;
        // Дорожка, до которой дело ещё не дошло, объект не трогает: шаг
        // «затем Scale» не имеет права заморозить масштаб, пока идёт Move.
        // Задом наперёд и в повторе дорожка уже прочитана — ставим её начало.
        if (tt < 0.0f && !ch.Resolved && !clip.Reverse) continue;
        if (!ch.Resolved) {
            glm::vec4 now(0.0f);
            if (!Read(ch.Access, *a.Reg, a.Target, now)) { ch.Missing = true; continue; }
            ch.From = tr.FromCurrent ? now : tr.From;
            ch.To = tr.ToCurrent ? now : tr.To;
            ch.Resolved = true;
        }
        const float e = Evaluate(tr.Curve, std::clamp(tt, 0.0f, 1.0f));
        Write(ch.Access, *a.Reg, a.Target, ch.From + (ch.To - ch.From) * e);
    }
    return finished;
}

void TweenPlayer::Update(float dt) {
    // По индексу: новый твин из скрипта может прийти посреди прохода
    // (OnComplete зовутся после, но Play из Write-сеттера не исключён).
    const size_t count = m_active.size();
    for (size_t i = 0; i < count && i < m_active.size(); ++i) {
        Active& a = m_active[i];
        if (a.Done || a.Paused) continue;
        if (!a.Reg->valid(a.Target)) { a.Done = true; continue; }
        Sync(a);
        a.Time += dt * std::max(a.Clip.Speed, 0.0f);
        const float local = a.Time - a.Clip.Delay;
        if (local < 0.0f) continue;
        if (Apply(a, local)) {
            a.Done = true;
            if (a.OnComplete) m_callbacks.push_back(std::move(a.OnComplete));
            if (!a.Clip.Then.empty() && a.Owner != entt::null) m_chains.push_back({a.Owner, a.Clip.Then});
        }
    }
    m_active.erase(std::remove_if(m_active.begin(), m_active.end(), [](const Active& a) { return a.Done; }),
                   m_active.end());
    if (m_callbacks.empty()) return;
    // Колбэк вправе запустить следующий твин (и даже снова этот): вектор
    // колбэков забирается целиком, прежде чем их звать.
    std::vector<std::function<void()>> run;
    run.swap(m_callbacks);
    for (auto& fn : run) fn();
    if (m_callbacks.empty()) {
        run.clear();
        m_callbacks.swap(run);   // вернуть ёмкость — без аллокаций в следующем кадре
    }
}

void TweenPlayer::Seek(TweenHandle h, float time) {
    Active* a = Find(h);
    if (!a || !a->Reg->valid(a->Target)) return;
    Sync(*a);
    a->Time = a->Clip.Delay + std::max(time, 0.0f);
    Apply(*a, std::max(time, 0.0f));
}

bool TweenPlayer::Pause(TweenHandle h) {
    Active* a = Find(h);
    if (!a) return false;
    a->Paused = true;
    return true;
}

bool TweenPlayer::Resume(TweenHandle h) {
    Active* a = Find(h);
    if (!a) return false;
    a->Paused = false;
    return true;
}

bool TweenPlayer::Stop(TweenHandle h) {
    Active* a = Find(h);
    if (!a) return false;
    a->Done = true;
    a->OnComplete = nullptr;
    return true;
}

int TweenPlayer::StopFor(entt::entity target, const std::string& property) {
    int n = 0;
    for (Active& a : m_active) {
        if (a.Done || a.Target != target) continue;
        if (property.empty()) {
            a.Done = true;
            a.OnComplete = nullptr;
            ++n;
            continue;
        }
        for (size_t k = 0; k < a.Clip.Tracks.size() && k < a.Channels.size(); ++k) {
            if (a.Clip.Tracks[k].Property == property && !a.Channels[k].Missing) {
                a.Channels[k].Missing = true;
                ++n;
            }
        }
    }
    return n;
}

int TweenPlayer::PauseFor(entt::entity target, bool pause) {
    int n = 0;
    for (Active& a : m_active) {
        if (a.Done || a.Target != target) continue;
        a.Paused = pause;
        ++n;
    }
    return n;
}

void TweenPlayer::StopAll() {
    m_active.clear();
    m_callbacks.clear();
}

bool TweenPlayer::IsPlaying(TweenHandle h) const {
    const Active* a = Find(h);
    return a && !a->Paused;
}

bool TweenPlayer::IsActive(TweenHandle h) const { return Find(h) != nullptr; }

bool TweenPlayer::AnyFor(entt::entity target) const {
    for (const Active& a : m_active)
        if (!a.Done && a.Target == target) return true;
    return false;
}

float TweenPlayer::TimeOf(TweenHandle h) const {
    const Active* a = Find(h);
    return a ? std::max(a->Time - a->Clip.Delay, 0.0f) : 0.0f;
}

TweenClip* TweenPlayer::Clip(TweenHandle h) {
    Active* a = Find(h);
    return a ? &a->Clip : nullptr;
}

void TweenPlayer::SetOnComplete(TweenHandle h, std::function<void()> fn) {
    if (Active* a = Find(h)) a->OnComplete = std::move(fn);
}

// =============================================================================
//  Сцена
// =============================================================================

entt::entity TargetOf(Scene& scene, entt::entity owner, const TweenClip& clip) {
    if (clip.Target == 0) return owner;
    GameObject o = scene.Get(clip.Target);
    return o.Valid() ? o.Entity() : entt::null;
}

TweenHandle PlayNamed(Scene& scene, entt::entity owner, const std::string& name) {
    entt::registry& reg = scene.Registry();
    const TweenComponent* tc = reg.try_get<TweenComponent>(owner);
    if (!tc) return kNoTween;
    for (const TweenClip& clip : tc->Tweens) {
        if (clip.Name != name) continue;
        const entt::entity target = TargetOf(scene, owner, clip);
        return target == entt::null ? kNoTween : scene.Tweens.Play(reg, target, clip, owner);
    }
    return kNoTween;
}

void UpdateTweens(Scene& scene, float dt) {
    entt::registry& reg = scene.Registry();
    auto view = reg.view<TweenComponent>();
    for (auto e : view) {
        TweenComponent& tc = view.get<TweenComponent>(e);
        if (tc.Started) continue;
        tc.Started = true;
        for (const TweenClip& clip : tc.Tweens) {
            if (!clip.PlayOnStart) continue;
            const entt::entity target = TargetOf(scene, e, clip);
            if (target != entt::null) scene.Tweens.Play(reg, target, clip, e);
        }
    }
    scene.Tweens.Update(dt);
    // «После завершения» — следующий твин того же объекта. Здесь, а не в
    // проигрывателе: искать твин по имени можно только в сцене.
    for (const TweenPlayer::Chain& c : scene.Tweens.TakeChains())
        if (reg.valid(c.Owner)) PlayNamed(scene, c.Owner, c.Name);
}

} // namespace sage::anim
