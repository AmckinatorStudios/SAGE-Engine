#include "ProblemGroups.h"

#include <algorithm>
#include <unordered_map>

namespace sage::editor::problems {

namespace {
std::string Lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}
} // namespace

bool Passes(const sage::Problem& p, const Filter& f) {
    switch (p.Level) {
        case sage::Severity::Fatal:
        case sage::Severity::Error: if (!f.Errors) return false; break;
        case sage::Severity::Warning: if (!f.Warnings) return false; break;
        case sage::Severity::Info: if (!f.Info) return false; break;
    }
    if (f.Text.empty()) return true;
    const std::string needle = Lower(f.Text);
    for (const std::string* s : {&p.Title, &p.File, &p.Subject, &p.Category})
        if (Lower(*s).find(needle) != std::string::npos) return true;
    return false;
}

std::vector<Group> Build(const std::vector<sage::Problem>& all, const Filter& f) {
    std::vector<Group> groups;
    std::unordered_map<uint64_t, size_t> byRoot;
    for (const sage::Problem& p : all) {
        if (p.Parent) continue;
        byRoot[p.Id] = groups.size();
        Group g;
        g.Root = &p;
        g.Worst = p.Level;
        groups.push_back(std::move(g));
    }
    for (const sage::Problem& p : all) {
        if (!p.Parent) continue;
        auto it = byRoot.find(p.Parent);
        if (it == byRoot.end()) continue;
        Group& g = groups[it->second];
        g.Effects.push_back(&p);
        if ((int)p.Level > (int)g.Worst) g.Worst = p.Level;
    }
    // Группа видна, если фильтр пропускает её первопричину или любое
    // последствие: искать «Player» и не найти группу, где Player — последствие,
    // значило бы спрятать причину его поломки.
    groups.erase(std::remove_if(groups.begin(), groups.end(),
                                [&](const Group& g) {
                                    if (Passes(*g.Root, f)) return false;
                                    for (const sage::Problem* e : g.Effects)
                                        if (Passes(*e, f)) return false;
                                    return true;
                                }),
                 groups.end());
    std::stable_sort(groups.begin(), groups.end(), [](const Group& a, const Group& b) {
        if (a.Worst != b.Worst) return (int)a.Worst > (int)b.Worst;
        double la = a.Root->LastSeen, lb = b.Root->LastSeen;
        for (const sage::Problem* e : a.Effects) la = std::max(la, e->LastSeen);
        for (const sage::Problem* e : b.Effects) lb = std::max(lb, e->LastSeen);
        return la > lb;
    });
    return groups;
}

} // namespace sage::editor::problems
