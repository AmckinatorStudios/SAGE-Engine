#include "ui/SuggestInput.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

#include <imgui.h>

#include "../Localization.h"
#include "ui/UI.h"

namespace sage::editor::suggest {

namespace {

// Имя списка — одно на OpenPopup и BeginPopup (см. CLAUDE.md); своё у каждого
// поля даёт PushID(id) вокруг обоих вызовов.
constexpr const char* kListPopup = "###sage_suggest_list";

std::string Lower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return out;
}

} // namespace

std::vector<int> Rank(const std::vector<Item>& items, const std::string& typed) {
    const std::string t = Lower(typed);
    auto score = [&](const Item& it) {
        if (t.empty()) return 3;
        const std::string v = Lower(it.Value);
        const std::string l = Lower(it.Label);
        if (v == t) return 0;
        if (v.rfind(t, 0) == 0 || l.rfind(t, 0) == 0) return 1;
        if (v.find(t) != std::string::npos || l.find(t) != std::string::npos) return 2;
        return 3;
    };
    std::vector<int> order((size_t)items.size());
    for (size_t i = 0; i < items.size(); ++i) order[i] = (int)i;
    std::stable_sort(order.begin(), order.end(),
                     [&](int a, int b) { return score(items[(size_t)a]) < score(items[(size_t)b]); });
    return order;
}

bool Known(const std::vector<Item>& items, const std::string& value) {
    return std::any_of(items.begin(), items.end(), [&](const Item& it) { return it.Value == value; });
}

bool Field(const char* id, std::string& value, const std::vector<Item>& items, const char* hint) {
    ImGui::PushID(id);
    bool committed = false;

    // Буфер правки живёт, пока поле в фокусе; вне фокуса поле показывает
    // само значение — иначе правка снаружи (отмена, выбор из списка) не была
    // бы видна до следующего щелчка.
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s", value.c_str());
    const float arrow = ImGui::GetFrameHeight();
    const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
    // Ширина — как у обычного поля (SetNextItemWidth / PushItemWidth снаружи),
    // стрелка входит в неё, а не вылезает за край.
    const float total = ImGui::CalcItemWidth();
    ImGui::SetNextItemWidth(std::max(40.0f, total - arrow - spacing));
    if (ImGui::InputTextWithHint("##text", hint, buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
        value = buf;
        committed = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        value = buf;
        committed = true;
    }
    // Набранное, но ещё не принятое: по нему упорядочен список.
    const std::string typed = ImGui::IsItemActive() ? std::string(buf) : value;

    ImGui::SameLine(0.0f, spacing);
    if (ImGui::ArrowButton("##open", ImGuiDir_Down)) ImGui::OpenPopup(kListPopup);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Pick from the list"));

    if (Sage::UI::MenuScope menu; ImGui::BeginPopup(kListPopup)) {
        if (items.empty()) ImGui::TextDisabled("%s", T("Nothing to suggest — type the name"));
        std::string group = "\x01"; // ни с чем не совпадает: первый раздел всегда с заголовком
        for (int i : Rank(items, typed)) {
            const Item& it = items[(size_t)i];
            // Разделы — в порядке ранга: заголовок, только когда раздел сменился.
            if (it.Group != group) {
                group = it.Group;
                if (!group.empty()) Sage::UI::MenuSection(group.c_str());
            }
            ImGui::PushID(i);
            const std::string& label = it.Label.empty() ? it.Value : it.Label;
            if (ImGui::Selectable(label.c_str(), it.Value == value)) {
                value = it.Value;
                committed = true;
            }
            if (!it.Tip.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", it.Tip.c_str());
            ImGui::PopID();
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return committed;
}

} // namespace sage::editor::suggest
