#include "sage/core/SystemAccess.h"

#include <algorithm>
#include <mutex>
#include <set>
#include <unordered_map>
#include <utility>

#include "sage/core/Log.h"

namespace sage {

namespace {

struct NameTable {
    std::mutex M;
    std::unordered_map<std::string, ComponentId> Ids;
    std::vector<std::string> Names;
};
NameTable& Names() {
    static NameTable* t = new NameTable();  // живёт до конца процесса
    return *t;
}

struct ThreadAccess {
    const SystemAccess* Access = nullptr;
    const char* Name = nullptr;
};
ThreadAccess& Tls() {
    thread_local ThreadAccess t;
    return t;
}

std::mutex g_violationM;
std::set<std::pair<std::string, ComponentId>> g_reported;
int g_violations = 0;

template <class T>
bool Intersects(const std::vector<T>& a, const std::vector<T>& b) {
    for (const T& x : a)
        if (std::find(b.begin(), b.end(), x) != b.end()) return true;
    return false;
}

} // namespace

ComponentId RegisterComponentAccessName(const char* typeName) {
    NameTable& t = Names();
    std::lock_guard<std::mutex> lk(t.M);
    auto it = t.Ids.find(typeName);
    if (it != t.Ids.end()) return it->second;
    const ComponentId id = (ComponentId)t.Names.size();
    t.Names.push_back(typeName);
    t.Ids.emplace(typeName, id);
    return id;
}

std::string ComponentAccessName(ComponentId id) {
    NameTable& t = Names();
    std::lock_guard<std::mutex> lk(t.M);
    return id < t.Names.size() ? t.Names[id] : std::string("?");
}

bool SystemAccess::ConflictsWith(const SystemAccess& o) const {
    if (Exclusive || o.Exclusive) return true;
    if (Intersects(Writes, o.Writes) || Intersects(Writes, o.Reads) || Intersects(Reads, o.Writes))
        return true;
    if (Intersects(WritesResources, o.WritesResources) || Intersects(WritesResources, o.ReadsResources) ||
        Intersects(ReadsResources, o.WritesResources))
        return true;
    return false;
}

bool SystemAccess::Allows(ComponentId id, bool write) const {
    if (Exclusive) return true;
    if (std::find(Writes.begin(), Writes.end(), id) != Writes.end()) return true;
    return !write && std::find(Reads.begin(), Reads.end(), id) != Reads.end();
}

std::string SystemAccess::Describe() const {
    if (Exclusive) return MainThread ? "exclusive, main thread" : "exclusive";
    std::string s = MainThread ? "main thread" : "workers";
    auto list = [&](const char* tag, const std::vector<ComponentId>& ids) {
        if (ids.empty()) return;
        s += std::string("; ") + tag + ":";
        for (ComponentId id : ids) s += " " + ComponentAccessName(id);
    };
    auto listRes = [&](const char* tag, const std::vector<std::string>& names) {
        if (names.empty()) return;
        s += std::string("; ") + tag + ":";
        for (const std::string& n : names) s += " " + n;
    };
    list("reads", Reads);
    list("writes", Writes);
    listRes("reads res", ReadsResources);
    listRes("writes res", WritesResources);
    return s;
}

const SystemAccess* CurrentSystemAccess() { return Tls().Access; }
const char* CurrentSystemName() { return Tls().Name ? Tls().Name : ""; }

ScopedSystemAccess::ScopedSystemAccess(const SystemAccess* access, const char* name)
    : m_prevAccess(Tls().Access), m_prevName(Tls().Name) {
    Tls().Access = access;
    Tls().Name = name;
}

ScopedSystemAccess::~ScopedSystemAccess() {
    Tls().Access = m_prevAccess;
    Tls().Name = m_prevName;
}

bool CheckComponentAccess(ComponentId id, bool write) {
    const SystemAccess* a = Tls().Access;
    if (!a || a->Allows(id, write)) return true;
    const std::string system = CurrentSystemName();
    std::lock_guard<std::mutex> lk(g_violationM);
    ++g_violations;
    if (g_reported.emplace(system, id).second) {
        LOG_ERROR("Systems") << "Система '" << system << "' " << (write ? "пишет" : "читает")
                             << " компонент " << ComponentAccessName(id)
                             << ", не объявив этого: параллельно с ней может идти система, "
                                "которая его меняет (гонка данных)";
    }
    return false;
}

int ComponentAccessViolations() {
    std::lock_guard<std::mutex> lk(g_violationM);
    return g_violations;
}

} // namespace sage
