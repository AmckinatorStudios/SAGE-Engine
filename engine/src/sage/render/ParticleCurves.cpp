#include "sage/render/ParticleCurves.h"

#include <algorithm>

namespace sage::fx {

namespace {

// Общая интерполяция по отсортированным ключам: за краями — крайнее значение.
template <typename Key, typename T, typename Get>
T Sample(const std::vector<Key>& keys, float t, Get get) {
    if (keys.size() == 1 || t <= keys.front().T) return get(keys.front());
    if (t >= keys.back().T) return get(keys.back());
    for (size_t i = 1; i < keys.size(); ++i) {
        if (t > keys[i].T) continue;
        const float span = keys[i].T - keys[i - 1].T;
        const float u = span > 1e-6f ? (t - keys[i - 1].T) / span : 1.0f;
        return glm::mix(get(keys[i - 1]), get(keys[i]), u);
    }
    return get(keys.back());
}

} // namespace

float Curve::Evaluate(float t) const {
    if (Keys.empty()) return 1.0f;
    if (Keys.size() == 1 || t <= Keys.front().x) return Keys.front().y;
    if (t >= Keys.back().x) return Keys.back().y;
    for (size_t i = 1; i < Keys.size(); ++i) {
        if (t > Keys[i].x) continue;
        const float span = Keys[i].x - Keys[i - 1].x;
        const float u = span > 1e-6f ? (t - Keys[i - 1].x) / span : 1.0f;
        return glm::mix(Keys[i - 1].y, Keys[i].y, u);
    }
    return Keys.back().y;
}

void Curve::Sort() {
    std::stable_sort(Keys.begin(), Keys.end(),
                     [](const glm::vec2& a, const glm::vec2& b) { return a.x < b.x; });
}

glm::vec4 Gradient::Evaluate(float t) const {
    const glm::vec3 c = Colors.empty()
                            ? glm::vec3(1.0f)
                            : Sample<ColorKey, glm::vec3>(Colors, t, [](const ColorKey& k) { return k.Color; });
    const float a = Alphas.empty()
                        ? 1.0f
                        : Sample<AlphaKey, float>(Alphas, t, [](const AlphaKey& k) { return k.Alpha; });
    return glm::vec4(c, a);
}

void Gradient::Sort() {
    std::stable_sort(Colors.begin(), Colors.end(),
                     [](const ColorKey& a, const ColorKey& b) { return a.T < b.T; });
    std::stable_sort(Alphas.begin(), Alphas.end(),
                     [](const AlphaKey& a, const AlphaKey& b) { return a.T < b.T; });
}

} // namespace sage::fx
