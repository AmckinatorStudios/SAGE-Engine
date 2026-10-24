#include "PluginsWindow.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_map>

#include "imgui.h"

#include "EditorHost.h"
#include "EditorIcons.h"
#include "EditorPrefs.h"
#include "FileBrowser.h"
#include "FsCache.h"
#include "Localization.h"
#include "LuaPlugins.h"
#include "PluginManager.h"
#include "Project.h"
#include "ProjectLauncher/ProjectDatabase.h"
#include "sage/core/Paths.h"
#include "sage/scripting/ScriptFields.h"

namespace fs = std::filesystem;
namespace lp = sage::editor::luaplugins;

namespace sage::editor::pluginswindow {

namespace {

enum class Filter { All, Project, User, Builtin, Native };

struct State {
    bool Open = false;
    bool CreatePending = false;
    bool RemovePending = false;
    std::string Selected;          // id плагина Lua или "native:<файл>"
    Filter Show = Filter::All;
    // «Создать плагин»
    char Name[96] = {0};
    char Author[64] = {0};
    int Kind = 0;                  // 0 — редактор, 1 — игра, 2 — оба
    bool InProject = true;
    std::string CreateError;
    // «Установить»
    FileBrowser Browser;
    bool InstallToProject = true;
    std::string Message;
};

State& S() {
    static State s;
    return s;
}

constexpr const char* kCreatePopup = "###plugin_create";
constexpr const char* kRemovePopup = "###plugin_remove";

const char* SourceLabel(lp::Source from) {
    switch (from) {
        case lp::Source::Project: return T("project");
        case lp::Source::Builtin: return T("built-in");
        case lp::Source::User:
        default: return T("mine");
    }
}

bool Matches(Filter f, lp::Source from) {
    switch (f) {
        case Filter::Project: return from == lp::Source::Project;
        case Filter::User: return from == lp::Source::User;
        case Filter::Builtin: return from == lp::Source::Builtin;
        case Filter::Native: return false;
        default: return true;
    }
}

// Следующая кнопка — в ту же строку, если влезает, иначе с новой: окно узкое,
// а кнопок у плагина до шести, и обрезанная кнопка справа не нажимается.
void Next(const char* label) {
    const float w = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f +
                    ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
    const float right = ImGui::GetItemRectMax().x + w;
    const float limit = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    if (right < limit) ImGui::SameLine();
}

std::string NativeKey(const fs::path& file) { return "plugins.native." + sage::PathToUtf8(file.filename()); }

std::string ReadAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Настройки = объявленные в runtime.lua переменные + значения из plugin.json.
// Объявление даёт ВИД и умолчание (как у скрипта объекта в инспекторе), файл —
// то, что выбрал человек.
//
// ОБЪЯВЛЕНИЕ — ИЗ КЭША. Окно рисуется каждый кадр, и чтение с разбором
// runtime.lua шестьдесят раз в секунду ради неизменного файла — это диск и
// разбор Lua в каждом кадре, пока окно открыто. Перечитываем, только когда
// файл поменялся (штамп времени — тоже из кэша, раз в секунду).
sage::vars::Table SettingsOf(const sage::plugins::Manifest& m) {
    sage::vars::Table t = m.Settings;
    if (m.HasRuntime()) {
        struct Declared { long long Stamp = -1; sage::vars::Table Fields; };
        static std::unordered_map<std::string, Declared> cache;
        const fs::path file = m.RuntimePath();
        const long long stamp = sage::editor::fscache::Stamp(file);
        Declared& d = cache[sage::PathToUtf8(file)];
        if (d.Stamp != stamp) {
            d.Fields = sage::scripting::ParseFields(sage::PathToUtf8(file), ReadAll(file));
            d.Stamp = stamp;
        }
        t.MergeDeclaration(d.Fields);
    }
    return t;
}

// Файлы плагинов C++ — из кэша списка папки, а не перебором каталога в каждом кадре.
std::vector<fs::path> NativeFiles(const fs::path& dir) {
    std::vector<fs::path> out;
    if (dir.empty()) return out;
    for (const fs::path& f : sage::editor::fscache::List(dir).Files)
        if (f.extension() == PluginManager::Extension()) out.push_back(f);
    return out;
}

// Одна строка настройки. true — значение поменяли (сохранить).
bool DrawSetting(sage::vars::Var& v) {
    ImGui::PushID(v.Name.c_str());
    bool changed = false;
    const std::string label = v.Title();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
    switch (v.Data.Type()) {
        case sage::vars::Kind::Bool: {
            bool b = v.Data.AsBool();
            if (ImGui::Checkbox(label.c_str(), &b)) {
                v.Data = sage::vars::Value(b);
                changed = true;
            }
            break;
        }
        case sage::vars::Kind::Int: {
            int i = v.Data.AsInt();
            ImGui::InputInt(label.c_str(), &i);
            v.Data = sage::vars::Value(i);
            changed = ImGui::IsItemDeactivatedAfterEdit();
            break;
        }
        case sage::vars::Kind::Float: {
            float f = v.Data.AsFloat();
            ImGui::DragFloat(label.c_str(), &f, 0.05f);
            v.Data = sage::vars::Value(f);
            changed = ImGui::IsItemDeactivatedAfterEdit();
            break;
        }
        case sage::vars::Kind::String: {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%s", v.Data.AsString().c_str());
            ImGui::InputText(label.c_str(), buf, sizeof(buf));
            v.Data = sage::vars::Value(std::string(buf));
            changed = ImGui::IsItemDeactivatedAfterEdit();
            break;
        }
        default:
            ImGui::TextDisabled("%s: %s", label.c_str(), T("edit in plugin.json"));
            break;
    }
    if (!v.Tooltip.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", v.Tooltip.c_str());
    ImGui::PopID();
    return changed;
}

void DrawLuaDetails(EditorHost& host, const lp::PluginInfo& info) {
    State& s = S();
    const sage::plugins::Manifest& m = info.Manifest;
    ImGui::TextUnformatted(m.Title().c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%s · %s", m.Version.c_str(), SourceLabel(info.From));
    if (!m.Author.empty()) ImGui::TextDisabled("%s %s", T("Author:"), m.Author.c_str());
    if (!m.Description.empty()) ImGui::TextWrapped("%s", m.Description.c_str());
    ImGui::Spacing();

    // Что внутри — двумя строками: человек должен знать, поедет ли плагин в игру.
    if (m.HasEditor())
        ImGui::BulletText(T("Editor part: %s (menu items: %d, windows: %d)"), m.Editor.c_str(), info.Menus,
                          info.Panels);
    if (m.HasRuntime()) ImGui::BulletText(T("Game part: %s — runs in Play and in the built game"), m.Runtime.c_str());
    if (info.From != lp::Source::Project && m.HasRuntime())
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1.0f), "%s",
                           T("The game part works only for project plugins: install it into the project."));

    if (!info.Error.empty()) {
        ImGui::Spacing();
        EditorIcons::Inline("error", glm::vec3(1.0f, 0.45f, 0.45f));
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", T("Error:"));
        ImGui::TextWrapped("%s", info.Error.c_str());
    } else if (info.Enabled && m.HasEditor()) {
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.5f, 1.0f), "%s", info.Running ? T("Running") : T("Not running"));
    }

    ImGui::Spacing();
    if (EditorIcons::Button("refresh", T("Reload"))) {
        lp::Reload(host, m.Id);
        s.Message = std::string(T("Plugin reloaded: ")) + m.Title();
    }
    Next(T("Open folder"));
    if (EditorIcons::Button("folder", T("Open folder"))) Sage::Launcher::OpenWithSystem(m.Dir);
    if (m.HasEditor()) {
        Next("editor.lua");
        if (EditorIcons::Button("code", "editor.lua")) host.OpenFileInSystemEditor(m.EditorPath());
    }
    if (m.HasRuntime()) {
        Next("runtime.lua");
        if (EditorIcons::Button("script", "runtime.lua")) host.OpenFileInSystemEditor(m.RuntimePath());
    }
    if (info.From != lp::Source::Builtin) {
        Next(T("Remove"));
        if (EditorIcons::Button("trash", T("Remove"))) s.RemovePending = true;
    }

    // Настройки — правятся прямо здесь, пишутся в plugin.json.
    sage::vars::Table settings = SettingsOf(m);
    if (!settings.Empty()) {
        ImGui::Spacing();
        ImGui::SeparatorText(T("Settings"));
        bool changed = false;
        for (sage::vars::Var& v : settings.All()) changed |= DrawSetting(v);
        if (changed) {
            std::string err;
            if (lp::SaveSettings(host, m.Id, settings, err)) s.Message = T("Settings saved to plugin.json");
            else s.Message = err;
        }
        ImGui::TextDisabled("%s", T("The game part picks the settings up on the next Play."));
    }
}

void DrawNativeDetails(const fs::path& file, const Native& native) {
    ImGui::TextUnformatted(sage::PathToUtf8(file.filename()).c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%s", T("C++ plugin"));
    const std::string name = native.Manager ? native.Manager->NameOf(file) : std::string();
    if (!name.empty()) ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.5f, 1.0f), T("Loaded: %s"), name.c_str());
    else ImGui::TextDisabled("%s", T("Not loaded"));
    ImGui::Spacing();
    ImGui::TextWrapped("%s", T("A C++ plugin is a compiled library: it runs without a sandbox and must be rebuilt "
                               "for every editor version. Turn it on only if you trust it. For your own tools "
                               "write a Lua plugin instead — no compiler needed."));
}

void DrawCreate(EditorHost& host) {
    State& s = S();
    if (s.CreatePending) {
        ImGui::OpenPopup(kCreatePopup);
        s.CreatePending = false;
    }
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 30.0f, 0.0f), ImGuiCond_Appearing);
    char title[128];
    std::snprintf(title, sizeof(title), "%s%s", T("New plugin"), kCreatePopup);
    if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::TextDisabled("%s", T("Name"));
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1.0f);
    bool create = ImGui::InputText("##plugin_name", s.Name, sizeof(s.Name), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::TextDisabled("%s", T("Author"));
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##plugin_author", s.Author, sizeof(s.Author));

    ImGui::Spacing();
    ImGui::TextDisabled("%s", T("What it does"));
    ImGui::RadioButton(T("Editor tool — menu items and windows in the editor"), &s.Kind, 0);
    ImGui::RadioButton(T("Game feature — runs in Play and in the built game"), &s.Kind, 1);
    ImGui::RadioButton(T("Both"), &s.Kind, 2);

    ImGui::Spacing();
    ImGui::TextDisabled("%s", T("Where"));
    const bool hasProject = host.CurrentProject().Loaded();
    if (!hasProject) s.InProject = false;
    ImGui::BeginDisabled(!hasProject);
    if (ImGui::RadioButton(T("This project (travels with it into the game)"), s.InProject)) s.InProject = true;
    ImGui::EndDisabled();
    if (ImGui::RadioButton(T("My plugins (every project, editor only)"), !s.InProject)) s.InProject = false;
    if (!s.InProject && s.Kind != 0)
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1.0f), "%s",
                           T("The game part works only for project plugins: install it into the project."));

    const std::string id = sage::plugins::MakeId(s.Name[0] ? s.Name : "My plugin");
    ImGui::TextDisabled(T("Folder: plugins/%s"), id.c_str());
    if (!s.CreateError.empty()) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", s.CreateError.c_str());

    ImGui::Spacing();
    create = EditorIcons::Button("plus", T("Create")) || create;
    ImGui::SameLine();
    const bool cancel = ImGui::Button(T("Cancel")) || ImGui::IsKeyPressed(ImGuiKey_Escape);
    if (create) {
        const fs::path where = s.InProject ? lp::ProjectDir(host) : lp::UserDir();
        const lp::Template kind = s.Kind == 0 ? lp::Template::Editor : s.Kind == 1 ? lp::Template::Runtime
                                                                                   : lp::Template::Both;
        const fs::path dir = lp::Create(where, s.Name, s.Author, kind, s.CreateError);
        if (!dir.empty()) {
            lp::Rescan(host);
            s.Selected = sage::PathToUtf8(dir.filename());
            s.Open = true;
            // Сразу в редактор кода — ради этого плагин и создавали.
            host.OpenFileInSystemEditor(kind == lp::Template::Runtime ? dir / "runtime.lua" : dir / "editor.lua");
            host.SetStatusMessage(std::string(T("Plugin created: ")) + sage::PathToUtf8(dir));
            s.CreateError.clear();
            ImGui::CloseCurrentPopup();
        }
    } else if (cancel) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void DrawRemove(EditorHost& host) {
    State& s = S();
    if (s.RemovePending) {
        ImGui::OpenPopup(kRemovePopup);
        s.RemovePending = false;
    }
    char title[128];
    std::snprintf(title, sizeof(title), "%s%s", T("Remove plugin"), kRemovePopup);
    if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::Text(T("Delete the plugin folder «%s» from disk?"), s.Selected.c_str());
    ImGui::TextDisabled("%s", T("This cannot be undone."));
    if (EditorIcons::Button("trash", T("Remove"))) {
        std::string err;
        if (lp::Remove(host, s.Selected, err)) {
            s.Message = std::string(T("Plugin removed: ")) + s.Selected;
            s.Selected.clear();
        } else {
            s.Message = err;
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Cancel")) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

} // namespace

void Open() { S().Open = true; }
bool IsOpen() { return S().Open; }
void OpenCreate() { S().CreatePending = true; }
void Select(const std::string& id) { S().Selected = id; }

bool NativeEnabled(const fs::path& file) { return sage::editor::prefs::GetBool(NativeKey(file), false); }

void Draw(EditorHost& host, const Native& native) {
    State& s = S();
    DrawCreate(host);
    if (!s.Open) return;

    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 52.0f, ImGui::GetFontSize() * 30.0f),
                             ImGuiCond_FirstUseEver);
    char title[96];
    std::snprintf(title, sizeof(title), "%s###plugins_window", T("Plugins"));
    if (!ImGui::Begin(title, &s.Open)) {
        ImGui::End();
        return;
    }

    // --- Действия -------------------------------------------------------------
    if (EditorIcons::Button("plus", T("Create plugin..."))) s.CreatePending = true;
    Next(T("Install..."));
    if (EditorIcons::Button("import", T("Install..."))) {
        FileBrowser::Config c;
        c.Title = T("Install a plugin: folder, plugin.json or .zip");
        c.Mode = FileBrowser::PickMode::OpenAny;
        c.FilterLabel = T("All files");
        s.Browser.Open(c);
    }
    Next(T("Rescan"));
    if (EditorIcons::Button("refresh", T("Rescan"))) lp::Rescan(host);
    Next(T("My plugins folder"));
    if (EditorIcons::Button("folder", T("My plugins folder"))) {
        std::error_code ec;
        fs::create_directories(lp::UserDir(), ec);
        Sage::Launcher::OpenWithSystem(lp::UserDir());
    }
    if (host.CurrentProject().Loaded()) {
        Next(T("Project plugins folder"));
        if (EditorIcons::Button("folder", T("Project plugins folder"))) {
            std::error_code ec;
            fs::create_directories(lp::ProjectDir(host), ec);
            Sage::Launcher::OpenWithSystem(lp::ProjectDir(host));
        }
    }
    ImGui::Checkbox(T("Install into the project"), &s.InstallToProject);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("Off — into My plugins: available in every project, but the game part does not run."));
    if (s.Browser.Draw()) {
        const fs::path where = (s.InstallToProject && host.CurrentProject().Loaded()) ? lp::ProjectDir(host)
                                                                                       : lp::UserDir();
        std::string err;
        const std::string id = lp::Install(s.Browser.Result(), where, err);
        if (!id.empty()) {
            lp::Rescan(host);
            s.Selected = id;
            s.Message = std::string(T("Plugin installed: ")) + id;
        } else {
            s.Message = err;
        }
    }

    // --- Фильтр ---------------------------------------------------------------
    const struct { Filter F; const char* Label; } tabs[] = {
        {Filter::All, T("All")}, {Filter::Project, T("Project")}, {Filter::User, T("Mine")},
        {Filter::Builtin, T("Built-in")}, {Filter::Native, "C++"}};
    for (const auto& t : tabs) {
        if (t.F != Filter::All) ImGui::SameLine();
        if (ImGui::RadioButton(t.Label, s.Show == t.F)) s.Show = t.F;
    }
    ImGui::Separator();

    // --- Список ---------------------------------------------------------------
    const std::vector<lp::PluginInfo> list = lp::List();
    const std::vector<fs::path> natives = NativeFiles(native.Dir);
    ImGui::BeginChild("##plugin_list", ImVec2(ImGui::GetFontSize() * 17.0f, -ImGui::GetFrameHeightWithSpacing()),
                      ImGuiChildFlags_Borders);
    int shown = 0;
    for (const lp::PluginInfo& info : list) {
        if (!Matches(s.Show, info.From)) continue;
        ++shown;
        const std::string& id = info.Manifest.Id;
        ImGui::PushID(id.c_str());
        bool on = info.Enabled;
        if (ImGui::Checkbox("##on", &on)) {
            std::string err;
            if (!lp::SetEnabled(host, id, on, err)) s.Message = err;
            else s.Message = std::string(on ? T("Plugin on: ") : T("Plugin off: ")) + info.Manifest.Title();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", on ? T("On — click to turn off") : T("Off — click to turn on"));
        ImGui::SameLine();
        if (!info.Error.empty()) {
            EditorIcons::Inline("error", glm::vec3(1.0f, 0.45f, 0.45f));
            ImGui::SameLine(0.0f, 0.0f);
        }
        std::string row = info.Manifest.Title() + "  ";
        if (ImGui::Selectable(row.c_str(), s.Selected == id)) s.Selected = id;
        ImGui::SameLine();
        ImGui::TextDisabled("%s", SourceLabel(info.From));
        ImGui::PopID();
    }
    if (s.Show == Filter::All || s.Show == Filter::Native) {
        for (const fs::path& file : natives) {
            ++shown;
            const std::string key = "native:" + sage::PathToUtf8(file.filename());
            ImGui::PushID(key.c_str());
            bool on = NativeEnabled(file);
            if (ImGui::Checkbox("##on", &on)) {
                sage::editor::prefs::SetBool(NativeKey(file), on);
                if (native.Manager && native.Context) {
                    if (on) native.Manager->LoadOne(file, *native.Context);
                    else native.Manager->UnloadOne(file);
                }
            }
            ImGui::SameLine();
            if (ImGui::Selectable(sage::PathToUtf8(file.filename()).c_str(), s.Selected == key)) s.Selected = key;
            ImGui::SameLine();
            ImGui::TextDisabled("%s", "C++");
            ImGui::PopID();
        }
    }
    if (shown == 0) {
        ImGui::TextWrapped("%s", T("No plugins here yet. «Create plugin...» makes a working example in one click."));
    }
    ImGui::EndChild();

    // --- Подробности ----------------------------------------------------------
    ImGui::SameLine();
    ImGui::BeginChild("##plugin_details", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing()), ImGuiChildFlags_Borders);
    bool found = false;
    for (const lp::PluginInfo& info : list)
        if (info.Manifest.Id == s.Selected) {
            DrawLuaDetails(host, info);
            found = true;
            break;
        }
    if (!found)
        for (const fs::path& file : natives)
            if ("native:" + sage::PathToUtf8(file.filename()) == s.Selected) {
                DrawNativeDetails(file, native);
                found = true;
                break;
            }
    if (!found) {
        ImGui::TextWrapped("%s", T("A plugin is a folder with plugin.json and Lua files: editor.lua adds menu "
                                   "items and windows to the editor, runtime.lua adds features to the game. "
                                   "Select a plugin on the left or create a new one."));
    }
    ImGui::EndChild();
    if (!s.Message.empty()) ImGui::TextDisabled("%s", s.Message.c_str());
    else ImGui::TextDisabled("%s", T("Saving editor.lua reloads the plugin by itself."));

    DrawRemove(host);
    ImGui::End();
}

} // namespace sage::editor::pluginswindow
