#include "sage/lang/ApiRegistry.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

#include <nlohmann/json.hpp>

namespace sage::lang {

namespace {

bool SameSignature(const ApiEntry& a, const ApiEntry& b) {
    if (a.Variadic != b.Variadic || a.Unknown != b.Unknown || a.Returns != b.Returns ||
        a.Params.size() != b.Params.size())
        return false;
    for (size_t i = 0; i < a.Params.size(); ++i)
        if (a.Params[i].Type != b.Params[i].Type || a.Params[i].Optional != b.Params[i].Optional) return false;
    return true;
}

// Имена, под которыми запись видна: полное и все псевдонимы.
std::vector<std::string> NamesOf(const ApiEntry& e) {
    std::vector<std::string> out{e.Qualified};
    out.insert(out.end(), e.Aliases.begin(), e.Aliases.end());
    return out;
}

std::string ParamName(const ApiParam& p, size_t i) { return p.Name.empty() ? "arg" + std::to_string(i + 1) : p.Name; }

} // namespace

std::string ApiEntry::Signature() const {
    std::string s;
    for (size_t i = 0; i < Params.size(); ++i) {
        if (i) s += ", ";
        s += ParamName(Params[i], i) + (Params[i].Optional ? "?: " : ": ") + Params[i].Type;
    }
    if (Variadic) s += Params.empty() ? "..." : ", ...";
    if (Unknown) s = "...";
    s += " -> ";
    if (Returns.empty()) s += "nil";
    for (size_t i = 0; i < Returns.size(); ++i) s += (i ? ", " : "") + Returns[i];
    return s;
}

ApiRegistry& ApiRegistry::Instance() {
    static ApiRegistry registry;
    return registry;
}

const ApiEntry* ApiRegistry::FindLocked(const std::string& name) const {
    for (const ApiEntry& e : m_entries) {
        if (e.Qualified == name) return &e;
        for (const std::string& a : e.Aliases)
            if (a == name) return &e;
    }
    return nullptr;
}

std::string ApiRegistry::Add(ApiEntry entry) {
    if (entry.Qualified.empty())
        entry.Qualified = entry.Module.empty() ? entry.Name : "sage." + entry.Module + "." + entry.Name;
    std::lock_guard<std::mutex> lock(m_mutex);
    // Та же функция ещё раз (новое состояние скриптов) — дописать недостающие
    // псевдонимы и описание, если первая регистрация их не знала.
    for (ApiEntry& e : m_entries) {
        if (e.Qualified != entry.Qualified) continue;
        if (!SameSignature(e, entry) || e.Origin != entry.Origin) {
            const std::string why = "API: «" + entry.Qualified + "» уже есть (" + e.Origin + ": " + e.Signature() +
                                    "), повтор из " + entry.Origin + " с " + entry.Signature();
            if (std::find(m_conflicts.begin(), m_conflicts.end(), why) == m_conflicts.end()) m_conflicts.push_back(why);
            return why;
        }
        for (const std::string& a : entry.Aliases)
            if (std::find(e.Aliases.begin(), e.Aliases.end(), a) == e.Aliases.end() && !FindLocked(a))
                e.Aliases.push_back(a);
        if (e.Doc.empty()) e.Doc = entry.Doc;
        e.Portable = e.Portable || entry.Portable;
        return {};
    }
    // Псевдоним, уже занятый ДРУГОЙ функцией, — конфликт имён: скрипт под этим
    // именем получит не то, что обещает подсказка. Псевдоним снимается, сама
    // запись остаётся.
    std::string result;
    std::vector<std::string> kept;
    for (const std::string& a : entry.Aliases) {
        if (const ApiEntry* other = FindLocked(a)) {
            const std::string why = "API: имя «" + a + "» занято «" + other->Qualified + "», псевдоним «" +
                                    entry.Qualified + "» не выдан";
            if (std::find(m_conflicts.begin(), m_conflicts.end(), why) == m_conflicts.end()) m_conflicts.push_back(why);
            if (result.empty()) result = why;
            continue;
        }
        if (a != entry.Qualified && std::find(kept.begin(), kept.end(), a) == kept.end()) kept.push_back(a);
    }
    entry.Aliases = std::move(kept);
    m_entries.push_back(std::move(entry));
    ++m_version;
    return result;
}

bool ApiRegistry::Find(const std::string& name, ApiEntry& out) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const ApiEntry* e = FindLocked(name);
    if (!e) return false;
    out = *e;
    return true;
}

bool ApiRegistry::Has(const std::string& name) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return FindLocked(name) != nullptr;
}

std::vector<ApiEntry> ApiRegistry::All() const {
    std::vector<ApiEntry> out;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        out = m_entries;
    }
    std::sort(out.begin(), out.end(), [](const ApiEntry& a, const ApiEntry& b) { return a.Qualified < b.Qualified; });
    return out;
}

std::vector<std::string> ApiRegistry::Modules() const {
    std::set<std::string> names;
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const ApiEntry& e : m_entries)
        if (!e.Module.empty()) names.insert(e.Module);
    return {names.begin(), names.end()};
}

std::vector<std::string> ApiRegistry::Conflicts() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_conflicts;
}

uint64_t ApiRegistry::Version() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_version;
}

int ApiRegistry::RemoveOrigin(const std::string& origin) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const size_t before = m_entries.size();
    m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(),
                                   [&](const ApiEntry& e) { return e.Origin == origin; }),
                    m_entries.end());
    const int removed = (int)(before - m_entries.size());
    if (removed) ++m_version;
    return removed;
}

void ApiRegistry::ClearForTests() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries.clear();
    m_conflicts.clear();
    ++m_version;
}

std::string ApiRegistry::ToJson() const {
    nlohmann::json list = nlohmann::json::array();
    for (const ApiEntry& e : All()) {
        nlohmann::json params = nlohmann::json::array();
        for (size_t i = 0; i < e.Params.size(); ++i)
            params.push_back({{"name", ParamName(e.Params[i], i)}, {"type", e.Params[i].Type},
                              {"optional", e.Params[i].Optional}});
        list.push_back({{"name", e.Qualified}, {"module", e.Module}, {"function", e.Name},
                        {"aliases", e.Aliases}, {"params", params}, {"variadic", e.Variadic},
                        {"unknown", e.Unknown}, {"returns", e.Returns}, {"doc", e.Doc},
                        {"origin", e.Origin}, {"portable", e.Portable}});
    }
    return nlohmann::json{{"functions", list}}.dump(1);
}

namespace {

// Тип каталога -> аннотация LuaLS.
std::string LuaType(const std::string& t) {
    if (t == "Entity") return "SageObject";
    if (t == "Quat") return "Vec4";
    if (t == "Asset") return "string";
    return t;
}

bool LuaIdent(const std::string& s) {
    if (s.empty() || std::isdigit((unsigned char)s[0])) return false;
    for (char c : s)
        if (!(std::isalnum((unsigned char)c) || c == '_')) return false;
    return true;
}

} // namespace

std::string ApiRegistry::ToLuaHints() const {
    const std::vector<ApiEntry> entries = All();
    std::string out =
        "---@meta\n"
        "-- Подсказки по API скриптов SAGE, собранные из каталога API (sage/lang/ApiRegistry.h):\n"
        "-- функции движка, модулей игры и плагинов с типами, выведенными из C++.\n"
        "-- Файл пишет движок; править руками бессмысленно.\n\n";
    std::set<std::string> modules;
    for (const ApiEntry& e : entries)
        if (!e.Module.empty()) modules.insert(e.Module);
    out += "sage = sage or {}\n";
    for (const std::string& m : modules)
        if (LuaIdent(m)) out += "---@class sage." + m + "\nsage." + m + " = sage." + m + " or {}\n";
    out += "\n";
    std::map<std::string, std::string> shortModules;   // "noise" -> "sage.noise"
    for (const ApiEntry& e : entries) {
        if (!e.Doc.empty()) {
            size_t start = 0;
            while (start <= e.Doc.size()) {
                const size_t nl = e.Doc.find('\n', start);
                out += "---" + e.Doc.substr(start, nl == std::string::npos ? std::string::npos : nl - start) + "\n";
                if (nl == std::string::npos) break;
                start = nl + 1;
            }
        }
        std::string args;
        for (size_t i = 0; i < e.Params.size(); ++i) {
            const std::string n = ParamName(e.Params[i], i);
            out += "---@param " + n + (e.Params[i].Optional ? "? " : " ") + LuaType(e.Params[i].Type) + "\n";
            args += (i ? ", " : "") + n;
        }
        if (e.Variadic || e.Unknown) {
            out += "---@param ... any\n";
            args += args.empty() ? "..." : ", ...";
        }
        for (const std::string& r : e.Returns) out += "---@return " + LuaType(r) + "\n";
        out += "function " + e.Qualified + "(" + args + ") end\n";
        for (const std::string& a : e.Aliases) {
            const size_t dot = a.find('.');
            if (dot == std::string::npos) {
                if (LuaIdent(a)) out += a + " = " + e.Qualified + "\n";
            } else if (!e.Module.empty() && a.compare(0, dot, e.Module) == 0) {
                shortModules[e.Module] = "sage." + e.Module;   // короткое имя модуля
            }
        }
        out += "\n";
    }
    for (const auto& [shortName, full] : shortModules)
        if (LuaIdent(shortName) && shortName != "math") out += shortName + " = " + full + "\n";
    return out;
}

} // namespace sage::lang
