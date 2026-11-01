#include "sage/lang/SageApi.h"

#include <algorithm>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>

#include "sage/lang/ApiRegistry.h"

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

// ЖИВОЙ КАТАЛОГ (ApiRegistry) — вторая ступень после таблицы: в нём функции,
// которых таблица знать не может, — модули игры и плагинов
// (RegisterApiFunction), зарегистрированные уже после сборки движка. Без него
// проверка называла бы их «неизвестным именем».
struct Owned {
    std::string Name, Params, Ret;
    ApiFunction View{};
};
struct Live {
    std::mutex M;
    uint64_t Version = ~0ull;
    std::unordered_map<std::string, const ApiFunction*> Functions;
    std::set<std::string, std::less<>> Modules, Globals;
    std::vector<std::string> GlobalList;
    std::deque<Owned> Storage;   // указатели отданы наружу — записи не двигаются
};

Live& TheLive() {
    static Live live;
    return live;
}

void AddLive(Live& l, const std::string& name, const ApiEntry& e) {
    std::string params;
    for (size_t i = 0; i < e.Params.size(); ++i) {
        if (i) params += ",";
        const std::string pname = e.Params[i].Name.empty() ? "arg" + std::to_string(i + 1) : e.Params[i].Name;
        params += pname + (e.Params[i].Optional ? "?:" : ":") + e.Params[i].Type;
    }
    if (e.Variadic || e.Unknown) params += params.empty() ? "..." : ",...";
    Owned& o = l.Storage.emplace_back();
    o.Name = name;
    o.Params = params;
    o.Ret = e.Returns.empty() ? "nil" : e.Returns.front();
    o.View = {o.Name.c_str(), o.Params.c_str(), o.Ret.c_str()};
    l.Functions[name] = &o.View;
    for (size_t dot = name.find('.'); dot != std::string::npos; dot = name.find('.', dot + 1))
        l.Modules.insert(name.substr(0, dot));
    const size_t dot = name.find('.');
    l.Globals.insert(dot == std::string::npos ? name : name.substr(0, dot));
}

// Пересобрать живую выборку, если каталог дописан. Под замком: проверку
// зовут и из фоновой сборки скриптов.
Live& Refreshed() {
    Live& l = TheLive();
    const uint64_t version = ApiRegistry::Instance().Version();
    if (l.Version == version) return l;
    l.Functions.clear();
    l.Modules.clear();
    l.Globals.clear();
    for (const ApiEntry& e : ApiRegistry::Instance().All()) {
        AddLive(l, e.Qualified, e);
        for (const std::string& a : e.Aliases) AddLive(l, a, e);
    }
    std::set<std::string, std::less<>> all = TheIndex().Globals;
    all.insert(l.Globals.begin(), l.Globals.end());
    l.GlobalList.assign(all.begin(), all.end());
    l.Version = version;
    return l;
}

} // namespace

const ApiFunction* FindApiFunction(std::string_view dotted) {
    const auto& f = TheIndex().Functions;
    auto it = f.find(dotted);
    if (it != f.end()) return it->second;
    Live& l = TheLive();
    std::lock_guard<std::mutex> lock(l.M);
    Refreshed();
    auto lit = l.Functions.find(std::string(dotted));
    return lit == l.Functions.end() ? nullptr : lit->second;
}

bool IsApiModule(std::string_view dotted) {
    if (TheIndex().Modules.count(dotted) > 0) return true;
    Live& l = TheLive();
    std::lock_guard<std::mutex> lock(l.M);
    return Refreshed().Modules.count(dotted) > 0;
}

bool IsKnownGlobal(std::string_view name) {
    if (TheIndex().Globals.count(name) > 0) return true;
    Live& l = TheLive();
    std::lock_guard<std::mutex> lock(l.M);
    return Refreshed().Globals.count(name) > 0;
}

const std::vector<std::string>& KnownGlobals() {
    Live& l = TheLive();
    std::lock_guard<std::mutex> lock(l.M);
    const std::vector<std::string>& list = Refreshed().GlobalList;
    return list.empty() ? TheIndex().GlobalList : list;
}

std::vector<const ApiFunction*> AllApiFunctions() {
    std::vector<const ApiFunction*> out;
    for (const ApiFunction& f : kApiFunctions) out.push_back(&f);
    return out;
}

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
