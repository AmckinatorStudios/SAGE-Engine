#pragma once
#include <string>
#include <vector>

#include "InputPanel.h"
#include "sage/core/Config.h"
#include "sage/core/Display.h"

class EditorHost;

namespace sage::editor::settingsui {
struct Ctx;
}

// ---------------------------------------------------------------------------
// «НАСТРОЙКИ» — одно окно на всё, что настраивают, а не строят:
//   • игра (уезжает в сборку): экран, качество графики, освещение и тени,
//     управление, дополнительно;
//   • редактор (живёт в профиле человека, а не в проекте): общие (язык,
//     программа для кода) и оформление.
//
// Постобработки, среды и шрифта интерфейса здесь нет: постобработка — компонент
// камеры, небо и туман — окно «Среда», шрифт — элементы интерфейса. Второе
// место правки того же самого только путало, какое из двух главнее.
//
// Внизу — действия раздела:
//   • Применить — записать в проект: sage.cfg (или input.sageinput в
//     «Управлении»). Картинка в редакторе меняется сразу, без этой кнопки —
//     её задача сохранить, а не «включить»;
//   • Отмена — вернуть то, что сохранено в проекте;
//   • По умолчанию — значения движка для ТЕКУЩЕГО раздела.
// Разделы редактора применяются сразу и сохраняются в профиле — кнопок у них нет.
// Режим экрана проверяется отдельно: «Проверить на этом экране» ставит его
// окну редактора, спрашивает «оставить?» и сам возвращает прежний, если ответа
// нет.
// ---------------------------------------------------------------------------
class SettingsPanel {
public:
    // Порядок — порядок списка слева; General — первый раздел редактора.
    enum class Category { Screen, Quality, Lighting, Controls, Advanced, General, Appearance, Count };
    // Раздел редактора (профиль человека, применяется сразу), а не игры.
    static bool IsEditorCategory(Category c) { return c == Category::General || c == Category::Appearance; }

    void Draw(EditorHost& host, bool& open);

    // Для самопроверки и команд.
    Category Current() const { return m_cat; }
    void Select(Category c) { m_cat = c; }
    void SetSearch(const char* text);
    // Размер окна при следующем открытии — для скриншот-проверки узкого окна.
    void ForceSize(float w, float h) { m_forceW = w; m_forceH = h; }
    // Раздел по ключу ("screen", "quality", "lighting", "controls", "advanced",
    // "general", "appearance"); false — нет такого.
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
    void DrawLighting(EditorHost& host, sage::editor::settingsui::Ctx& ctx);
    void DrawAdvanced(EditorHost& host, sage::editor::settingsui::Ctx& ctx);
    void DrawGeneral(EditorHost& host, sage::editor::settingsui::Ctx& ctx);
    void DrawAppearance(EditorHost& host, sage::editor::settingsui::Ctx& ctx);

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
    InputPanel m_controls;             // раздел «Управление» (input.sageinput)
    float m_forceW = 0.0f, m_forceH = 0.0f;
};
