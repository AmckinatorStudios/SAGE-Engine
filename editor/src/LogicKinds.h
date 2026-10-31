#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

class EditorHost;

// ---------------------------------------------------------------------------
// ВИДЫ ЛОГИКИ — список языков и графов, которыми в проекте пишут поведение:
// Lua, SAGE Lua, Visual Script, задача Visual Scripting, AI Graph — и всё, что
// добавит плагин (editor.logic_kind).
//
// ЗАЧЕМ СПИСКОМ. Окно «Создать…» знало ровно два языка, зашитых веткой
// switch: третий (визуальный скрипт, граф ИИ) создавался только в своём окне,
// а свой язык плагина не создавался вовсе. Теперь окно, меню панели Assets и
// кнопка «+» слота читают ОДИН список; новый язык — одна запись Register,
// а не правка трёх мест.
//
// Slot — какой слот ассета принимает созданное ("script" — компонент Script,
// "visual" — Visual Script, "ai" — компонент AI, "task" — задача, которую
// зовут по имени). Слот «+» предлагает только свои виды.
// ---------------------------------------------------------------------------
namespace sage::editor::logic {

struct Kind {
    std::string Id;        // "lua", "slua", "visual", "task", "ai", "<плагин>.<имя>"
    std::string Title;     // название: «Lua (.lua)»; ключ перевода T() (имена языков — как есть)
    std::string Note;      // одна строка, чем он хорош; английский ключ перевода T()
    std::string Ext;       // ".lua"
    std::string Folder;    // папка от assets/ по умолчанию: "scripts", "visual", "ai"
    // Создаётся только в своей папке: граф задачи и AI Graph ищутся
    // библиотекой ИИ по имени в assets/visual и assets/ai, а не где угодно.
    bool OwnFolderOnly = false;
    std::string Icon = "script";
    std::string Slot;      // см. выше
    std::string Owner;     // id плагина; пусто — встроенный
    // Создать файл name + Ext в папке dir. created — путь созданного.
    std::function<bool(const std::string& name, const std::filesystem::path& dir,
                       std::filesystem::path& created, std::string& err)>
        Create;
    // Открыть созданное в его редакторе (необязательно).
    std::function<void(EditorHost&, const std::filesystem::path&)> Open;
};

// Встроенные (Lua, SAGE Lua, Visual Script, задача, AI Graph) — редактор
// регистрирует при запуске (LogicKindsBuiltin.cpp).
void RegisterBuiltins();
// Тот же Id второй раз — замена записи (перезагрузка плагина).
void Register(Kind kind);
void RemoveOwner(const std::string& owner);
const std::vector<Kind>& All();
const Kind* Find(const std::string& id);
// Виды для слота (пусто — все).
std::vector<const Kind*> ForSlot(const std::string& slot);

} // namespace sage::editor::logic
