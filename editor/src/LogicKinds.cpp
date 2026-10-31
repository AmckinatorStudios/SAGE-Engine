#include "LogicKinds.h"

#include <algorithm>

namespace sage::editor::logic {

namespace {

std::vector<Kind>& Store() {
    static std::vector<Kind> s;
    return s;
}

} // namespace

void Register(Kind kind) {
    std::vector<Kind>& s = Store();
    for (Kind& k : s)
        if (k.Id == kind.Id) {
            k = std::move(kind);
            return;
        }
    s.push_back(std::move(kind));
}

void RemoveOwner(const std::string& owner) {
    if (owner.empty()) return;   // встроенные не снимаются
    std::vector<Kind>& s = Store();
    s.erase(std::remove_if(s.begin(), s.end(), [&](const Kind& k) { return k.Owner == owner; }), s.end());
}

const std::vector<Kind>& All() { return Store(); }

const Kind* Find(const std::string& id) {
    for (const Kind& k : Store())
        if (k.Id == id) return &k;
    return nullptr;
}

std::vector<const Kind*> ForSlot(const std::string& slot) {
    std::vector<const Kind*> out;
    for (const Kind& k : Store())
        if (slot.empty() || k.Slot == slot) out.push_back(&k);
    return out;
}

} // namespace sage::editor::logic
