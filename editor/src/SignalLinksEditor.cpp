#include "SignalLinksEditor.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>

#include <imgui.h>

#include "EditorHost.h"
#include "EditorIcons.h"
#include "EditorTheme.h"
#include "Localization.h"
#include "ObjectSlot.h"
#include "Project.h"
#include "sage/scene/Scene.h"
#include "sage/scene/Signals.h"
#include "sage/scripting/ScriptComponent.h"
#include "sage/scripting/ScriptFields.h"
#include "ui/UI.h"

namespace sage::editor {

namespace {

using sage::signals::Link;
using sage::signals::SignalLinksComponent;

// Имя всплывающего окна — одно на OpenPopup и BeginPopup (см. CLAUDE.md).
constexpr const char* kAddEventPopup = "###sage_add_event";

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

// Заголовок события: «On Click» — переведённым, если перевод есть.
std::string TitleOf(const std::string& signal) { return T(sage::signals::Title(signal)); }

} // namespace

std::vector<std::string> ScriptMethodsOf(EditorHost& host, GameObject target) {
    if (!target.Valid()) return {};
    const ScriptComponent* sc = target.Registry()->try_get<ScriptComponent>(target.Entity());
    if (!sc || sc->Path.empty()) return {};
    std::filesystem::path full = sc->Path;
    if (full.is_relative() && host.CurrentProject().Loaded()) full = host.CurrentProject().Dir() / full;

    static std::map<std::string, CachedMethods> cache;
    CachedMethods& c = cache[full.string()];
    const long long stamp = StampOf(full);
    if (stamp == c.Stamp) return c.Methods;
    c.Stamp = stamp;
    c.Methods.clear();
    std::ifstream in(full, std::ios::binary);
    if (!in) return {};
    const std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    c.Methods = sage::scripting::ParseMethods(sc->Path, source);
    return c.Methods;
}

bool DrawSignalLinks(EditorHost& host, GameObject obj) {
    if (!obj.Valid()) return false;
    entt::registry& reg = *obj.Registry();
    bool changed = false;
    ImGui::PushID("signal_links");

    SignalLinksComponent* sl = reg.try_get<SignalLinksComponent>(obj.Entity());

    // Порядок групп — порядок первой связи каждого события: человек видит
    // события в том порядке, в каком он их добавлял.
    std::vector<std::string> order;
    if (sl)
        for (const Link& l : sl->Links)
            if (std::find(order.begin(), order.end(), l.Signal) == order.end()) order.push_back(l.Signal);

    int removeAt = -1;
    std::string addTo;   // «+» у события: ещё один метод на то же событие
    for (const std::string& signal : order) {
        ImGui::PushID(signal.c_str());
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(TitleOf(signal).c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(T("In a script: obj.%s:connect(fn)"), signal.c_str());
        ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - ImGui::GetFrameHeight());
        if (EditorIcons::IconOnlyButton("plus", T("Call one more method on this event"))) addTo = signal;

        ImGui::Indent();
        for (size_t i = 0; i < sl->Links.size(); ++i) {
            Link& link = sl->Links[i];
            if (link.Signal != signal) continue;
            ImGui::PushID((int)i);

            // Строка связи — таблицей: слот объекта занимает всю доступную
            // ширину, и только ячейка таблицы даёт ему честные «пол-строки»
            // рядом с методом.
            const ImGuiTableFlags tf = ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadOuterX;
            if (ImGui::BeginTable("##link", 4, tf)) {
                const float icon = ImGui::GetFrameHeight();
                // Колонка галочки — по самой галочке (она меньше поля, см.
                // CheckboxScale): колонка в высоту поля оставляла справа от
                // неё пустую полосу, и слот цели отъезжал от своей галки.
                const float check = std::ceil(icon * ImGui::GetStyle().CheckboxScale);
                ImGui::TableSetupColumn("on", ImGuiTableColumnFlags_WidthFixed, check);
                ImGui::TableSetupColumn("target", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("method", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("trash", ImGuiTableColumnFlags_WidthFixed, icon);
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                if (ImGui::Checkbox("##on", &link.Enabled)) {
                    host.PushUndoSnapshot();
                    changed = true;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Link is on"));

                if (!link.Broadcast.empty()) {
                    // Переходник из старой сцены: связь слала событие по имени.
                    // Изменить её нельзя (так связи больше не заводят), убрать — можно.
                    ImGui::TableSetColumnIndex(1);
                    ImGui::AlignTextToFramePadding();
                    // Код, а не подпись: переводить имя функции нельзя.
                    const std::string code = "Events.emit(\"" + link.Broadcast + "\")";
                    ImGui::TextDisabled("%s", code.c_str());
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", T("A link from an older scene: it sends a global event by name.\n"
                                                 "Subscribe to it in a script with Events.on(name, fn)."));
                } else {
                    // Цель — только объект со скриптом: у объекта без скрипта
                    // звать нечего, и дать его выбрать значит завести связь,
                    // которая заведомо не сработает.
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
                    const bool known = link.Method.empty() ||
                                       std::find(methods.begin(), methods.end(), link.Method) != methods.end();
                    // Метода, которого в скрипте нет, — «(!)» и подсказка: связь с
                    // опечаткой молча не сработает, и сказать об этом надо здесь.
                    const std::string preview =
                        link.Method.empty() ? std::string(T("Method"))
                                            : link.Method + (known ? "()" : "()  (!)");
                    ImGui::SetNextItemWidth(-1.0f);
                    if (ImGui::BeginCombo("##method", preview.c_str())) {
                        if (!target.Valid())
                            ImGui::TextDisabled("%s", T("Pick a target object first"));
                        else if (methods.empty())
                            ImGui::TextDisabled("%s", T("The script declares no methods"));
                        for (const std::string& m : methods) {
                            const bool sel = m == link.Method;
                            if (ImGui::Selectable((m + "()").c_str(), sel)) {
                                host.PushUndoSnapshot();
                                link.Method = m;
                                changed = true;
                            }
                            if (sel) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    } else if (!known && ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", T("The target's script has no such method"));
                    }
                }
                ImGui::TableSetColumnIndex(3);
                if (EditorIcons::IconOnlyButton("trash", T("Remove the link"))) removeAt = (int)i;
                ImGui::EndTable();
            }
            ImGui::PopID();
        }
        ImGui::Unindent();
        ImGui::PopID();
    }

    if (removeAt >= 0 && sl) {
        host.PushUndoSnapshot();
        sl->Links.erase(sl->Links.begin() + removeAt);
        if (sl->Links.empty()) reg.remove<SignalLinksComponent>(obj.Entity());
        sl = reg.try_get<SignalLinksComponent>(obj.Entity());
        changed = true;
    }
    if (!addTo.empty()) {
        host.PushUndoSnapshot();
        reg.get_or_emplace<SignalLinksComponent>(obj.Entity()).Links.push_back(Link{addTo, {}, {}, true, {}});
        changed = true;
    }

    // --- + Add Event ---------------------------------------------------------
    if (EditorIcons::Button("plus", T("Add Event"))) ImGui::OpenPopup(kAddEventPopup);
    if (order.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", T("Nothing happens yet"));
    }
    std::string chosen;
    {
        Sage::UI::MenuScope menu;
        if (ImGui::BeginPopup(kAddEventPopup)) {
            const std::vector<sage::signals::SignalInfo> declared = sage::signals::Of(reg, obj.Entity());
            if (!declared.empty()) {
                Sage::UI::MenuSection(T("Events of this object"), true);
                for (const sage::signals::SignalInfo& s : declared) {
                    if (ImGui::MenuItem(TitleOf(s.Name).c_str())) chosen = s.Name;
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s: %s\n%s", s.Source.c_str(), s.Name.c_str(), T(s.Help));
                }
            }
            // Своё событие: его шлёт скрипт (`obj:emit("player_died")`), и
            // движок заранее знать его не может.
            Sage::UI::MenuSection(T("Custom event"), declared.empty());
            static char custom[64] = "";
            ImGui::SetNextItemWidth(160.0f);
            // Пример — имя из кода игры, а не подпись: не переводится.
            const char* example = "player_died";
            const bool enter = ImGui::InputTextWithHint("##custom", example, custom, sizeof(custom),
                                                        ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            const bool valid = sage::signals::IsValidName(custom);
            if (!valid) ImGui::BeginDisabled();
            if ((ImGui::Button(T("Add")) || enter) && valid) {
                chosen = custom;
                custom[0] = 0;
            }
            if (!valid) ImGui::EndDisabled();
            if (!chosen.empty()) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
    if (!chosen.empty()) {
        host.PushUndoSnapshot();
        reg.get_or_emplace<SignalLinksComponent>(obj.Entity()).Links.push_back(Link{chosen, {}, {}, true, {}});
        changed = true;
    }

    ImGui::PopID();
    return changed;
}

} // namespace sage::editor
