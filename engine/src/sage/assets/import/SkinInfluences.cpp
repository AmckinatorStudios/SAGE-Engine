#include "sage/assets/import/SkinInfluences.h"

#include <algorithm>

#include "sage/anim/Skeleton.h"

namespace sage::assets {

bool ResolveInfluences(std::vector<SkinInfluence>& influences, int jointCount,
                       glm::vec4& outJoints, glm::vec4& outWeights) {
    // Палитра шейдера короче скелета у моделей с сотнями костей — номер за её
    // пределами читает чужую память уже на видеокарте, где не будет ни
    // исключения, ни лога, а будет чёрный экран или вылет драйвера.
    const int limit = std::min(jointCount, sage::anim::kMaxBones);

    influences.erase(std::remove_if(influences.begin(), influences.end(),
                                    [&](const SkinInfluence& i) {
                                        return i.Weight <= 0.0f || i.Joint < 0 || i.Joint >= limit;
                                    }),
                     influences.end());
    if (influences.empty()) return false;

    // Устойчивая сортировка: при равных весах порядок остаётся файловым, и
    // одна и та же модель разбирается одинаково от запуска к запуску.
    std::stable_sort(influences.begin(), influences.end(),
                     [](const SkinInfluence& a, const SkinInfluence& b) {
                         return a.Weight > b.Weight;
                     });

    outJoints = glm::vec4(0.0f);
    outWeights = glm::vec4(0.0f);
    float sum = 0.0f;
    const size_t take = std::min<size_t>(influences.size(), 4);
    for (size_t k = 0; k < take; ++k) {
        outJoints[(int)k] = (float)influences[k].Joint;
        outWeights[(int)k] = influences[k].Weight;
        sum += influences[k].Weight;
    }
    if (sum <= 1e-6f) return false;
    outWeights /= sum;
    return true;
}

} // namespace sage::assets
