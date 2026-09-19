#include "PathScope.h"

namespace fs = std::filesystem;

namespace sage::editor::pathscope {

bool Within(const fs::path& root, const fs::path& p) {
    if (root.empty()) return true;

    std::error_code ec;
    const fs::path r = fs::weakly_canonical(fs::absolute(root, ec), ec).lexically_normal();
    const fs::path q = fs::weakly_canonical(fs::absolute(p, ec), ec).lexically_normal();

    // ПОЭЛЕМЕНТНО, а не по началу строки: у «…/assets2» то же начало, что у
    // «…/assets». Заодно это единственный способ не зависеть от того, чем
    // разделены части пути на этой системе.
    auto ri = r.begin();
    auto qi = q.begin();
    for (; ri != r.end(); ++ri, ++qi) {
        if (qi == q.end()) return false;   // путь КОРОЧЕ границы — он выше неё
        if (*qi != *ri) return false;
    }
    return true;
}

} // namespace sage::editor::pathscope
