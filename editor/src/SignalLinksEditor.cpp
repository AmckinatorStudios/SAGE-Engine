#include "SignalLinksEditor.h"

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

// Заголовок события: «On Click» — переведённым, если перевод есть.
std::string TitleOf(const std::string& signal) { return T(sage::signals::Title(signal)); }

// Сигналы, которые шлют скрипты проекта (`self:emit("player_died")`), — для
// списка событий. Обход папки — не чаще раза в две секунды: инспектор
// спрашивает каждый кадр, а скрипты правят реже.
const std::vector<std::string>& ProjectEmittedSignals(EditorHost& host) {
    static std::string forDir;
    static double at = -100.0;
    static std::vector<std::string> names;
    const std::string dir = host.CurrentProject().Loaded() ? host.CurrentProject().Dir().string() : std::string();
    const double now = ImGui::GetTime();
    if (dir == forDir && now - at < 2.0) return names;
    forDir = dir;
    at = now;
    names.clear();
    if (dir.empty()) return names;
    std::error_code ec;
    std::set<std::string> seen;
    for (auto it = std::filesystem::recursive_directory_iterator(host.CurrentProject().AssetsDir(), ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string ext = it->path().extension().string();
        if (ext != ".lua" && ext != ".slua") continue;
        if (it->file_size(ec) > 512 * 1024) continue; // не скрипт, а данные
        std::ifstream in(it->path(), std::ios::binary);
        const std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        for (std::string& n : sage::scripting::ParseEmittedSignals(src))
            if (seen.insert(n).second) names.push_back(std::move(n));
    }
    return names;
}

// Список для поля события: объявленные сигналы объекта, затем — из скриптов.
std::vector<suggest::Item> EventItems(EditorHost& host, const entt::registry& reg, entt::entity e) {
    std::vector<suggest::Item> items;
    for (const sage::signals::SignalInfo& s : sage::signals::Of(reg, e))
        items.push_back({s.Name, TitleOf(s.Name) + "   " + s.Name, T("Events of this object"),
                         s.Source + ": " + T(s.Help)});
    for (const std::string& n : ProjectEmittedSignals(host)) {
        if (suggest::Known(items, n)) continue;
        items.push_back({n, TitleOf(n) + "   " + n, T("Sent by project scripts"),
                         std::string(T("A script sends it: obj:emit(\"")) + n + "\")"});
    }
    return items;
}

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
    std::string renameFrom, renameTo;
    const std::vector<suggest::Item> eventItems = EventItems(host, reg, obj.Entity());
    for (const std::string& signal : order) {
        ImGui::PushID(signal.c_str());
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(TitleOf(signal).c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(T("In a script: obj.%s:connect(fn)"), signal.c_str());
        // Имя события правится на месте — вручную или из списка: раньше
        // ошибиться в нём можно было только один раз, а исправить — лишь
        // удалив все связи события и заведя их заново.
        ImGui::SameLine();
        {
            const float plus = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
            std::string name = signal;
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - plus);
            if (suggest::Field("name", name, eventItems, "event_name") && name != signal &&
                sage::signals::IsValidName(name)) {
                renameFrom = signal;
                renameTo = name;
            }
        }
        ImGui::SameLine();
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
                ImGui::TableSetupColumn("target", ImGuiTableColumnFlags_WidthStretch, 1.4f);
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
                    std::vector<suggest::Item> methodItems;
                    for (const std::string& m : methods) methodItems.push_back({m, m + "()", {}, {}});
                    const bool known = link.Method.empty() || suggest::Known(methodItems, link.Method);
                    // Метод пишут вручную или берут из списка, прочитанного из
                    // скрипта цели. Вручную — для метода, которого в скрипте
                    // ещё нет; такая связь помечена: с опечаткой она молча не
                    // сработает, и сказать об этом надо здесь.
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
    if (!renameFrom.empty() && sl) {
        host.PushUndoSnapshot();
        for (Link& l : sl->Links)
            if (l.Signal == renameFrom) l.Signal = renameTo;
        changed = true;
    }
    if (!addTo.empty()) {
        host.PushUndoSnapshot();
        reg.get_or_emplace<SignalLinksComponent>(obj.Entity()).Links.push_back(Link{addTo, {}, {}, true, {}});
        changed = true;
    }

    // --- + Add Event ---------------------------------------------------------
    // Одно поле: имя пишут вручную (своё событие, которое шлёт скрипт:
    // `obj:emit("player_died")`) или берут из списка — сигналы объекта и те,
    // что шлют скрипты проекта. Пример в подсказке — имя из кода игры, не
    // подпись: не переводится.
    std::string chosen;
    {
        static std::string draft;
        // Недописанное имя у одного объекта не переезжает к другому.
        static entt::entity draftFor = entt::null;
        if (draftFor != obj.Entity()) {
            draft.clear();
            draftFor = obj.Entity();
        }
        const float add = ImGui::CalcTextSize(T("Add Event")).x + ImGui::GetFrameHeight() +
                          ImGui::GetStyle().FramePadding.x * 3.0f + ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - add);
        const bool picked = suggest::Field("new_event", draft, eventItems, "player_died");
        ImGui::SameLine();
        const bool valid = sage::signals::IsValidName(draft);
        ImGui::BeginDisabled(!valid);
        const bool pressed = EditorIcons::Button("plus", T("Add Event"));
        ImGui::EndDisabled();
        // Выбор из списка добавляет сразу; набранное — по Enter или кнопке.
        if (valid && (pressed || (picked && (suggest::Known(eventItems, draft) ||
                                             ImGui::IsKeyPressed(ImGuiKey_Enter) ||
                                             ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))))) {
            chosen = draft;
            draft.clear();
        }
    }
    if (order.empty()) ImGui::TextDisabled("%s", T("Nothing happens yet"));
    if (!chosen.empty()) {
        host.PushUndoSnapshot();
        reg.get_or_emplace<SignalLinksComponent>(obj.Entity()).Links.push_back(Link{chosen, {}, {}, true, {}});
        changed = true;
    }

    ImGui::PopID();
    return changed;
}

} // namespace sage::editor
