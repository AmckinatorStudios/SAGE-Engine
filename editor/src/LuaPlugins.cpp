#include "LuaPlugins.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <unordered_map>

#include <sol/sol.hpp>

#include "imgui.h"

#include "EditorHost.h"
#include "EditorPrefs.h"
#include "EditorSelection.h"
#include "Localization.h"
#include "ObjectCatalog.h"
#include "Project.h"
#include "sage/assets/Zip.h"
#include "sage/core/Log.h"
#include "sage/core/Paths.h"
#include "sage/scene/Components.h"
#include "sage/scene/Prefab.h"
#include "sage/scene/Scene.h"

namespace fs = std::filesystem;
namespace pl = sage::plugins;

namespace sage::editor::luaplugins {

// ---------------------------------------------------------------------------
//  Состояние загруженного плагина
// ---------------------------------------------------------------------------
struct Entry {
    PluginInfo Info;
    std::unique_ptr<sol::state> Lua;
    long long Stamp = 0;                 // время правки editor.lua при загрузке
    struct Menu { std::string Label; sol::protected_function Fn; };
    struct Panel { std::string Title; sol::protected_function Fn; bool Open = true; };
    std::vector<Menu> Menus;
    std::vector<Panel> Panels;
    std::unordered_map<std::string, std::vector<sol::protected_function>> Handlers;
    bool Muted = false;                  // упал обработчик — молчит до перезагрузки
};

namespace {

struct State {
    std::vector<std::unique_ptr<Entry>> Entries;
    EditorHost* Host = nullptr;          // хост текущего вызова (API сцены)
    bool Snapshot = false;               // шаг отмены уже записан в этом вызове
    int LastPrimary = -2;
    bool LastPlay = false;
    std::string LastProject;
    float RecheckIn = 0.0f;
};

State& S() {
    static State s;
    return s;
}

long long Stamp(const fs::path& p) {
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    return ec ? 0 : (long long)t.time_since_epoch().count();
}

std::string ReadAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// --- Сторож зависания ----------------------------------------------------------
// Бесконечный цикл в плагине повесил бы весь редактор: Lua не отдаёт управление
// сам. Раз в 10 000 инструкций проверяем часы; вызов дольше 2 секунд
// прерывается ошибкой, и плагин замолкает, как при любой другой ошибке.
std::chrono::steady_clock::time_point g_deadline;
bool g_guard = false;

void Watchdog(lua_State* L, lua_Debug*) {
    if (g_guard && std::chrono::steady_clock::now() > g_deadline)
        luaL_error(L, "плагин работает дольше 2 секунд без перерыва (бесконечный цикл?) — остановлен");
}

struct Guarded {
    Guarded() {
        g_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        g_guard = true;
        S().Snapshot = false;
    }
    ~Guarded() { g_guard = false; }
};

void Fail(Entry& e, const std::string& what) {
    e.Info.Error = what;
    e.Muted = true;
    LOG_ERROR("Plugin") << "[" << e.Info.Manifest.Id << "] " << what;
}

// Вызов обработчика плагина: ошибка -> в окно «Плагины» и в консоль, плагин
// замолкает. Возвращает результат вызова (или пусто).
template <typename... Args>
sol::object Call(Entry& e, sol::protected_function& fn, Args&&... args) {
    if (e.Muted || !fn.valid()) return sol::lua_nil;
    Guarded guard;
    EditorHost* host = S().Host;
    const size_t depth = host ? host->History().UndoDepth() : 0;
    sol::protected_function_result r = fn(std::forward<Args>(args)...);
    // Всё, что вызов положил в историю (каждое editor.create — свой снимок), —
    // одним шагом.
    if (host) host->History().CollapseSince(depth);
    if (!r.valid()) {
        sol::error err = r;
        Fail(e, err.what());
        return sol::lua_nil;
    }
    return r.get<sol::object>();
}

// Перед первой правкой сцены за вызов — шаг отмены: всё, что плагин сделал по
// одному нажатию, отменяется одним Ctrl+Z.
void Touch() {
    State& s = S();
    if (s.Host && !s.Snapshot) {
        s.Host->PushUndoSnapshot();
        s.Snapshot = true;
    }
}

Scene* CurrentScene() { return S().Host ? &S().Host->CurrentScene() : nullptr; }

// --- Объект сцены для Lua ------------------------------------------------------
// Держит НОМЕР объекта, а не указатель: плагин может хранить объект между
// кадрами, а сцена тем временем перезагружается и пересоздаёт сущности.
struct Obj {
    int Id = 0;
    GameObject Get() const {
        Scene* scene = CurrentScene();
        return scene ? scene->Get(Id) : GameObject();
    }
    bool Alive() const { return Get().Valid(); }
};

sol::table Vec(sol::state_view lua, const glm::vec3& v) {
    sol::table t = lua.create_table();
    t["x"] = v.x;
    t["y"] = v.y;
    t["z"] = v.z;
    return t;
}

glm::vec3 VecFrom(const sol::object& o, const glm::vec3& fallback) {
    if (o.get_type() != sol::type::table) return fallback;
    sol::table t = o.as<sol::table>();
    return {t.get_or("x", t.get_or(1, fallback.x)), t.get_or("y", t.get_or(2, fallback.y)),
            t.get_or("z", t.get_or(3, fallback.z))};
}

sol::object Wrap(sol::state_view lua, const GameObject& o) {
    if (!o.Valid() || !o.Registry()->all_of<IdComponent>(o.Entity())) return sol::lua_nil;
    return sol::make_object(lua, Obj{o.Id()});
}

GameObject Need(const Obj& o, const char* what) {
    GameObject g = o.Get();
    if (!g.Valid()) throw std::runtime_error(std::string(what) + ": объекта больше нет в сцене");
    return g;
}

// --- API editor.* и ui.* -------------------------------------------------------
void BindApi(sol::state& lua, Entry* entry) {
    lua.open_libraries(sol::lib::base, sol::lib::string, sol::lib::table, sol::lib::math,
                       sol::lib::os, sol::lib::io, sol::lib::utf8, sol::lib::package);
    lua_sethook(lua.lua_state(), &Watchdog, LUA_MASKCOUNT, 10000);

    lua.new_usertype<Obj>(
        "EditorObject", sol::no_constructor,
        "id", sol::readonly_property([](const Obj& o) { return o.Id; }),
        "valid", sol::readonly_property([](const Obj& o) { return o.Alive(); }),
        "name", sol::property([](const Obj& o) { return Need(o, "name").Name(); },
                              [](Obj& o, const std::string& v) { Touch(); Need(o, "name").SetName(v); }),
        "position", sol::property(
            [](const Obj& o, sol::this_state L) { return Vec(L, Need(o, "position").GetTransform().Position); },
            [](Obj& o, sol::object v) {
                Touch();
                Transform& t = Need(o, "position").GetTransform();
                t.Position = VecFrom(v, t.Position);
            }),
        "rotation", sol::property(
            [](const Obj& o, sol::this_state L) { return Vec(L, Need(o, "rotation").GetTransform().Rotation); },
            [](Obj& o, sol::object v) {
                Touch();
                Transform& t = Need(o, "rotation").GetTransform();
                t.Rotation = VecFrom(v, t.Rotation);
            }),
        "scale", sol::property(
            [](const Obj& o, sol::this_state L) { return Vec(L, Need(o, "scale").GetTransform().Scale); },
            [](Obj& o, sol::object v) {
                Touch();
                Transform& t = Need(o, "scale").GetTransform();
                t.Scale = VecFrom(v, t.Scale);
            }),
        "color", sol::property(
            [](const Obj& o, sol::this_state L) -> sol::object {
                GameObject g = Need(o, "color");
                if (!g.HasRenderer()) return sol::lua_nil;
                return Vec(L, g.Renderer().Color);
            },
            [](Obj& o, sol::object v) {
                GameObject g = Need(o, "color");
                if (!g.HasRenderer()) return;
                Touch();
                const glm::vec3 c = g.Renderer().Color;
                sol::table t = v.as<sol::table>();
                g.Renderer().Color = {t.get_or("r", t.get_or("x", t.get_or(1, c.r))),
                                      t.get_or("g", t.get_or("y", t.get_or(2, c.g))),
                                      t.get_or("b", t.get_or("z", t.get_or(3, c.b)))};
            }),
        "parent", sol::property(
            [](const Obj& o, sol::this_state L) -> sol::object {
                Scene* scene = CurrentScene();
                GameObject g = Need(o, "parent");
                const entt::entity p = scene->ParentOf(g.Entity());
                return p == entt::null ? sol::object(sol::lua_nil) : Wrap(L, GameObject(&scene->Registry(), p));
            },
            [](Obj& o, sol::object v) {
                Scene* scene = CurrentScene();
                GameObject g = Need(o, "parent");
                Touch();
                if (v.is<Obj>()) scene->SetParent(g.Entity(), Need(v.as<Obj>(), "parent").Entity());
                else scene->SetParent(g.Entity(), entt::null);
            }),
        "children", [](const Obj& o, sol::this_state L) {
            sol::state_view lua(L);
            sol::table out = lua.create_table();
            Scene* scene = CurrentScene();
            GameObject g = Need(o, "children");
            int i = 1;
            for (entt::entity c : scene->ChildrenOf(g.Entity()))
                out[i++] = Wrap(lua, GameObject(&scene->Registry(), c));
            return out;
        },
        "delete", [](Obj& o) {
            GameObject g = Need(o, "delete");
            Touch();
            CurrentScene()->RemoveObject(g.Id());
        },
        sol::meta_function::to_string, [](const Obj& o) {
            GameObject g = o.Get();
            return g.Valid() ? "EditorObject(" + g.Name() + ")" : std::string("EditorObject(удалён)");
        },
        sol::meta_function::equal_to, [](const Obj& a, const Obj& b) { return a.Id == b.Id; });

    const std::string id = entry ? entry->Info.Manifest.Id : std::string("dry");
    sol::table ed = lua.create_named_table("editor");
    ed["plugin_id"] = id;
    ed["plugin_dir"] = entry ? sage::PathToUtf8(entry->Info.Manifest.Dir) : std::string();

    ed.set_function("menu", [entry](const std::string& label, sol::protected_function fn) {
        if (entry) entry->Menus.push_back({label, fn});
    });
    // Окно закрыто, пока его не открыли из меню «Плагины», и помнит это между
    // запусками: десяток плагинов, каждый со своим окном поверх сцены при
    // старте, — это не помощь, а уборка.
    ed.set_function("panel", [entry](const std::string& title, sol::protected_function fn) {
        if (!entry) return;
        const std::string key = "plugins.panel." + entry->Info.Manifest.Id + "." + std::to_string(entry->Panels.size());
        entry->Panels.push_back({title, fn, sage::editor::prefs::GetBool(key, false)});
    });
    ed.set_function("on", [entry](const std::string& event, sol::protected_function fn) {
        static const char* kEvents[] = {"update", "selection", "play", "stop", "project"};
        if (std::find_if(std::begin(kEvents), std::end(kEvents),
                         [&](const char* e) { return event == e; }) == std::end(kEvents))
            throw std::runtime_error("editor.on: нет события «" + event +
                                     "» (есть update, selection, play, stop, project)");
        if (entry) entry->Handlers[event].push_back(fn);
    });

    auto prefix = [id](const std::string& msg) { return "[" + id + "] " + msg; };
    ed.set_function("log", [prefix](const std::string& m) { LOG_INFO("Plugin") << prefix(m); });
    ed.set_function("warn", [prefix](const std::string& m) { LOG_WARN("Plugin") << prefix(m); });
    ed.set_function("error", [prefix](const std::string& m) { LOG_ERROR("Plugin") << prefix(m); });
    ed.set_function("status", [](const std::string& m) {
        if (S().Host) S().Host->SetStatusMessage(m);
    });

    ed.set_function("selected", [](sol::this_state L) -> sol::object {
        if (!S().Host) return sol::lua_nil;
        return Wrap(L, S().Host->SelectedObject());
    });
    ed.set_function("selection", [](sol::this_state L) {
        sol::state_view lua(L);
        sol::table out = lua.create_table();
        if (!S().Host) return out;
        int i = 1;
        for (int sid : S().Host->Selection().All()) {
            GameObject g = S().Host->CurrentScene().Get(sid);
            if (g.Valid()) out[i++] = Wrap(lua, g);
        }
        return out;
    });
    ed.set_function("select", [](sol::object what) {
        if (!S().Host) return;
        std::vector<int> ids;
        if (what.is<Obj>()) ids.push_back(what.as<Obj>().Id);
        else if (what.get_type() == sol::type::table)
            for (auto& kv : what.as<sol::table>())
                if (kv.second.is<Obj>()) ids.push_back(kv.second.as<Obj>().Id);
        S().Host->Selection().Set(ids);
    });
    ed.set_function("objects", [](sol::this_state L) {
        sol::state_view lua(L);
        sol::table out = lua.create_table();
        Scene* scene = CurrentScene();
        if (!scene) return out;
        int i = 1;
        // В порядке списка сцены, обходом в глубину: «все объекты» плагин видит
        // так же, как человек в иерархии.
        std::vector<entt::entity> stack = scene->Roots();
        std::reverse(stack.begin(), stack.end());
        while (!stack.empty()) {
            const entt::entity e = stack.back();
            stack.pop_back();
            out[i++] = Wrap(lua, GameObject(&scene->Registry(), e));
            std::vector<entt::entity> kids = scene->ChildrenOf(e);
            for (auto it = kids.rbegin(); it != kids.rend(); ++it) stack.push_back(*it);
        }
        return out;
    });
    ed.set_function("find", [](const std::string& name, sol::this_state L) -> sol::object {
        Scene* scene = CurrentScene();
        if (!scene) return sol::lua_nil;
        return Wrap(L, scene->FindByName(name));
    });
    // Объект — по ключу каталога «Создать»: "empty", "shape.cube", "light.point"…
    ed.set_function("create", [](sol::optional<std::string> kind, sol::optional<std::string> name,
                                 sol::this_state L) -> sol::object {
        if (!S().Host) return sol::lua_nil;
        // Каталог сам делает снимок для отмены; лишние склеит Call.
        S().Snapshot = true;
        const int newId = S().Host->CreateCatalogObject(kind.value_or("empty"));
        if (newId < 0)
            throw std::runtime_error("editor.create: нет такого вида «" + kind.value_or("") +
                                     "» (см. editor.kinds())");
        GameObject g = S().Host->CurrentScene().Get(newId);
        if (name && g.Valid()) g.SetName(*name);
        return Wrap(L, g);
    });
    // Копия объекта вместе с потомками, у того же родителя.
    ed.set_function("duplicate", [](const Obj& o, sol::this_state L) -> sol::object {
        Scene* scene = CurrentScene();
        GameObject src = Need(o, "duplicate");
        Touch();
        const entt::entity parent = scene->ParentOf(src.Entity());
        GameObject copy = sage::scene::CopySubtree(*scene, src.Entity(), *scene, parent);
        return Wrap(L, copy);
    });
    ed.set_function("kinds", [](sol::this_state L) {
        sol::state_view lua(L);
        sol::table out = lua.create_table();
        int i = 1;
        std::function<void(const objectcatalog::Group&)> walk = [&](const objectcatalog::Group& g) {
            for (const objectcatalog::Item& it : g.Items) out[i++] = std::string(it.Id);
            for (const objectcatalog::Group& sub : g.Groups) walk(sub);
        };
        walk(objectcatalog::Catalog());
        return out;
    });
    ed.set_function("undo_point", []() {
        S().Snapshot = false;
        Touch();
    });
    ed.set_function("playing", []() { return S().Host && S().Host->InPlayMode(); });
    ed.set_function("project_dir", []() -> std::string {
        if (!S().Host || !S().Host->CurrentProject().Loaded()) return {};
        return sage::PathToUtf8(S().Host->CurrentProject().Dir());
    });
    ed.set_function("assets_dir", []() -> std::string {
        if (!S().Host || !S().Host->CurrentProject().Loaded()) return {};
        return sage::PathToUtf8(S().Host->CurrentProject().AssetsDir());
    });
    ed.set_function("open", [](const std::string& path) {
        return S().Host && S().Host->OpenFileInSystemEditor(sage::PathFromUtf8(path));
    });
    ed.set_function("save_scene", []() {
        if (S().Host) S().Host->SaveCurrentScene();
    });

    // Настройки плагина — из plugin.json; editor.save_settings() пишет обратно.
    sol::table settings = lua.create_table();
    if (entry)
        for (const sage::vars::Var& v : entry->Info.Manifest.Settings.All()) {
            switch (v.Data.Type()) {
                case sage::vars::Kind::Bool: settings[v.Name] = v.Data.AsBool(); break;
                case sage::vars::Kind::Int: settings[v.Name] = v.Data.AsInt(); break;
                case sage::vars::Kind::Float: settings[v.Name] = v.Data.AsFloat(); break;
                default: settings[v.Name] = v.Data.AsString(); break;
            }
        }
    ed["settings"] = settings;
    ed.set_function("save_settings", [entry](sol::this_state L) {
        if (!entry || !S().Host) return false;
        sol::state_view lua(L);
        sol::table t = lua["editor"]["settings"];
        sage::vars::Table out;
        for (auto& kv : t) {
            if (kv.first.get_type() != sol::type::string) continue;
            const std::string key = kv.first.as<std::string>();
            const sol::object v = kv.second;
            if (v.get_type() == sol::type::boolean) out.Set(key, sage::vars::Value(v.as<bool>()));
            else if (v.get_type() == sol::type::number) {
                const double d = v.as<double>();
                if (d == (double)(int)d) out.Set(key, sage::vars::Value((int)d));
                else out.Set(key, sage::vars::Value((float)d));
            } else if (v.get_type() == sol::type::string) out.Set(key, sage::vars::Value(v.as<std::string>()));
        }
        std::string err;
        return SaveSettings(*S().Host, entry->Info.Manifest.Id, out, err);
    });

    // --- ui.* — виджеты для editor.panel -----------------------------------------
    // Возвращают новое значение (а не «изменилось ли»): `speed = ui.slider("Скорость",
    // speed, 0, 10)` — одна строка вместо пары «указатель + флаг».
    sol::table ui = lua.create_named_table("ui");
    // Любые значения через пробел, как print: ui.text("Объектов:", n).
    ui.set_function("text", [](sol::variadic_args va, sol::this_state L) {
        sol::protected_function tostr = sol::state_view(L)["tostring"];
        std::string line;
        for (auto v : va) {
            if (!line.empty()) line += ' ';
            sol::protected_function_result r = tostr(v.get<sol::object>());
            if (r.valid()) line += r.get<std::string>();
        }
        ImGui::TextWrapped("%s", line.c_str());
    });
    ui.set_function("heading", [](const std::string& s) {
        ImGui::Spacing();
        ImGui::TextUnformatted(s.c_str());
        ImGui::Separator();
    });
    ui.set_function("hint", [](const std::string& s) { ImGui::TextDisabled("%s", s.c_str()); });
    ui.set_function("button", [](const std::string& label) { return ImGui::Button(label.c_str()); });
    ui.set_function("checkbox", [](const std::string& label, bool v) {
        ImGui::Checkbox(label.c_str(), &v);
        return v;
    });
    ui.set_function("slider", [](const std::string& label, float v, sol::optional<float> lo, sol::optional<float> hi) {
        ImGui::SliderFloat(label.c_str(), &v, lo.value_or(0.0f), hi.value_or(1.0f));
        return v;
    });
    ui.set_function("int", [](const std::string& label, int v, sol::optional<int> lo, sol::optional<int> hi) {
        if (lo && hi) ImGui::SliderInt(label.c_str(), &v, *lo, *hi);
        else ImGui::InputInt(label.c_str(), &v);
        return v;
    });
    ui.set_function("number", [](const std::string& label, float v) {
        ImGui::DragFloat(label.c_str(), &v, 0.05f);
        return v;
    });
    ui.set_function("input", [](const std::string& label, sol::optional<std::string> text) {
        char buf[512];
        std::snprintf(buf, sizeof(buf), "%s", text.value_or("").c_str());
        ImGui::InputText(label.c_str(), buf, sizeof(buf));
        return std::string(buf);
    });
    ui.set_function("color", [](const std::string& label, sol::table c, sol::this_state L) {
        float rgb[3] = {c.get_or("r", c.get_or(1, 1.0f)), c.get_or("g", c.get_or(2, 1.0f)),
                        c.get_or("b", c.get_or(3, 1.0f))};
        ImGui::ColorEdit3(label.c_str(), rgb);
        sol::table out = sol::state_view(L).create_table();
        out["r"] = rgb[0];
        out["g"] = rgb[1];
        out["b"] = rgb[2];
        return out;
    });
    ui.set_function("combo", [](const std::string& label, int current, sol::table items) {
        std::vector<std::string> names;
        for (size_t i = 1; i <= items.size(); ++i) names.push_back(items.get_or(i, std::string()));
        const char* preview = (current >= 1 && current <= (int)names.size()) ? names[current - 1].c_str() : "";
        if (ImGui::BeginCombo(label.c_str(), preview)) {
            for (int i = 0; i < (int)names.size(); ++i)
                if (ImGui::Selectable(names[i].c_str(), current == i + 1)) current = i + 1;
            ImGui::EndCombo();
        }
        return current;
    });
    ui.set_function("separator", []() { ImGui::Separator(); });
    ui.set_function("same_line", []() { ImGui::SameLine(); });
    ui.set_function("space", []() { ImGui::Spacing(); });
    ui.set_function("tooltip", [](const std::string& s) {
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", s.c_str());
    });
}

// --- Где «включён ли» ----------------------------------------------------------
std::string PrefKey(const std::string& id) { return "plugins.off." + id; }

bool IsEnabled(const pl::Manifest& m, Source from) {
    if (from == Source::Project) return m.Enabled;
    return m.Enabled && !sage::editor::prefs::GetBool(PrefKey(m.Id), false);
}

Entry* FindEntry(const std::string& id) {
    for (auto& e : S().Entries)
        if (e->Info.Manifest.Id == id) return e.get();
    return nullptr;
}

void Unload(Entry& e) {
    e.Menus.clear();
    e.Panels.clear();
    e.Handlers.clear();
    e.Lua.reset();
    e.Info.Running = false;
    e.Info.Menus = e.Info.Panels = 0;
}

void Load(EditorHost* host, Entry& e) {
    Unload(e);
    e.Muted = false;
    e.Info.Error = e.Info.Manifest.Error;
    if (!e.Info.Error.empty() || !e.Info.Enabled || !e.Info.Manifest.HasEditor()) return;
    const fs::path file = e.Info.Manifest.EditorPath();
    std::error_code ec;
    if (!fs::exists(file, ec)) {
        e.Info.Error = "нет файла " + e.Info.Manifest.Editor;
        return;
    }
    e.Stamp = Stamp(file);
    e.Lua = std::make_unique<sol::state>();
    BindApi(*e.Lua, &e);
    // require ищет модули рядом с плагином: editor.lua вправе разложить код по файлам.
    const std::string dir = sage::PathToUtf8(e.Info.Manifest.Dir);
    (*e.Lua)["package"]["path"] = dir + "/?.lua;" + dir + "/?/init.lua";
    State& s = S();
    EditorHost* prev = s.Host;
    s.Host = host;
    {
        Guarded guard;
        sol::load_result chunk = e.Lua->load(ReadAll(file), "@" + sage::PathToUtf8(file));
        if (!chunk.valid()) {
            sol::error err = chunk;
            s.Host = prev;
            Fail(e, err.what());
            return;
        }
        sol::protected_function fn = chunk;
        sol::protected_function_result r = fn();
        if (!r.valid()) {
            sol::error err = r;
            s.Host = prev;
            Fail(e, err.what());
            return;
        }
    }
    s.Host = prev;
    e.Info.Running = true;
    e.Info.Menus = (int)e.Menus.size();
    e.Info.Panels = (int)e.Panels.size();
    LOG_INFO("Plugin") << "Плагин «" << e.Info.Manifest.Title() << "» загружен";
}

void Emit(EditorHost& host, const std::string& event, sol::object arg = sol::lua_nil) {
    State& s = S();
    s.Host = &host;
    for (auto& e : s.Entries) {
        if (!e->Info.Running || e->Muted) continue;
        auto it = e->Handlers.find(event);
        if (it == e->Handlers.end()) continue;
        for (sol::protected_function& fn : it->second) {
            // Объект — в состояние ЭТОГО плагина: у каждого своё.
            if (event == "selection") Call(*e, fn, Wrap(*e->Lua, host.SelectedObject()));
            else if (arg.valid() && arg.get_type() == sol::type::number) Call(*e, fn, arg.as<double>());
            else Call(*e, fn);
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
fs::path BuiltinDir() { return sage::ExecutableDir() / "plugins"; }
fs::path UserDir() { return sage::ConfigDir() / "plugins"; }
fs::path ProjectDir(EditorHost& host) {
    return host.CurrentProject().Loaded() ? host.CurrentProject().Dir() / "plugins" : fs::path();
}

void Rescan(EditorHost& host) {
    State& s = S();
    s.Entries.clear();
    auto add = [&](const fs::path& dir, Source from) {
        if (dir.empty()) return;
        for (pl::Manifest& m : pl::Scan(dir)) {
            // Один id — один плагин: проектный сильнее «моего», «мой» — встроенного
            // (порядок обхода ниже), как локальная настройка сильнее общей.
            if (Entry* old = FindEntry(m.Id)) {
                old->Info.Manifest = m;
                old->Info.From = from;
                old->Info.Enabled = IsEnabled(m, from);
                continue;
            }
            auto e = std::make_unique<Entry>();
            e->Info.Manifest = std::move(m);
            e->Info.From = from;
            e->Info.Enabled = IsEnabled(e->Info.Manifest, from);
            s.Entries.push_back(std::move(e));
        }
    };
    add(BuiltinDir(), Source::Builtin);
    add(UserDir(), Source::User);
    add(ProjectDir(host), Source::Project);
    for (auto& e : s.Entries) Load(&host, *e);
    s.LastProject = host.CurrentProject().Loaded() ? sage::PathToUtf8(host.CurrentProject().Dir()) : "";
}

void Tick(EditorHost& host, float dt) {
    State& s = S();
    const std::string project =
        host.CurrentProject().Loaded() ? sage::PathToUtf8(host.CurrentProject().Dir()) : "";
    if (project != s.LastProject) {
        Rescan(host);
        Emit(host, "project");
    }
    // Правка editor.lua — перезапуск на лету. Раз в полсекунды, а не каждый
    // кадр: спрашивать файловую систему 60 раз в секунду незачем.
    s.RecheckIn -= dt;
    if (s.RecheckIn <= 0.0f) {
        s.RecheckIn = 0.5f;
        for (auto& e : s.Entries) {
            if (!e->Info.Enabled || !e->Info.Manifest.HasEditor()) continue;
            if (Stamp(e->Info.Manifest.EditorPath()) != e->Stamp) {
                pl::Manifest fresh;
                if (pl::ReadManifest(e->Info.Manifest.Dir, fresh)) e->Info.Manifest = fresh;
                Load(&host, *e);
                if (e->Info.Running) host.SetStatusMessage(std::string(T("Plugin reloaded: ")) + e->Info.Manifest.Title());
            }
        }
    }
    const int primary = host.Selection().Primary();
    if (primary != s.LastPrimary) {
        s.LastPrimary = primary;
        Emit(host, "selection");
    }
    const bool play = host.InPlayMode();
    if (play != s.LastPlay) {
        s.LastPlay = play;
        Emit(host, play ? "play" : "stop");
    }
    for (auto& e : s.Entries) {
        if (!e->Info.Running || e->Muted) continue;
        auto it = e->Handlers.find("update");
        if (it == e->Handlers.end()) continue;
        s.Host = &host;
        for (sol::protected_function& fn : it->second) Call(*e, fn, dt);
    }
}

void DrawMenuItems(EditorHost& host) {
    State& s = S();
    s.Host = &host;
    for (auto& e : s.Entries) {
        if (!e->Info.Running || (e->Menus.empty() && e->Panels.empty())) continue;
        ImGui::PushID(e->Info.Manifest.Id.c_str());
        // Один пункт — прямо в меню; несколько — подменю с именем плагина.
        const bool single = e->Menus.size() + e->Panels.size() == 1;
        const bool open = single || ImGui::BeginMenu(e->Info.Manifest.Title().c_str());
        if (open) {
            for (Entry::Menu& m : e->Menus)
                if (ImGui::MenuItem(m.Label.c_str(), nullptr, false, !e->Muted)) Call(*e, m.Fn);
            for (size_t i = 0; i < e->Panels.size(); ++i) {
                Entry::Panel& p = e->Panels[i];
                if (ImGui::MenuItem(p.Title.c_str(), nullptr, &p.Open))
                    sage::editor::prefs::SetBool("plugins.panel." + e->Info.Manifest.Id + "." + std::to_string(i), p.Open);
            }
            if (!single) ImGui::EndMenu();
        }
        ImGui::PopID();
    }
}

void DrawPanels(EditorHost& host) {
    State& s = S();
    s.Host = &host;
    for (auto& e : s.Entries) {
        if (!e->Info.Running) continue;
        for (size_t i = 0; i < e->Panels.size(); ++i) {
            Entry::Panel& p = e->Panels[i];
            if (!p.Open) continue;
            // ### — id окна не зависит от подписи: плагин вправе её менять.
            const std::string title = p.Title + "###plugin." + e->Info.Manifest.Id + "." + std::to_string(i);
            // Первое появление — у правого края вьюпорта, лесенкой: в углу (0,0)
            // окна плагинов ложились друг на друга и под окно «Плагины».
            const ImGuiViewport* vp = ImGui::GetMainViewport();
            const float step = ImGui::GetFontSize() * 2.0f * (float)(i + 1);
            ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.62f + step, vp->WorkPos.y + step * 2.0f),
                                    ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 20.0f, ImGui::GetFontSize() * 15.0f),
                                     ImGuiCond_FirstUseEver);
            if (ImGui::Begin(title.c_str(), &p.Open)) {
                ImGui::PushID(title.c_str());
                if (e->Muted) {
                    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", T("The plugin stopped with an error:"));
                    ImGui::TextWrapped("%s", e->Info.Error.c_str());
                    if (ImGui::Button(T("Restart plugin"))) Load(&host, *e);
                } else {
                    // Неснятые со стека виджеты (забытый same_line) не должны
                    // разламывать соседние окна: стек ImGui выравнивается здесь.
                    Call(*e, p.Fn);
                }
                ImGui::PopID();
            }
            ImGui::End();
            if (!e->Info.Running) break;   // «Перезапустить» пересобрал список окон
            if (!p.Open)   // закрыли крестиком — запомнить
                sage::editor::prefs::SetBool("plugins.panel." + e->Info.Manifest.Id + "." + std::to_string(i), false);
        }
    }
}

void Shutdown() { S().Entries.clear(); }

bool RunMenu(EditorHost& host, const std::string& id, int index) {
    Entry* e = FindEntry(id);
    if (!e || !e->Info.Running || index < 0 || index >= (int)e->Menus.size()) return false;
    S().Host = &host;
    Call(*e, e->Menus[(size_t)index].Fn);
    return !e->Muted;
}

std::vector<PluginInfo> List() {
    std::vector<PluginInfo> out;
    for (auto& e : S().Entries) out.push_back(e->Info);
    return out;
}

bool SetEnabled(EditorHost& host, const std::string& id, bool enabled, std::string& error) {
    Entry* e = FindEntry(id);
    if (!e) {
        error = "нет плагина " + id;
        return false;
    }
    if (e->Info.From == Source::Project) {
        e->Info.Manifest.Enabled = enabled;
        if (!pl::WriteManifest(e->Info.Manifest, error)) return false;
    } else {
        if (enabled && !e->Info.Manifest.Enabled) {
            e->Info.Manifest.Enabled = true;   // выключен в самом файле — включаем там
            pl::WriteManifest(e->Info.Manifest, error);
        }
        sage::editor::prefs::SetBool(PrefKey(id), !enabled);
    }
    e->Info.Enabled = enabled;
    if (enabled) Load(&host, *e);
    else Unload(*e);
    return true;
}

bool Reload(EditorHost& host, const std::string& id) {
    Entry* e = FindEntry(id);
    if (!e) return false;
    pl::Manifest fresh;
    if (pl::ReadManifest(e->Info.Manifest.Dir, fresh)) e->Info.Manifest = fresh;
    e->Info.Enabled = IsEnabled(e->Info.Manifest, e->Info.From);
    Load(&host, *e);
    return e->Info.Running || !e->Info.Manifest.HasEditor();
}

bool SaveSettings(EditorHost& host, const std::string& id, const sage::vars::Table& settings,
                  std::string& error) {
    (void)host;
    Entry* e = FindEntry(id);
    if (!e) {
        error = "нет плагина " + id;
        return false;
    }
    e->Info.Manifest.Settings = settings;
    return pl::WriteManifest(e->Info.Manifest, error);
}

std::string TemplateEditorLua(const std::string& name) {
    return "-- " + name + " — часть РЕДАКТОРА (выполняется в редакторе SAGE).\n"
           "-- Сохраните файл — плагин перезапустится сам. Справка: docs/plugins.md\n"
           "\n"
           "-- Пункт в меню «Плагины»: всё, что он сделает со сценой, отменяется одним Ctrl+Z.\n"
           "editor.menu(\"Поставить куб над выделенным\", function()\n"
           "    local target = editor.selected()\n"
           "    local cube = editor.create(\"shape.cube\", \"Куб от плагина\")\n"
           "    if target and cube then\n"
           "        local p = target.position\n"
           "        cube.position = { x = p.x, y = p.y + 2, z = p.z }\n"
           "    end\n"
           "    editor.status(\"Куб создан\")\n"
           "end)\n"
           "\n"
           "-- Своё окно: открывается из меню «Плагины».\n"
           "local count = 5\n"
           "local spacing = 2.0\n"
           "editor.panel(\"" + name + "\", function()\n"
           "    ui.heading(\"Ряд объектов\")\n"
           "    count = ui.int(\"Сколько\", count, 1, 50)\n"
           "    spacing = ui.slider(\"Шаг\", spacing, 0.5, 10)\n"
           "    if ui.button(\"Создать ряд\") then\n"
           "        for i = 1, count do\n"
           "            local o = editor.create(\"shape.cube\", \"Ряд \" .. i)\n"
           "            o.position = { x = (i - 1) * spacing, y = 0.5, z = 0 }\n"
           "        end\n"
           "    end\n"
           "    local sel = editor.selected()\n"
           "    ui.hint(sel and (\"Выделен: \" .. sel.name) or \"Ничего не выделено\")\n"
           "end)\n"
           "\n"
           "-- События: update(dt), selection(obj), play, stop, project.\n"
           "editor.on(\"play\", function() editor.log(\"Игра запущена\") end)\n";
}

std::string TemplateRuntimeLua(const std::string& id, const std::string& name) {
    return "-- " + name + " — часть ДВИЖКА: работает в Play и в собранной игре.\n"
           "-- Это обычный скрипт (тот же API, что у скриптов объектов), только объект\n"
           "-- у него служебный — его создаёт движок в каждой сцене.\n"
           "\n"
           "-- Переменные верхнего уровня — настройки плагина (окно «Плагины»).\n"
           "message = \"Привет из плагина!\"\n"
           "\n"
           "-- То, что получат скрипты игры: plugins.Get(\"" + id + "\").hello()\n"
           "local api = {}\n"
           "function api.hello()\n"
           "    Debug.Log(message)\n"
           "end\n"
           "plugins.Register(\"" + id + "\", api)\n"
           "\n"
           "function Start()\n"
           "    Debug.Log(\"" + name + ": сцена запущена\")\n"
           "end\n"
           "\n"
           "function Update(dt)\n"
           "end\n";
}

fs::path Create(const fs::path& where, const std::string& name, const std::string& author, Template kind,
                std::string& error) {
    if (where.empty()) {
        error = T("Open a project first: project plugins live in its plugins folder");
        return {};
    }
    const std::string title = name.empty() ? std::string("My plugin") : name;
    std::string id = pl::MakeId(title);
    fs::path dir = where / id;
    std::error_code ec;
    for (int n = 2; fs::exists(dir, ec); ++n) dir = where / (id + "_" + std::to_string(n));
    id = sage::PathToUtf8(dir.filename());
    fs::create_directories(dir, ec);
    if (ec) {
        error = T("Could not create the folder: ") + sage::PathToUtf8(dir);
        return {};
    }
    pl::Manifest m;
    m.Id = id;
    m.Name = title;
    m.Author = author;
    m.Description = T("Describe what the plugin does");
    m.Dir = dir;
    if (kind != Template::Runtime) m.Editor = "editor.lua";
    if (kind != Template::Editor) m.Runtime = "runtime.lua";
    if (!pl::WriteManifest(m, error)) return {};
    if (m.HasEditor()) std::ofstream(dir / "editor.lua", std::ios::binary) << TemplateEditorLua(title);
    if (m.HasRuntime()) std::ofstream(dir / "runtime.lua", std::ios::binary) << TemplateRuntimeLua(id, title);
    return dir;
}

std::string Install(const fs::path& from, const fs::path& where, std::string& error) {
    std::error_code ec;
    if (where.empty()) {
        error = T("Open a project first: project plugins live in its plugins folder");
        return {};
    }
    fs::path source = from;
    fs::path temp;
    if (fs::is_regular_file(from, ec) && sage::assets::IsZip(from)) {
        temp = fs::temp_directory_path(ec) / ("sage_plugin_" + std::to_string(Stamp(from)));
        fs::remove_all(temp, ec);
        if (sage::assets::ExtractZip(from, temp, error) < 0) return {};
        source = temp;
    } else if (fs::is_regular_file(from, ec) && from.filename() == pl::kManifestFile) {
        source = from.parent_path();
    }
    // plugin.json — в корне или в единственной папке внутри (так пакуют архивы).
    if (!fs::exists(source / pl::kManifestFile, ec)) {
        fs::path inner;
        int dirs = 0;
        for (const auto& d : fs::directory_iterator(source, ec))
            if (d.is_directory(ec) && fs::exists(d.path() / pl::kManifestFile, ec)) {
                inner = d.path();
                ++dirs;
            }
        if (dirs != 1) {
            error = T("No plugin.json found: a plugin is a folder with plugin.json inside");
            if (!temp.empty()) fs::remove_all(temp, ec);
            return {};
        }
        source = inner;
    }
    pl::Manifest m;
    pl::ReadManifest(source, m);
    if (!m.Error.empty()) {
        error = m.Error;
        if (!temp.empty()) fs::remove_all(temp, ec);
        return {};
    }
    const std::string id = pl::MakeId(m.Id);
    const fs::path dest = where / id;
    // Плагин уже там, куда его ставят (выбрали его же папку): удалять «старую
    // версию» значило бы стереть единственную.
    if (fs::exists(dest, ec) && fs::equivalent(source, dest, ec)) {
        if (!temp.empty()) fs::remove_all(temp, ec);
        return id;
    }
    fs::create_directories(where, ec);
    fs::remove_all(dest, ec);   // переустановка: новая версия заменяет старую
    fs::copy(source, dest, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (!temp.empty()) fs::remove_all(temp, ec);
    if (ec) {
        error = T("Could not copy the plugin: ") + ec.message();
        return {};
    }
    return id;
}

bool Remove(EditorHost& host, const std::string& id, std::string& error) {
    Entry* e = FindEntry(id);
    if (!e) {
        error = "нет плагина " + id;
        return false;
    }
    if (e->Info.From == Source::Builtin) {
        error = T("Built-in plugins can only be turned off");
        return false;
    }
    std::error_code ec;
    fs::remove_all(e->Info.Manifest.Dir, ec);
    if (ec) {
        error = ec.message();
        return false;
    }
    Rescan(host);
    return true;
}

DryRun Try(const std::string& source, const std::string& chunkName) {
    DryRun out;
    Entry e;
    e.Info.Manifest.Id = "dry";
    e.Lua = std::make_unique<sol::state>();
    BindApi(*e.Lua, &e);
    State& s = S();
    EditorHost* prev = s.Host;
    s.Host = nullptr;
    Guarded guard;
    sol::load_result chunk = e.Lua->load(source, "@" + chunkName);
    if (!chunk.valid()) {
        sol::error err = chunk;
        out.Error = err.what();
    } else {
        sol::protected_function fn = chunk;
        sol::protected_function_result r = fn();
        if (!r.valid()) {
            sol::error err = r;
            out.Error = err.what();
        } else {
            out.Ok = true;
        }
    }
    s.Host = prev;
    for (auto& m : e.Menus) out.Menus.push_back(m.Label);
    for (auto& p : e.Panels) out.Panels.push_back(p.Title);
    return out;
}

} // namespace sage::editor::luaplugins
