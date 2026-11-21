#include "SelfTestCommon.h"

#include "LuaPlugins.h"

// РЕЕСТР ПАНЕЛЕЙ в живом редакторе: встроенные панели все на месте с тем же
// составом раскладки, что был до реестра; панель, зарегистрированная на ходу,
// сразу получает команду палитры и снимается без следа; окно Lua-плагина —
// такая же запись реестра.
bool EditorLayer::SelfTestPanelRegistry() {
    bool ok = true;
    auto fail = [&](const std::string& why) {
        LOG_ERROR("Editor") << "SELFTEST: panel-registry: " << why;
        ok = false;
    };
    namespace pe = sage::editor;

    // Встроенные: каждая видимая в меню «Окно» панель — в реестре.
    for (const char* id : {"Viewport", "Game", "Hierarchy", "Inspector", "Lighting", "InterfaceViewport",
                           "InterfaceHierarchy", "InterfaceInspector", "InterfacePreview", "Assets", "Console",
                           "Animation", "AnimGraph", "Tween", "Profiler"})
        if (!m_panelRegistry.Find(id)) fail(std::string("нет встроенной панели ") + id);

    // «Сбросить раскладку» открывает ровно прежний состав (инструменты — нет).
    m_panelRegistry.CloseAll();
    m_panelRegistry.Restore();
    for (EditorPanel p : {EditorPanel::Hierarchy, EditorPanel::Inspector, EditorPanel::Environment,
                          EditorPanel::Viewport, EditorPanel::Game, EditorPanel::Console, EditorPanel::Assets,
                          EditorPanel::InterfaceHierarchy, EditorPanel::InterfaceViewport,
                          EditorPanel::InterfaceInspector, EditorPanel::InterfacePreview})
        if (!m_panels[p]) fail("сброс раскладки не вернул встроенную панель " + std::to_string((int)p));
    if (m_panels[EditorPanel::Profiler]) fail("сброс раскладки открыл профилировщик");
    if (!m_panels[EditorPanel::Animation] || !m_panelRegistry.IsVisible("AnimGraph"))
        fail("сброс раскладки не вернул клип и граф анимации");
    for (const char* id : {"AnimViewport", "AnimHierarchy", "AnimInspector"})
        if (!m_panelRegistry.IsVisible(id)) fail(std::string("сброс раскладки не вернул ") + id);
    // Пространства: у анимации СВОИ вьюпорт, иерархия и инспектор (сцена
    // предпросмотра, animws/), панели уровня туда не попадают; без Game и Tween.
    {
        const pe::PanelSpec* vp = m_panelRegistry.Find("Viewport");
        const pe::PanelSpec* tw = m_panelRegistry.Find("Tween");
        const pe::PanelSpec* game = m_panelRegistry.Find("Game");
        for (const char* id : {"AnimViewport", "AnimHierarchy", "AnimInspector"}) {
            const pe::PanelSpec* sp = m_panelRegistry.Find(id);
            if (!sp || !pe::PanelRegistry::InSpace(*sp, EditorWorkspace::Animation) || !sp->InLayout)
                fail(std::string("у пространства анимации нет панели ") + id);
        }
        for (const char* id : {"Viewport", "Hierarchy", "Inspector"})
            if (const pe::PanelSpec* sp = m_panelRegistry.Find(id); sp && pe::PanelRegistry::InSpace(*sp, EditorWorkspace::Animation))
                fail(std::string("панель уровня попала в пространство анимации: ") + id);
        (void)vp;
        if (tw && pe::PanelRegistry::InSpace(*tw, EditorWorkspace::Animation)) fail("Tween попал в пространство анимации");
        if (game && pe::PanelRegistry::InSpace(*game, EditorWorkspace::Animation)) fail("Game попал в пространство анимации");
    }

    // Панель «со стороны»: одна регистрация — и она в палитре.
    {
        pe::PanelSpec s;
        s.Id = "SelftestNotes";
        s.Title = "Notes";
        s.Section = pe::PanelSection::Tools;
        s.Draw = [](EditorHost&, bool*, const std::string&) {};
        Panels().Register(std::move(s));
        if (!m_commands.Find("panel.selftestnotes")) fail("новая панель не получила команду палитры");
        m_commands.Run("panel.selftestnotes");
        if (!m_panelRegistry.IsVisible("SelftestNotes")) fail("команда палитры не открыла новую панель");
        Panels().Unregister("SelftestNotes");
        if (m_commands.Find("panel.selftestnotes")) fail("снятая панель осталась в палитре");
    }

    // Окно Lua-плагина — запись реестра в разделе «Плагины».
    {
        std::string err;
        namespace lp = sage::editor::luaplugins;
        const fs::path dir = lp::Create(lp::ProjectDir(*this), "Окна в реестре", "selftest", lp::Template::Editor, err);
        lp::Rescan(*this);
        const std::string key = "plugin." + sage::PathToUtf8(dir.filename()) + ".0";
        const pe::PanelSpec* spec = m_panelRegistry.Find(key);
        if (!spec || spec->Section != pe::PanelSection::Plugins) fail("окно Lua-плагина не попало в реестр: " + key);
        lp::Remove(*this, sage::PathToUtf8(dir.filename()), err);
        if (m_panelRegistry.Find(key)) fail("окно удалённого плагина осталось в реестре");
    }

    // Окно «Настройки»: управление и оформление — его разделы, а лишних
    // (постобработка — компонент камеры, среда — своё окно, шрифт) нет.
    {
        for (const char* key :
             {"screen", "quality", "lighting", "controls", "advanced", "build", "general", "appearance"})
            if (!m_settingsPanel.SelectByKey(key)) fail(std::string("в настройках нет раздела ") + key);
        for (const char* key : {"post", "environment", "interface", "shadows"})
            if (m_settingsPanel.SelectByKey(key)) fail(std::string("в настройках остался лишний раздел ") + key);
        if (!m_commands.Find("window.controls")) fail("нет команды «Управление» в палитре");
        if (m_commands.Find("play.luaTests")) fail("в палитре остался проверочный пункт Lua-тестов");
        if (m_commands.Find("play.checkScripts")) fail("в палитре осталась «Проверить скрипты»");
        m_commands.Run("window.controls");
        if (!m_panels[EditorPanel::Settings] || m_settingsPanel.Current() != SettingsPanel::Category::Controls)
            fail("«Управление» не открыло раздел окна настроек");
        // Сведения об игре — страница настроек, а не своё окно; девятина —
        // режим редактора спрайтов, а не отдельное окно.
        if (m_panelRegistry.Find("GameSettings")) fail("«Game Settings» осталось отдельным окном");
        if (m_panelRegistry.Find("NineSlice")) fail("старое окно девятины осталось в реестре");
        if (!m_commands.Find("window.buildSettings")) fail("нет команды «Build Settings» в палитре");
        m_commands.Run("window.buildSettings");
        if (!m_panels[EditorPanel::Settings] || m_settingsPanel.Current() != SettingsPanel::Category::BuildSettings)
            fail("«Build Settings» не открыло раздел окна настроек");
        if (const pe::PanelSpec* dbg = m_panelRegistry.Find("AiDebugger"); dbg && dbg->InMenu)
            fail("отладчик ИИ остался в меню Window");
        m_panels[EditorPanel::Settings] = false;
        m_settingsPanel.Select(SettingsPanel::Category::Screen);
    }

    if (ok) LOG_INFO("Editor") << "SELFTEST: panel-registry OK";
    return ok;
}
