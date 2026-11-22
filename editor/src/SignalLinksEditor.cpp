#include "SignalLinksEditor.h"
#include "InspectorControls.h"
#include "sage/scripting/Languages.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

#include <imgui.h>

#include "EditorHost.h"
#include "EditorIcons.h"
#include "EditorTheme.h"
#include "Localization.h"
#include "ObjectSlot.h"
#include "ui/SuggestInput.h"
#include "Project.h"
#include "sage/scene/Scene.h"
#include "sage/scene/Signals.h"
#include "sage/scene/ScriptComponent.h"
#include "sage/scripting/ScriptFields.h"
#include "ui/UI.h"

namespace ictl = sage::editor::ictl;

namespace sage::editor {

namespace {

using sage::signals::Link;
using sage::signals::SignalLinksComponent;

struct CachedMethods {
    long long Stamp = -1;
    std::vector<std::string> Methods;
};

long long StampOf(const std::filesystem::path& p) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(p, ec);
    if (ec) return -1;
    return (long long)t.time_since_epoch().count();
}

// Заголовок: встроенные — «On Click», свои — имя как объявлено («Died»).
std::string TitleOf(const std::string& signal) {
    const std::string t = sage::signals::Title(signal);
    static const char* kBuiltin[] = {sage::signals::kClicked,       sage::signals::kPressed,
                                     sage::signals::kReleased,      sage::signals::kHovered,
                                     sage::signals::kUnhovered,     sage::signals::kValueChanged,
                                     sage::signals::kTextChanged,   sage::signals::kSubmitted,
                                     sage::signals::kCollision,     sage::signals::kCollisionEnded,
                                     sage::signals::kTriggerEntered, sage::signals::kTriggerExited};
    for (const char* b : kBuiltin)
        if (signal == b) return T(t);
    return signal;
}

// События, которые объявляют СКРИПТЫ ЭТОГО объекта (`self:define_event("Died")`).
// По тексту, до запуска игры; кэш по времени правки файла.
std::vector<std::string> DefinedEventsOf(EditorHost& host, const entt::registry& reg, entt::entity e) {
    std::vector<std::string> out;
    const ScriptComponent* sc = reg.try_get<ScriptComponent>(e);
    if (!sc) return out;
    static std::map<std::string, CachedMethods> cache;
    for (const ScriptEntry& entry : sc->Scripts) {
        if (entry.Path.empty()) continue;
        std::filesystem::path full = entry.Path;
        if (full.is_relative() && host.CurrentProject().Loaded()) full = host.CurrentProject().Dir() / full;
        CachedMethods& c = cache[full.string()];
        const long long stamp = StampOf(full);
        if (stamp != c.Stamp) {
            c.Stamp = stamp;
            c.Methods.clear();
            std::ifstream in(full, std::ios::binary);
            if (in) {
                const std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                c.Methods = sage::scripting::ParseDefinedEvents(src);
            }
        }
        for (const std::string& n : c.Methods)
            if (std::find(out.begin(), out.end(), n) == out.end()) out.push_back(n);
    }
    return out;
}

} // namespace

std::vector<std::string> ScriptMethodsOf(EditorHost& host, GameObject target) {
    if (!target.Valid()) return {};
    const ScriptComponent* sc = target.Registry()->try_get<ScriptComponent>(target.Entity());
    if (!sc) return {};
    // Методы ВСЕХ скриптов объекта: связь адресует метод объекта, а в игре
    // зовётся первый скрипт, где он есть (ScriptRuntime::FindMethod).
    std::vector<std::string> out;
    static std::map<std::string, CachedMethods> cache;
    for (const ScriptEntry& entry : sc->Scripts) {
        if (entry.Path.empty()) continue;
        std::filesystem::path full = entry.Path;
        if (full.is_relative() && host.CurrentProject().Loaded()) full = host.CurrentProject().Dir() / full;

        CachedMethods& c = cache[full.string()];
        const long long stamp = StampOf(full);
        if (stamp != c.Stamp) {
            c.Stamp = stamp;
            c.Methods.clear();
            std::ifstream in(full, std::ios::binary);
            if (in) {
                const std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                c.Methods = sage::scripting::ParseMethods(entry.Path, source);
            }
        }
        for (const std::string& m : c.Methods)
            if (std::find(out.begin(), out.end(), m) == out.end()) out.push_back(m);
    }
    return out;
}

bool DrawSignalLinks(EditorHost& host, GameObject obj) {
    if (!obj.Valid()) return false;
    entt::registry& reg = *obj.Registry();
    bool changed = false;
    ImGui::PushID("signal_links");

    // Какое событие открыто для правки. Один на редактор: другой объект —
    // снова список.
    static entt::entity openFor = entt::null;
    static std::string open;
    if (openFor != obj.Entity()) {
        openFor = obj.Entity();
        open.clear();
    }

    SignalLinksComponent* sl = reg.try_get<SignalLinksComponent>(obj.Entity());
    const std::vector<std::string> defined = DefinedEventsOf(host, reg, obj.Entity());

    // Список строк: события с записью в сцене (в порядке добавления), затем
    // объявленные скриптом, которых там ещё нет — они видны сразу.
    std::vector<std::string> rows;
    if (sl)
        for (const Link& l : sl->Links)
            if (std::find(rows.begin(), rows.end(), l.Signal) == rows.end()) rows.push_back(l.Signal);
    for (const std::string& n : defined)
        if (std::find(rows.begin(), rows.end(), n) == rows.end()) rows.push_back(n);
    if (!open.empty() && std::find(rows.begin(), rows.end(), open) == rows.end()) open.clear();

    auto countOf = [&](const std::string& signal) {
        int n = 0;
        if (sl)
            for (const Link& l : sl->Links)
                if (l.Signal == signal && (l.Target.Valid() || !l.Method.empty() || !l.Broadcast.empty())) ++n;
        return n;
    };

    if (open.empty()) {
        // --- Список: одна строка — одно событие -----------------------------
        std::string toOpen;
        for (const std::string& signal : rows) {
            ImGui::PushID(signal.c_str());
            const std::string count = std::to_string(countOf(signal));
            const float w = ImGui::GetContentRegionAvail().x;
            if (ImGui::Selectable("##row", false, 0, ImVec2(w, 0))) toOpen = signal;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(T("In a script: obj.%s:connect(fn)"), signal.c_str());
            ImGui::SameLine(ImGui::GetStyle().FramePadding.x);
            ImGui::TextUnformatted(TitleOf(signal).c_str());
            const float arrow = ImGui::CalcTextSize(">").x;
            ImGui::SameLine(w - ImGui::CalcTextSize(count.c_str()).x - arrow - ImGui::GetStyle().ItemSpacing.x * 2);
            ImGui::TextDisabled("%s  >", count.c_str());
            ImGui::PopID();
        }
        if (!toOpen.empty()) open = toOpen;
        if (rows.empty()) ImGui::TextDisabled("%s", T("Nothing happens yet"));

        // Встроенные события этого объекта, которых ещё нет в списке.
        std::vector<sage::signals::SignalInfo> avail;
        for (const sage::signals::SignalInfo& s : sage::signals::Of(reg, obj.Entity()))
            if (std::find(rows.begin(), rows.end(), s.Name) == rows.end()) avail.push_back(s);
        ImGui::BeginDisabled(avail.empty());
        if (EditorIcons::IconOnlyButton("plus", T("Add a built-in event of this object"))) ImGui::OpenPopup("add_event");
        ImGui::EndDisabled();
        if (ImGui::BeginPopup("add_event")) {
            for (const sage::signals::SignalInfo& s : avail) {
                if (ImGui::MenuItem(TitleOf(s.Name).c_str())) {
                    host.PushUndoSnapshot();
                    reg.get_or_emplace<SignalLinksComponent>(obj.Entity()).Links.push_back(Link{s.Name, {}, {}, true, {}});
                    changed = true;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T(s.Help));
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
        return changed;
    }

    // --- Редактирование события: список его методов -------------------------
    const std::string signal = open;
    if (EditorIcons::IconOnlyButton("back", T("Back to events"))) {
        open.clear();
        ImGui::PopID();
        return changed;
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s / %s", T("Events"), TitleOf(signal).c_str());

    int removeAt = -1;
    if (sl) {
        ImGui::BeginChild("##methods", ImVec2(0, ImGui::GetFrameHeightWithSpacing() * 6), false);
        for (size_t i = 0; i < sl->Links.size(); ++i) {
            Link& link = sl->Links[i];
            if (link.Signal != signal) continue;
            ImGui::PushID((int)i);
            // Строка связи — таблицей: слот объекта занимает всю доступную
            // ширину, и только ячейка таблицы даёт ему «пол-строки» рядом с методом.
            const ImGuiTableFlags tf = ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadOuterX;
            if (ImGui::BeginTable("##link", 4, tf)) {
                const float icon = ImGui::GetFrameHeight();
                const float check = std::ceil(icon * ImGui::GetStyle().CheckboxScale);
                ImGui::TableSetupColumn("on", ImGuiTableColumnFlags_WidthFixed, check);
                ImGui::TableSetupColumn("target", ImGuiTableColumnFlags_WidthStretch, 1.4f);
                ImGui::TableSetupColumn("method", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("trash", ImGuiTableColumnFlags_WidthFixed, icon);
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                if (ictl::Check("##on", &link.Enabled)) {
                    host.PushUndoSnapshot();
                    changed = true;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Link is on"));

                if (!link.Broadcast.empty()) {
                    // Переходник из старой сцены: связь слала событие по имени.
                    ImGui::TableSetColumnIndex(1);
                    ImGui::AlignTextToFramePadding();
                    const std::string code = "Events.emit(\"" + link.Broadcast + "\")";
                    ImGui::TextDisabled("%s", code.c_str());
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", T("A link from an older scene: it sends a global event by name.\n"
                                                 "Subscribe to it in a script with Events.on(name, fn)."));
                } else {
                    // Цель — только объект со скриптом: звать у остальных нечего.
                    ImGui::TableSetColumnIndex(1);
                    objectslot::Options opt;
                    opt.EmptyLabel = "Target";
                    opt.Accept = [](const entt::registry& r, entt::entity e) {
                        return r.all_of<ScriptComponent>(e);
                    };
                    opt.AcceptHint = "The object needs a script: the link calls a method of it";
                    const objectslot::Result r = objectslot::Draw(host, "target", link.Target.Id, opt);
                    if (r.Changed) {
                        host.PushUndoSnapshot();
                        link.Target.Id = r.Id;
                        changed = true;
                    }

                    ImGui::TableSetColumnIndex(2);
                    GameObject target = link.Target.Valid() ? host.CurrentScene().Get(link.Target.Id)
                                                            : GameObject();
                    const std::vector<std::string> methods = ScriptMethodsOf(host, target);
                    std::vector<suggest::Item> methodItems;
                    for (const std::string& m : methods) methodItems.push_back({m, m + "()", {}, {}});
                    const bool known = link.Method.empty() || suggest::Known(methodItems, link.Method);
                    // Метод, которого нет в скрипте цели, помечен: с опечаткой
                    // связь молча не сработает.
                    if (!known) ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(EditorTheme::Role::Warn));
                    std::string method = link.Method;
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (suggest::Field("method", method, methodItems, T("method")) && method != link.Method) {
                        host.PushUndoSnapshot();
                        link.Method = method;
                        changed = true;
                    }
                    if (!known) ImGui::PopStyleColor();
                    if (!known && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup))
                        ImGui::SetTooltip("%s", target.Valid() ? T("The target's script has no such method")
                                                               : T("Pick a target object first"));
                }
                ImGui::TableSetColumnIndex(3);
                if (EditorIcons::IconOnlyButton("trash", T("Remove the link"))) removeAt = (int)i;
                ImGui::EndTable();
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
    }

    if (EditorIcons::Button("plus", T("Add Method"))) {
        host.PushUndoSnapshot();
        reg.get_or_emplace<SignalLinksComponent>(obj.Entity()).Links.push_back(Link{signal, {}, {}, true, {}});
        changed = true;
    }

    if (removeAt >= 0 && sl) {
        host.PushUndoSnapshot();
        sl->Links.erase(sl->Links.begin() + removeAt);
        // Событие остаётся в списке с нулём методов: убирают метод, а не событие.
        const bool left = std::any_of(sl->Links.begin(), sl->Links.end(),
                                      [&](const Link& l) { return l.Signal == signal; });
        if (!left && std::find(defined.begin(), defined.end(), signal) == defined.end())
            sl->Links.push_back(Link{signal, {}, {}, true, {}});
        changed = true;
    }

    ImGui::PopID();
    return changed;
}

} // namespace sage::editor
