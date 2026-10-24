#include "LuaPlugins.h"

#include <fstream>

#include "Localization.h"
#include "sage/assets/Zip.h"
#include "sage/core/Paths.h"

// Файловая сторона плагинов: заготовки нового плагина и установка из папки
// или архива. Отдельно от LuaPlugins.cpp — здесь нет ни Lua, ни состояния
// загруженных плагинов, только файлы проекта.

namespace fs = std::filesystem;
namespace pl = sage::plugins;

namespace sage::editor::luaplugins {

namespace {
// Метка времени файла — имя временной папки распаковки, своё у каждого архива.
long long Stamp(const fs::path& p) {
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    return ec ? 0 : (long long)t.time_since_epoch().count();
}
} // namespace

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

} // namespace sage::editor::luaplugins
