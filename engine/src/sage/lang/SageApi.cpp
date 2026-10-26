#include "sage/lang/SageApi.h"

#include <algorithm>
#include <set>
#include <unordered_map>

namespace sage::lang {
namespace {

#include "sage/lang/SageApiTable.inl"

struct Index {
    std::unordered_map<std::string_view, const ApiFunction*> Functions;
    std::set<std::string, std::less<>> Modules;   // каждый префикс до точки
    std::set<std::string, std::less<>> Globals;
    std::vector<std::string> GlobalList;
};

const Index& TheIndex() {
    static const Index idx = [] {
        Index i;
        for (const ApiFunction& f : kApiFunctions) {
            const std::string_view name = f.Name;
            i.Functions.emplace(name, &f);
            for (size_t dot = name.find('.'); dot != std::string_view::npos; dot = name.find('.', dot + 1))
                i.Modules.emplace(name.substr(0, dot));
        }
        for (const char* g : kKnownGlobals) i.Globals.emplace(g);
        // Корень каждого модуля — тоже имя, которое видит скрипт (sage, noise).
        for (const std::string& m : i.Modules)
            if (m.find('.') == std::string::npos) i.Globals.insert(m);
        i.GlobalList.assign(i.Globals.begin(), i.Globals.end());
        return i;
    }();
    return idx;
}

} // namespace

const ApiFunction* FindApiFunction(std::string_view dotted) {
    const auto& f = TheIndex().Functions;
    auto it = f.find(dotted);
    return it == f.end() ? nullptr : it->second;
}

bool IsApiModule(std::string_view dotted) { return TheIndex().Modules.count(dotted) > 0; }

bool IsKnownGlobal(std::string_view name) { return TheIndex().Globals.count(name) > 0; }

const std::vector<std::string>& KnownGlobals() { return TheIndex().GlobalList; }

std::vector<std::string> ApiModuleMembers(std::string_view module) {
    std::vector<std::string> out;
    const std::string prefix = std::string(module) + ".";
    for (const ApiFunction& f : kApiFunctions) {
        const std::string_view name = f.Name;
        if (name.size() > prefix.size() && name.compare(0, prefix.size(), prefix) == 0 &&
            name.find('.', prefix.size()) == std::string_view::npos)
            out.emplace_back(name.substr(prefix.size()));
    }
    return out;
}

std::string ClosestName(std::string_view name, const std::vector<std::string>& candidates) {
    // Расстояние Левенштейна без учёта регистра: Perlin и perlin — одна опечатка.
    auto lower = [](char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c); };
    std::vector<size_t> prev, cur;
    std::string best;
    size_t bestDist = std::max<size_t>(1, name.size() / 3) + 1;
    for (const std::string& c : candidates) {
        if (c == name) continue;
        const size_t lenDiff = c.size() > name.size() ? c.size() - name.size() : name.size() - c.size();
        if (lenDiff >= bestDist) continue;
        prev.assign(c.size() + 1, 0);
        cur.assign(c.size() + 1, 0);
        for (size_t j = 0; j <= c.size(); ++j) prev[j] = j;
        for (size_t i = 1; i <= name.size(); ++i) {
            cur[0] = i;
            for (size_t j = 1; j <= c.size(); ++j) {
                const size_t sub = prev[j - 1] + (lower(name[i - 1]) == lower(c[j - 1]) ? 0 : 1);
                cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, sub});
            }
            std::swap(prev, cur);
        }
        if (prev[c.size()] < bestDist) {
            bestDist = prev[c.size()];
            best = c;
        }
    }
    return best;
}

} // namespace sage::lang
