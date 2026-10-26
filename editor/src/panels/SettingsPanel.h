#pragma once
#include <string>
#include <vector>

#include "sage/core/Config.h"
#include "sage/core/Display.h"

class EditorHost;

namespace sage::editor::settingsui {
struct Ctx;
}

// ---------------------------------------------------------------------------
// «НАСТРОЙКИ ИГРЫ» — качество и цена кадра проекта (EngineConfig, sage.cfg) и
// общая постобработка сцены.
//
// Разделы слева, параметры раздела — группами справа, внизу — действия:
//   • Применить — записать в проект (sage.cfg): с ним настройки уезжают в
//     собранную игру. Картинка в редакторе меняется сразу, без этой кнопки —
//     её задача сохранить, а не «включить»;
//   • Отмена — вернуть то, что сохранено в проекте (отменить несохранённое);
//   • По умолчанию — значения движка для ТЕКУЩЕГО раздела.
// Режим экрана проверяется отдельно: «Проверить на этом экране» ставит его
// окну редактора, спрашивает «оставить?» и сам возвращает прежний, если ответа
// нет.
// ---------------------------------------------------------------------------
class SettingsPanel {
public:
    enum class Category { Screen, Quality, Shadows, Lighting, Environment, Post, Interface, Advanced, Count };

    void Draw(EditorHost& host, bool& open);

    // Для самопроверки и команд.
    Category Current() const { return m_cat; }
    void Select(Category c) { m_cat = c; }
    void SetSearch(const char* text);
    // Раздел по ключу ("screen", "quality", … "advanced"); false — нет такого.
    bool SelectByKey(const char* key);
    bool HasUnsavedChanges(const EditorHost& host) const;
    // Записать в проект (как кнопка «Применить»).
    bool SaveToProject(EditorHost& host);
    // Вернуть сохранённое (как кнопка «Отмена»).
    void RevertUnsaved(EditorHost& host);
    void ResetCategory(EditorHost& host, Category c);
    const sage::display::DisplayTrial& Trial() const { return m_trial; }

private:
    void SyncSaved(EditorHost& host);
    void DrawNav(EditorHost& host);
    void DrawFooter(EditorHost& host);
    void DrawTrialPopup(EditorHost& host);
    void DrawCategory(EditorHost& host, Category c, sage::editor::settingsui::Ctx& ctx);

    // Разделы (SettingsPanel_Screen.cpp, SettingsPanel_Sections.cpp).
    void DrawScreen(EditorHost& host, sage::editor::settingsui::Ctx& ctx);
    void DrawQuality(EditorHost& host, sage::editor::settingsui::Ctx& ctx);
    void DrawShadows(EditorHost& host, sage::editor::settingsui::Ctx& ctx);
    void DrawLighting(EditorHost& host, sage::editor::settingsui::Ctx& ctx);
    void DrawEnvironment(EditorHost& host, sage::editor::settingsui::Ctx& ctx);
    void DrawPost(EditorHost& host, sage::editor::settingsui::Ctx& ctx);
    void DrawInterface(EditorHost& host, sage::editor::settingsui::Ctx& ctx);
    void DrawAdvanced(EditorHost& host, sage::editor::settingsui::Ctx& ctx);

    const std::vector<sage::display::Monitor>& Monitors();

    Category m_cat = Category::Screen;
    char m_search[96] = {};
    sage::EngineConfig m_saved;        // как лежит в проекте (sage.cfg)
    std::string m_savedFor;            // для какого проекта снят m_saved
    sage::display::DisplayTrial m_trial;
    std::string m_trialError;
    bool m_openTrialPopup = false;
    std::vector<sage::display::Monitor> m_monitors;
    double m_monitorsAt = -100.0;
};
