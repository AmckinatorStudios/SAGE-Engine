#include "AiGraphExtensions.h"

#include <algorithm>

namespace sage::editor::aigraph {

namespace {
std::vector<PinEditorExt>& Pins() {
    static std::vector<PinEditorExt> v;
    return v;
}
std::vector<ToolExt>& ToolList() {
    static std::vector<ToolExt> v;
    return v;
}
} // namespace

void AddPinEditor(PinEditorExt ext) {
    auto& v = Pins();
    // Тип с уже заданным полем — замена: последний загруженный плагин прав.
    v.erase(std::remove_if(v.begin(), v.end(), [&](const PinEditorExt& e) { return e.Type == ext.Type; }), v.end());
    v.push_back(std::move(ext));
}

void AddTool(ToolExt ext) { ToolList().push_back(std::move(ext)); }

void RemoveOwner(const std::string& owner) {
    auto& p = Pins();
    p.erase(std::remove_if(p.begin(), p.end(), [&](const PinEditorExt& e) { return e.Owner == owner; }), p.end());
    auto& t = ToolList();
    t.erase(std::remove_if(t.begin(), t.end(), [&](const ToolExt& e) { return e.Owner == owner; }), t.end());
}

const PinEditorExt* FindPinEditor(const std::string& type) {
    for (const PinEditorExt& e : Pins())
        if (e.Type == type) return &e;
    return nullptr;
}

const std::vector<ToolExt>& Tools() { return ToolList(); }

} // namespace sage::editor::aigraph
