// Реестр панелей редактора (editor/src/PanelRegistry.h): панель описывается
// один раз, а меню, команды, раскладка и сброс читают реестр.
#include "TestFramework.h"

#include "../editor/src/PanelRegistry.h"
#include "../editor/src/ui/Commands.h"

namespace pe = sage::editor;

namespace {
pe::PanelSpec Spec(const char* id, pe::PanelSection section) {
    pe::PanelSpec s;
    s.Id = id;
    s.Title = id;
    s.Section = section;
    return s;
}
} // namespace

TEST(PanelRegistry_menu_order_is_section_then_registration) {
    pe::PanelRegistry reg;
    reg.Register(Spec("Tools1", pe::PanelSection::Tools));
    reg.Register(Spec("SceneB", pe::PanelSection::Scene));
    reg.Register(Spec("SceneA", pe::PanelSection::Scene));
    const auto all = reg.All();
    CHECK_EQ((int)all.size(), 3);
    CHECK_EQ(all[0]->Id, std::string("SceneB"));   // раздел «Сцена» первым,
    CHECK_EQ(all[1]->Id, std::string("SceneA"));   // внутри — как регистрировали
    CHECK_EQ(all[2]->Id, std::string("Tools1"));
}

TEST(PanelRegistry_new_panel_gets_a_command_and_can_be_removed) {
    Sage::UI::CommandRegistry commands;
    pe::PanelRegistry reg;
    reg.AttachCommands(commands, "Window");
    reg.Register(Spec("Notes", pe::PanelSection::Tools));   // уже после AttachCommands
    CHECK_TRUE(commands.Find("panel.notes") != nullptr);
    CHECK_FALSE(reg.IsVisible("Notes"));
    CHECK_TRUE(commands.Run("panel.notes"));
    CHECK_TRUE(reg.IsVisible("Notes"));
    CHECK_TRUE(reg.Unregister("Notes"));
    CHECK_TRUE(commands.Find("panel.notes") == nullptr);   // в палитре не остаётся мёртвой строки
    CHECK_TRUE(reg.Find("Notes") == nullptr);
}

TEST(PanelRegistry_restore_opens_only_layout_panels_and_keeps_external_flags) {
    bool builtin = false;
    pe::PanelRegistry reg;
    pe::PanelSpec a = Spec("Hierarchy", pe::PanelSection::Scene);
    a.InLayout = true;
    a.Visible = &builtin;   // флаг живёт снаружи (EditorPanelVisibility)
    reg.Register(a);
    reg.Register(Spec("Profiler", pe::PanelSection::Tools));
    CHECK_FALSE(reg.AnyVisible());
    reg.Restore();
    CHECK_TRUE(builtin);                      // реестр правит ТОТ ЖЕ флаг
    CHECK_FALSE(reg.IsVisible("Profiler"));   // инструмент раскладкой не открывается
    CHECK_TRUE(reg.AnyVisible());
    reg.CloseAll();
    CHECK_FALSE(builtin);
}

TEST(PanelRegistry_replacing_a_panel_keeps_its_visibility_and_menu_place) {
    pe::PanelRegistry reg;
    reg.Register(Spec("First", pe::PanelSection::Plugins));
    reg.Register(Spec("Second", pe::PanelSection::Plugins));
    reg.SetVisible("First", true);
    pe::PanelSpec again = Spec("First", pe::PanelSection::Plugins);
    again.Title = "First v2";   // плагин перезагрузился
    reg.Register(again);
    CHECK_EQ((int)reg.Count(), 2);
    CHECK_TRUE(reg.IsVisible("First"));
    CHECK_EQ(reg.All()[0]->Title, std::string("First v2"));
}

TEST(PanelRegistry_layout_docks_by_space_and_order) {
    pe::PanelRegistry reg;
    pe::PanelSpec game = Spec("Game", pe::PanelSection::Scene);
    game.Spaces = pe::panelspace::Scene;
    game.SceneDock = {pe::PanelDock::Center, 1};
    pe::PanelSpec view = Spec("Viewport", pe::PanelSection::Scene);
    view.Spaces = pe::panelspace::Scene;
    view.SceneDock = {pe::PanelDock::Center, 0};
    pe::PanelSpec assets = Spec("Assets", pe::PanelSection::Shared);
    assets.PerSpaceWindow = true;
    assets.SceneDock = {pe::PanelDock::Bottom, 0};
    assets.InterfaceDock = {pe::PanelDock::Bottom, 0};
    reg.Register(game);   // зарегистрирован раньше, но докается вторым
    reg.Register(view);
    reg.Register(assets);

    std::vector<std::string> scene;
    reg.ForEachDocked(EditorWorkspace::Scene, [&](const std::string& id, pe::PanelDock) { scene.push_back(id); });
    CHECK_EQ((int)scene.size(), 3);
    CHECK_EQ(scene[0], std::string("Viewport"));   // первая вкладка узла — выбранная
    CHECK_EQ(scene[1], std::string("Assets"));
    CHECK_EQ(scene[2], std::string("Game"));

    std::vector<std::string> ui;
    reg.ForEachDocked(EditorWorkspace::Interface, [&](const std::string& id, pe::PanelDock) { ui.push_back(id); });
    CHECK_EQ((int)ui.size(), 1);
    CHECK_EQ(ui[0], std::string("AssetsUI"));      // своё окно в пространстве интерфейса
}
