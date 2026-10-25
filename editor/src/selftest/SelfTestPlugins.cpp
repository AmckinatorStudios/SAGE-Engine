#include "EditorLayer.h"

#include <chrono>
#include <fstream>
#include <sstream>

#include "LuaPlugins.h"
#include "sage/core/Paths.h"
#include "sage/scripting/Plugins.h"

namespace fs = std::filesystem;
namespace lp = sage::editor::luaplugins;

namespace {

const lp::PluginInfo* Find(const std::vector<lp::PluginInfo>& list, const std::string& id) {
    for (const lp::PluginInfo& p : list)
        if (p.Manifest.Id == id) return &p;
    return nullptr;
}

std::string ReadAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

// ПЛАГИНЫ НА LUA: созданный по шаблону плагин работает (пункт меню, окно,
// отмена одним Ctrl+Z), выключается и включается, ошибка и бесконечный цикл в
// нём не роняют редактор, а часть движка запускается в Play.
bool EditorLayer::SelfTestPlugins() {
    bool ok = true;
    auto fail = [&](const std::string& why) {
        LOG_ERROR("Editor") << "SELFTEST: plugins: " << why;
        ok = false;
    };
    std::string err;

    // Встроенный пример «Расстановка»: десять копий выделенного — и один Ctrl+Z
    // убирает их все (каждая копия кладёт свой снимок, плагин склеивает).
    {
        // Встроенный пример по умолчанию ВЫКЛЮЧЕН: он не должен появляться в
        // меню у того, кто его не просил. Включаем на время проверки и
        // возвращаем как было.
        const lp::PluginInfo* off = Find(lp::List(), "scatter");
        if (!off) fail("встроенного плагина «Расстановка» нет в списке");
        else if (off->Enabled || off->Running) fail("встроенный плагин включён по умолчанию");
        lp::SetEnabled(*this, "scatter", true, err);
        const lp::PluginInfo* scatter = Find(lp::List(), "scatter");
        if (!scatter || !scatter->Running || scatter->From != lp::Source::Builtin || scatter->Panels != 1) {
            fail("встроенный плагин «Расстановка» не запущен" +
                 std::string(scatter ? (": " + scatter->Error) : " (нет в списке)"));
        } else {
            const int cube = CreateCatalogObject("shape.cube");
            m_selection.Set({cube});
            const size_t before = m_scene->Count();
            if (!lp::RunMenu(*this, "scatter", 0)) fail("«Расстановка» упала: " + Find(lp::List(), "scatter")->Error);
            if (m_scene->Count() != before + 10)
                fail("«Расстановка» создала " + std::to_string(m_scene->Count() - before) + " копий вместо 10");
            Undo();
            if (m_scene->Count() != before) fail("один Ctrl+Z не убрал все копии «Расстановки»");
        }
        lp::SetEnabled(*this, "scatter", false, err);
    }

    const fs::path dir = lp::Create(lp::ProjectDir(*this), "Проба плагина", "selftest", lp::Template::Both, err);
    if (dir.empty()) {
        fail("шаблон не создан: " + err);
        return false;
    }
    const std::string id = sage::PathToUtf8(dir.filename());
    if (id != "proba_plagina") fail("id из кириллического имени: " + id);
    lp::Rescan(*this);

    const lp::PluginInfo* info = Find(lp::List(), id);
    if (!info || !info->Running || info->Menus != 1 || info->Panels != 1 || info->From != lp::Source::Project)
        fail("шаблонный плагин не запустился как надо" + std::string(info ? (": " + info->Error) : ""));

    // Пункт меню: создаёт куб, отменяется одним Ctrl+Z.
    if (ok) {
        if (!lp::RunMenu(*this, id, 0)) fail("пункт меню шаблона упал: " + Find(lp::List(), id)->Error);
        if (!m_scene->FindByName("Куб от плагина").Valid()) fail("пункт меню не создал куб");
        Undo();
        if (m_scene->FindByName("Куб от плагина").Valid()) fail("Ctrl+Z не убрал созданное плагином");
    }

    // Выключение: плагин проекта помнит это в plugin.json (уедет в игру так же).
    if (!lp::SetEnabled(*this, id, false, err)) fail("не выключился: " + err);
    info = Find(lp::List(), id);
    if (!info || info->Running) fail("выключенный плагин продолжает работать");
    if (ReadAll(dir / "plugin.json").find("\"enabled\": false") == std::string::npos)
        fail("выключение не записано в plugin.json");
    lp::SetEnabled(*this, id, true, err);

    // Ошибка в обработчике — в окно плагинов, редактор живёт.
    std::ofstream(dir / "editor.lua", std::ios::binary | std::ios::trunc)
        << "editor.menu('boom', function() error('плохо') end)\n"
           "editor.menu('loop', function() while true do end end)\n";
    lp::Reload(*this, id);
    lp::RunMenu(*this, id, 0);
    info = Find(lp::List(), id);
    if (!info || info->Error.find("плохо") == std::string::npos) fail("ошибка обработчика не показана");

    // Бесконечный цикл останавливается сторожем, а не вешает редактор.
    lp::Reload(*this, id);
    const auto t0 = std::chrono::steady_clock::now();
    lp::RunMenu(*this, id, 1);
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    info = Find(lp::List(), id);
    if (!info || info->Error.find("2 секунд") == std::string::npos || sec > 5.0)
        fail("бесконечный цикл не остановлен сторожем");

    // Синтаксическая ошибка — плагин не запущен, причина видна.
    std::ofstream(dir / "editor.lua", std::ios::binary | std::ios::trunc) << "editor.menu(\n";
    lp::Reload(*this, id);
    info = Find(lp::List(), id);
    if (!info || info->Running || info->Error.empty()) fail("синтаксическая ошибка не показана");

    // Часть движка — в Play: служебный объект плагина есть в сцене игры.
    StartPlay();
    if (InPlayMode()) {
        int hosts = 0;
        for (auto e : CurrentScene().Registry().view<sage::plugins::PluginHostComponent>()) {
            (void)e;
            ++hosts;
        }
        if (hosts != 1) fail("часть движка не запущена в Play (объектов плагина: " + std::to_string(hosts) + ")");
        StopPlay();
    } else {
        fail("Play не запустился");
    }

    if (!lp::Remove(*this, id, err)) fail("не удалился: " + err);
    if (Find(lp::List(), id)) fail("удалённый плагин остался в списке");
    if (ok) LOG_INFO("Editor") << "SELFTEST: plugins OK";
    return ok;
}
