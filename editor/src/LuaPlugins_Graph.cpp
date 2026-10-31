#include <fstream>
#include <string>
#include <vector>

#include <sol/sol.hpp>

#include "LogicKinds.h"
#include "aigraph/AiGraphExtensions.h"
#include "sage/core/Paths.h"
#include "sage/core/Log.h"
#include "sage/vars/Value.h"

// ---------------------------------------------------------------------------
// API плагинов редактора для SAGE AI Graph: своё поле значения типа в окне
// свойств узла и свой инструмент на панели графа (см. AiGraphExtensions.h).
// ---------------------------------------------------------------------------
namespace sage::editor::luaplugins {

namespace {

// Значение графа <-> таблицы плагина. Векторы — {x, y, z}, как везде в API
// плагинов; поворот — углами в градусах; объект — номером.
sol::object PluginValueToLua(sol::state_view lua, const sage::vars::Value& v) {
    using K = sage::vars::Kind;
    auto vec = [&](glm::vec4 x, int n) {
        sol::table t = lua.create_table();
        t["x"] = x.x;
        t["y"] = x.y;
        if (n > 2) t["z"] = x.z;
        if (n > 3) t["w"] = x.w;
        return sol::object(t);
    };
    switch (v.Type()) {
        case K::Bool: return sol::make_object(lua, v.AsBool());
        case K::Int: return sol::make_object(lua, v.AsInt());
        case K::Float: return sol::make_object(lua, v.AsFloat());
        case K::String:
        case K::Asset: return sol::make_object(lua, v.AsString());
        case K::Entity: return sol::make_object(lua, v.AsEntity().Id);
        case K::Vec2: return vec(glm::vec4(v.AsVec2(), 0, 0), 2);
        case K::Vec3:
        case K::Rotation: return vec(glm::vec4(v.AsVec3(), 0), 3);
        case K::Color: return vec(v.AsVec4(), 4);
        case K::Array: {
            sol::table t = lua.create_table();
            for (size_t i = 0; i < v.Items().size(); ++i) t[i + 1] = PluginValueToLua(lua, v.Items()[i]);
            return t;
        }
        case K::Struct: {
            sol::table t = lua.create_table();
            for (const sage::vars::Field& f : v.Fields()) t[f.first] = PluginValueToLua(lua, f.second);
            return t;
        }
    }
    return sol::lua_nil;
}

// Обратно — по виду прежнего значения: плагин правит поле, а не меняет тип.
sage::vars::Value PluginValueFromLua(const sol::object& o, const sage::vars::Value& like) {
    using K = sage::vars::Kind;
    using sage::vars::Value;
    auto num = [&](const sol::table& t, const char* k, float d) {
        sol::object x = t[k];
        return x.get_type() == sol::type::number ? x.as<float>() : d;
    };
    switch (like.Type()) {
        case K::Bool: return Value(o.is<bool>() ? o.as<bool>() : like.AsBool());
        case K::Int: return Value(o.get_type() == sol::type::number ? (int)o.as<double>() : like.AsInt());
        case K::Float: return Value(o.get_type() == sol::type::number ? o.as<float>() : like.AsFloat());
        case K::String: return Value(o.is<std::string>() ? o.as<std::string>() : like.AsString());
        case K::Asset: return Value(sage::vars::AssetRef{o.is<std::string>() ? o.as<std::string>() : like.AsString()});
        case K::Entity: return Value(sage::vars::EntityRef{o.get_type() == sol::type::number ? o.as<int>() : 0});
        default: break;
    }
    if (o.get_type() != sol::type::table) return like;
    sol::table t = o.as<sol::table>();
    switch (like.Type()) {
        case K::Vec2: return Value(glm::vec2(num(t, "x", 0), num(t, "y", 0)));
        case K::Vec3: return Value(glm::vec3(num(t, "x", 0), num(t, "y", 0), num(t, "z", 0)));
        case K::Rotation:
            return Value(glm::quat(glm::radians(glm::vec3(num(t, "x", 0), num(t, "y", 0), num(t, "z", 0)))));
        case K::Color: return Value(glm::vec4(num(t, "x", 1), num(t, "y", 1), num(t, "z", 1), num(t, "w", 1)));
        case K::Array: {
            std::vector<Value> items;
            const Value proto = like.Items().empty() ? Value(0.0f) : like.Items()[0];
            for (size_t i = 1; i <= t.size(); ++i) items.push_back(PluginValueFromLua(t[i], proto));
            return Value::Array(std::move(items));
        }
        case K::Struct: {
            Value out = like;
            for (const sage::vars::Field& f : like.Fields()) {
                sol::object x = t[f.first];
                if (x.valid() && x.get_type() != sol::type::lua_nil) out.SetField(f.first, PluginValueFromLua(x, f.second));
            }
            return out;
        }
        default: return like;
    }
}

} // namespace

void BindGraphApi(sol::table& ed, const std::string& id) {
    // --- SAGE AI Graph: своё поле значения и свой инструмент -------------------
    //
    //   editor.graph_pin("Target", function(v) v.pos = ui.vec3("Позиция", v.pos) return v end)
    //   editor.graph_tool("Разложить", function(graph) editor.log(graph.name) end)
    //
    // Поле зовётся внутри окна свойств узла — в нём работают ui.*. Вернуть
    // новое значение (или nil — ничего не менять).
    ed.set_function("graph_pin", [id](const std::string& type, sol::protected_function fn) {
        sage::editor::aigraph::PinEditorExt ext;
        ext.Owner = id;
        ext.Type = type;
        ext.Edit = [fn, id](sage::vars::Value& v) -> bool {
            sol::state_view lua(fn.lua_state());
            auto r = fn(PluginValueToLua(lua, v));
            if (!r.valid()) {
                sol::error e = r;
                LOG_ERROR("Plugin") << "[" << id << "] graph_pin: " << e.what();
                return false;
            }
            if (r.return_count() == 0) return false;
            sol::object nv = r.get<sol::object>(0);
            if (!nv.valid() || nv.get_type() == sol::type::lua_nil) return false;
            const sage::vars::Value out = PluginValueFromLua(nv, v);
            if (out == v) return false;
            v = out;
            return true;
        };
        sage::editor::aigraph::AddPinEditor(std::move(ext));
    });
    // Третий аргумент {editor = "ai" | "visual"} — кнопка только в одном из
    // редакторов графов; без него — в обоих.
    ed.set_function("graph_tool", [id](const std::string& title, sol::protected_function fn,
                                       sol::optional<sol::table> opts) {
        sage::editor::aigraph::ToolExt t;
        if (opts) t.Editor = opts->get_or<std::string>("editor", "");
        t.Owner = id;
        t.Title = title;
        t.Run = [fn, id](const sage::editor::aigraph::ToolContext& c) {
            sol::state_view lua(fn.lua_state());
            sol::table g = lua.create_table();
            g["path"] = c.Path;
            g["name"] = c.Name;
            g["editor"] = c.Editor;
            sol::table sel = lua.create_table();
            for (size_t i = 0; i < c.Selected.size(); ++i) sel[i + 1] = c.Selected[i];
            g["selected"] = sel;
            auto r = fn(g);
            if (!r.valid()) {
                sol::error e = r;
                LOG_ERROR("Plugin") << "[" << id << "] graph_tool: " << e.what();
            }
        };
        sage::editor::aigraph::AddTool(std::move(t));
    });

    // --- Свой вид логики в окне «Создать…» --------------------------------------
    //
    //   editor.logic_kind("fennel", {title = "Fennel (.fnl)", ext = ".fnl", folder = "scripts",
    //                     note = "Lisp поверх Lua", template = ";; {name}\n"})
    //
    // Файл создаётся из template, «{name}» заменяется именем. Выгрузка плагина
    // убирает вид (LogicKinds::RemoveOwner).
    ed.set_function("logic_kind", [id](const std::string& kindId, sol::table spec) {
        auto str = [&spec](const char* key, const std::string& def) {
            sol::optional<std::string> v = spec[key];
            return v ? *v : def;
        };
        sage::editor::logic::Kind k;
        k.Id = id + "." + kindId;
        k.Owner = id;
        k.Title = str("title", kindId);
        k.Note = str("note", "");
        k.Ext = str("ext", "." + kindId);
        if (k.Ext.empty() || k.Ext[0] != '.') k.Ext = "." + k.Ext;
        k.Folder = str("folder", "scripts");
        k.Slot = str("slot", "");
        k.Icon = str("icon", "script");
        const std::string tpl = str("template", "");
        const std::string ext = k.Ext;
        k.Create = [tpl, ext](const std::string& name, const std::filesystem::path& dir,
                              std::filesystem::path& created, std::string& err) {
            if (name.empty()) {
                err = "Name must not be empty";
                return false;
            }
            std::filesystem::path target = dir / sage::PathFromUtf8(name);
            if (target.extension() != ext) target += ext;
            std::error_code ec;
            if (std::filesystem::exists(target, ec)) {
                err = "Already exists: " + sage::PathToUtf8(target.filename());
                return false;
            }
            std::filesystem::create_directories(dir, ec);
            std::string text = tpl;
            const std::string stem = sage::PathToUtf8(target.stem());
            for (size_t at = text.find("{name}"); at != std::string::npos; at = text.find("{name}", at + stem.size()))
                text.replace(at, 6, stem);
            std::ofstream f(target, std::ios::binary);
            f << text;
            if (!f) {
                err = "Could not write " + sage::PathToUtf8(target);
                return false;
            }
            created = target;
            return true;
        };
        sage::editor::logic::Register(std::move(k));
    });

}

} // namespace sage::editor::luaplugins
