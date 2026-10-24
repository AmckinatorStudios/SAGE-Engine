#include "EditorLayer.h"

// ---------------------------------------------------------------------------
// ВСТРОЕННЫЕ ПАНЕЛИ РЕДАКТОРА — единственное место, где они перечислены.
//
// Меню «Окно», палитра команд, «Отдельным окном», раскладка по умолчанию,
// «Сбросить раскладку» и отрисовка берут их из реестра (PanelRegistry.h).
// Порядок регистрации — порядок в меню внутри раздела; место в доке задаёт
// SceneDock/InterfaceDock.
//
// Флаги видимости встроенных панелей живут в EditorPanelVisibility (m_panels):
// по EditorPanel к ним обращаются верхняя панель и самопроверка, и реестр
// показывает тот же флаг, а не заводит второй.
// ---------------------------------------------------------------------------

using sage::editor::PanelDock;
using sage::editor::PanelSection;
using sage::editor::PanelSpec;
namespace panelspace = sage::editor::panelspace;

void EditorLayer::RegisterPanels() {
    auto make = [this](EditorPanel flag, const char* id, const char* title, const char* icon,
                       PanelSection section, auto&& draw) {
        PanelSpec s;
        s.Id = id;
        s.Title = title;
        s.Icon = icon;
        s.Section = section;
        s.Visible = &m_panels[flag];
        s.Draw = std::forward<decltype(draw)>(draw);
        return s;
    };

    // --- Сцена ---------------------------------------------------------------
    {
        PanelSpec s = make(EditorPanel::Viewport, "Viewport", "Viewport", "world", PanelSection::Scene,
                           [this](EditorHost&, bool* open, const std::string&) { m_viewport.Draw(*this, open); });
        s.Spaces = panelspace::Scene;
        s.SceneDock = {PanelDock::Center, 0};
        s.InLayout = true;
        m_panelRegistry.Register(std::move(s));
    }
    {
        // Game — ПОСЛЕ вьюпорта в том же узле: первой докнутой вкладкой
        // редактор открывался бы на игровом окне без пикинга и гизмо.
        PanelSpec s = make(EditorPanel::Game, "Game", "Game", "game", PanelSection::Scene,
                           [this](EditorHost&, bool* open, const std::string&) { m_game.Draw(*this, open); });
        s.Spaces = panelspace::Scene;
        s.SceneDock = {PanelDock::Center, 1};
        s.InLayout = true;
        m_panelRegistry.Register(std::move(s));
    }
    {
        PanelSpec s = make(EditorPanel::Hierarchy, "Hierarchy", "Hierarchy", "hierarchy", PanelSection::Scene,
                           [this](EditorHost&, bool* open, const std::string&) { m_hierarchy.Draw(*this, open); });
        s.Spaces = panelspace::Scene;
        s.SceneDock = {PanelDock::Left, 0};
        s.InLayout = true;
        m_panelRegistry.Register(std::move(s));
    }
    {
        PanelSpec s = make(EditorPanel::Inspector, "Inspector", "Inspector", "inspector", PanelSection::Scene,
                           [this](EditorHost&, bool* open, const std::string&) { m_inspector.Draw(*this, open); });
        s.Spaces = panelspace::Scene;
        s.SceneDock = {PanelDock::Right, 1};
        s.InLayout = true;
        m_panelRegistry.Register(std::move(s));
    }
    {
        // «Освещение» стало «Средой»: окно — небо, воздух и окружающий свет.
        // Имя окна осталось прежним («Lighting»): по нему ImGui помнит раскладку.
        PanelSpec s = make(EditorPanel::Environment, "Lighting", "Environment", "sun", PanelSection::Scene,
                           [this](EditorHost&, bool* open, const std::string&) { m_environment.Draw(*this, open); });
        s.Command = "panel.environment";
        s.Spaces = panelspace::Scene;
        s.SceneDock = {PanelDock::Right, 0};
        s.InLayout = true;
        m_panelRegistry.Register(std::move(s));
    }

    // --- Интерфейс -----------------------------------------------------------
    {
        PanelSpec s = make(EditorPanel::InterfaceViewport, "InterfaceViewport", "Canvas", "layout",
                           PanelSection::Interface,
                           [this](EditorHost&, bool* open, const std::string&) { m_uiViewport.Draw(*this, *open); });
        s.Spaces = panelspace::Interface;
        s.InterfaceDock = {PanelDock::Center, 0};
        s.InLayout = true;
        m_panelRegistry.Register(std::move(s));
    }
    {
        PanelSpec s = make(EditorPanel::InterfaceHierarchy, "InterfaceHierarchy", "Elements", "list",
                           PanelSection::Interface,
                           [this](EditorHost&, bool* open, const std::string&) { m_uiHierarchy.Draw(*this, *open); });
        s.Spaces = panelspace::Interface;
        s.InterfaceDock = {PanelDock::Left, 0};
        s.InLayout = true;
        m_panelRegistry.Register(std::move(s));
    }
    {
        PanelSpec s = make(EditorPanel::InterfaceInspector, "InterfaceInspector", "Element", "inspector",
                           PanelSection::Interface,
                           [this](EditorHost&, bool* open, const std::string&) { m_uiInspector.Draw(*this, *open); });
        s.Spaces = panelspace::Interface;
        s.InterfaceDock = {PanelDock::Right, 0};
        s.InLayout = true;
        m_panelRegistry.Register(std::move(s));
    }
    {
        // Предпросмотр — рядом с холстом: «верстаю» и «смотрю» одним щелчком.
        PanelSpec s = make(EditorPanel::InterfacePreview, "InterfacePreview", "Preview", "eye",
                           PanelSection::Interface,
                           [this](EditorHost&, bool* open, const std::string&) { m_uiPreview.Draw(*this, *open); });
        s.Spaces = panelspace::Interface;
        s.InterfaceDock = {PanelDock::Center, 1};
        s.InLayout = true;
        s.Detachable = false;
        m_panelRegistry.Register(std::move(s));
    }

    // --- Общие: своим окном в каждом пространстве (PanelWindowId.h) ----------
    {
        PanelSpec s = make(EditorPanel::Assets, "Assets", "Assets", "folder-full", PanelSection::Shared,
                           [this](EditorHost&, bool* open, const std::string& id) { m_assets.Draw(*this, open, id); });
        s.PerSpaceWindow = true;
        s.SceneDock = {PanelDock::Bottom, 1};
        s.InterfaceDock = {PanelDock::Bottom, 0};
        s.InLayout = true;
        m_panelRegistry.Register(std::move(s));
    }
    {
        PanelSpec s = make(EditorPanel::Console, "Console", "Console", "console", PanelSection::Shared,
                           [this](EditorHost&, bool* open, const std::string& id) { m_console.Draw(open, id, this); });
        s.PerSpaceWindow = true;
        s.SceneDock = {PanelDock::Bottom, 0};
        s.InterfaceDock = {PanelDock::Bottom, 1};
        s.InLayout = true;
        m_panelRegistry.Register(std::move(s));
    }
    {
        // Анимация и Tween сами решают про своё окно и про закрытое состояние:
        // Tween по закрытию возвращает значения, которые показывал просмотр.
        PanelSpec s = make(EditorPanel::Animation, "Animation", "Animation", "anim", PanelSection::Shared,
                           [this](EditorHost&, bool* open, const std::string& id) { m_animation.Draw(*this, open, id); });
        s.PerSpaceWindow = true;
        s.SceneDock = {PanelDock::Bottom, 2};
        s.InterfaceDock = {PanelDock::Bottom, 2};
        s.WrapWindow = false;
        s.DrawWhenHidden = true;
        m_panelRegistry.Register(std::move(s));
    }
    {
        PanelSpec s = make(EditorPanel::Tween, "Tween", "Tween", "clock", PanelSection::Shared,
                           [this](EditorHost&, bool* open, const std::string& id) { m_tween.Draw(*this, open, id); });
        s.PerSpaceWindow = true;
        s.SceneDock = {PanelDock::Bottom, 3};
        s.InterfaceDock = {PanelDock::Bottom, 3};
        s.WrapWindow = false;
        s.DrawWhenHidden = true;
        m_panelRegistry.Register(std::move(s));
    }

    // --- Инструменты: открывают под задачу, в раскладку не входят -----------
    {
        PanelSpec s = make(EditorPanel::Profiler, "Profiler", "Profiler", "chart", PanelSection::Tools,
                           [this](EditorHost&, bool* open, const std::string&) { m_profiler.Draw(open); });
        s.Detachable = false;
        m_panelRegistry.Register(std::move(s));
    }

}
