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
                           [this](EditorHost&, bool* open, const std::string& id) { m_viewport.Draw(*this, open, id); });
        // Вьюпорт уровня — только в сцене: у пространства Animation свой
        // (сцена предпросмотра, animws/AnimStage.h).
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
                           [this](EditorHost&, bool* open, const std::string& id) { m_hierarchy.Draw(*this, open, id); });
        s.Spaces = panelspace::Scene;
        s.SceneDock = {PanelDock::Left, 0};
        s.InLayout = true;
        m_panelRegistry.Register(std::move(s));
    }
    {
        PanelSpec s = make(EditorPanel::Inspector, "Inspector", "Inspector", "inspector", PanelSection::Scene,
                           [this](EditorHost&, bool* open, const std::string& id) { m_inspector.Draw(*this, open, id); });
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

    // --- Анимация: своя иерархия, вьюпорт и инспектор (animws/AnimPanels.h) --
    {
        auto animPanel = [this](const char* id, const char* title, const char* icon, PanelDock dock, auto&& draw) {
            PanelSpec s;
            s.Id = id;
            s.Title = title;
            s.Icon = icon;
            s.Section = PanelSection::Animation;
            s.Spaces = panelspace::Animation;
            s.AnimationDock = {dock, 0};
            s.InLayout = true;
            s.DefaultVisible = true;
            s.WrapWindow = false;
            s.Draw = std::forward<decltype(draw)>(draw);
            m_panelRegistry.Register(std::move(s));
        };
        animPanel("AnimViewport", "Viewport", "world", PanelDock::Center,
                  [this](EditorHost& host, bool* open, const std::string& id) { m_animViewport.Draw(host, open, id); });
        animPanel("AnimHierarchy", "Animation Hierarchy", "hierarchy", PanelDock::Left,
                  [this](EditorHost& host, bool* open, const std::string& id) { m_animHierarchy.Draw(host, open, id); });
        animPanel("AnimInspector", "Animation Inspector", "inspector", PanelDock::Right,
                  [this](EditorHost& host, bool* open, const std::string& id) { m_animInspector.Draw(host, open, id); });
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
        s.Spaces = panelspace::All;
        s.SceneDock = {PanelDock::Bottom, 1};
        s.InterfaceDock = {PanelDock::Bottom, 0};
        s.AnimationDock = {PanelDock::Bottom, 2};
        s.InLayout = true;
        m_panelRegistry.Register(std::move(s));
    }
    {
        PanelSpec s = make(EditorPanel::Console, "Console", "Console", "console", PanelSection::Shared,
                           [this](EditorHost&, bool* open, const std::string& id) { m_console.Draw(open, id, this); });
        s.PerSpaceWindow = true;
        s.Spaces = panelspace::All;
        s.SceneDock = {PanelDock::Bottom, 0};
        s.InterfaceDock = {PanelDock::Bottom, 1};
        s.AnimationDock = {PanelDock::Bottom, 3};
        s.InLayout = true;
        m_panelRegistry.Register(std::move(s));
    }
    {
        // Клип анимации — в пространстве анимации, вкладкой рядом с Assets и
        // Console; играет он на объекте сцены, и результат виден в общем
        // вьюпорте. В вёрстке он остаётся: им анимируют элементы интерфейса.
        // Сам решает про своё окно и закрытое состояние.
        PanelSpec s = make(EditorPanel::Animation, "Animation", "Animation Timeline", "anim", PanelSection::Animation,
                           [this](EditorHost&, bool* open, const std::string& id) { m_animation.Draw(*this, open, id); });
        s.Spaces = panelspace::Interface | panelspace::Animation;
        s.PerSpaceWindow = true;
        s.InterfaceDock = {PanelDock::Bottom, 2};
        s.AnimationDock = {PanelDock::Bottom, 0};
        s.InLayout = true;
        s.WrapWindow = false;
        s.DrawWhenHidden = true;
        m_panelRegistry.Register(std::move(s));
    }
    {
        // Граф анимации — вкладкой рядом с клипом. Предпросмотра своего нет:
        // граф играет на выбранном объекте в общем вьюпорте.
        PanelSpec s;
        s.Id = "AnimGraph";
        s.Title = "Animation Graph";
        s.Icon = "wire";
        s.Section = PanelSection::Animation;
        s.Spaces = panelspace::Animation;
        s.AnimationDock = {PanelDock::Bottom, 1};
        s.InLayout = true;
        s.DefaultVisible = true;
        s.WrapWindow = false;
        s.DrawWhenHidden = true;   // закрытое окно возвращает объекту его граф
        s.Draw = [this](EditorHost& host, bool* open, const std::string& id) { m_animGraph.Draw(host, open, id); };
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
        // SAGE AI Graph и его отладчик — окна под задачу: флаг видимости
        // держит реестр (Visible = nullptr), окна рисуют себя сами.
        PanelSpec s;
        s.Id = "AiGraph";
        s.Title = "AI Graph";
        s.Icon = "wire";
        s.Section = PanelSection::Tools;
        s.WrapWindow = false;
        s.Draw = [this](EditorHost&, bool* open, const std::string&) { m_aiGraph.Draw(*this, open); };
        m_panelRegistry.Register(std::move(s));
    }
    {
        PanelSpec s;
        s.Id = "VisualScript";
        s.Title = "Script Graph";
        s.Icon = "wire";
        s.Section = PanelSection::Tools;
        s.WrapWindow = false;
        s.Draw = [this](EditorHost&, bool* open, const std::string&) { m_visualScript.Draw(*this, open); };
        m_panelRegistry.Register(std::move(s));
    }
    {
        // Редактор спрайтов — своё рабочее место с доком (рисует себя само).
        PanelSpec s;
        s.Id = "SpriteSheet";
        s.Title = "Sprite Editor";
        s.Icon = "grid";
        s.Section = PanelSection::Tools;
        s.WrapWindow = false;
        s.Draw = [this](EditorHost& host, bool* open, const std::string&) { m_spriteSheet.Draw(host, open); };
        m_panelRegistry.Register(std::move(s));
    }
    {
        PanelSpec s;
        s.Id = "AiDebugger";
        s.Title = "AI Debugger";
        s.Icon = "debug";
        s.Section = PanelSection::Tools;
        // Открывается кнопкой окна AI Graph — в меню Window его нет.
        s.InMenu = false;
        s.WrapWindow = false;
        s.Draw = [this](EditorHost&, bool* open, const std::string&) { m_aiDebug.Draw(*this, open, m_aiGraph); };
        m_panelRegistry.Register(std::move(s));
    }
    {
        PanelSpec s = make(EditorPanel::Profiler, "Profiler", "Profiler", "chart", PanelSection::Tools,
                           [this](EditorHost&, bool* open, const std::string&) { m_profiler.Draw(open); });
        s.Detachable = false;
        m_panelRegistry.Register(std::move(s));
    }

}
