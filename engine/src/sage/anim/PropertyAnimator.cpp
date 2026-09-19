#include "sage/anim/PropertyAnimator.h"

#include <algorithm>

#include "sage/anim/AnimProperty.h"
#include "sage/core/Log.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"

namespace sage::anim {

namespace {

// Один шаг пути: имя до ближайшей «/».
std::string Head(const std::string& path, size_t& pos) {
    const size_t slash = path.find('/', pos);
    const std::string head =
        slash == std::string::npos ? path.substr(pos) : path.substr(pos, slash - pos);
    pos = slash == std::string::npos ? path.size() : slash + 1;
    return head;
}

} // namespace

entt::entity ResolveTarget(Scene& scene, entt::entity owner, const std::string& path) {
    if (path.empty()) return owner;
    entt::registry& reg = scene.Registry();
    entt::entity cur = owner;
    size_t pos = 0;
    while (pos < path.size()) {
        const std::string name = Head(path, pos);
        if (name.empty()) continue;
        const HierarchyComponent* h = reg.try_get<HierarchyComponent>(cur);
        if (!h) return entt::null;
        entt::entity next = entt::null;
        for (entt::entity c : h->Children) {
            const NameComponent* n = reg.try_get<NameComponent>(c);
            if (n && n->Name == name) { next = c; break; }
        }
        if (next == entt::null) return entt::null;
        cur = next;
    }
    return cur;
}

void ApplyClipAt(Scene& scene, entt::entity owner, const PropertyClip& clip, float time) {
    entt::registry& reg = scene.Registry();
    for (const Track& t : clip.Tracks) {
        if (t.Keys.empty() || t.Property.empty()) continue;
        const PropertyType* prop = FindProperty(t.Property);
        // Свойства нет в этой сборке игры — дорожка не ошибка, а чужие данные:
        // клип мог прийти из проекта, где есть своя часть. Молча пропускаем,
        // и в файле она останется нетронутой.
        if (!prop) continue;
        const entt::entity target = ResolveTarget(scene, owner, t.Target);
        if (target == entt::null || !reg.valid(target)) continue;
        if (!HasProperty(*prop, reg, target)) continue;
        WriteProperty(*prop, reg, target, Sample(t, time));
    }
}

void UpdatePropertyAnimators(Scene& scene, float dt) {
    entt::registry& reg = scene.Registry();
    auto view = reg.view<PropertyAnimatorComponent>();
    for (entt::entity e : view) {
        PropertyAnimatorComponent& a = view.get<PropertyAnimatorComponent>(e);
        if (!a.Ready) {
            a.Ready = true;
            if (!a.ClipPath.empty()) {
                auto clip = std::make_shared<PropertyClip>();
                std::string err;
                if (LoadClipFile(a.ClipPath, *clip, err)) {
                    a.Clip = std::move(clip);
                } else {
                    // Сказать вслух и один раз: путь есть, а файла нет — это
                    // опечатка или потерянный ассет, и молчание тут означает
                    // «кнопка не мигает, и непонятно почему».
                    LOG_WARN("Anim") << "Клип не загрузился (" << a.ClipPath << "): " << err;
                }
            }
        }
        if (!a.Clip) continue;
        const PropertyClip& clip = *a.Clip;
        if (a.Playing && dt > 0.0f) a.Time += dt * a.Speed;
        a.Time = WrapTime(a.Time, clip.Duration, a.Loop);
        ApplyClipAt(scene, e, clip, a.Time);
    }
}

} // namespace sage::anim
