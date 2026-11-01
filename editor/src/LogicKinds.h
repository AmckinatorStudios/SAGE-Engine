#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

class EditorHost;

// ---------------------------------------------------------------------------
// ВИДЫ ЛОГИКИ — то, чем в проекте пишут поведение. Встроенных три:
//   SAGE Lua (.slua)      — текстовая игровая логика;
//   Script Graph (.sgraph) — визуальная игровая логика объектов;
//   AI Graph (.aigraph)   — визуальное поведение ИИ.
// Плюс то, что добавит плагин (editor.logic_kind). Обычный Lua (.lua) и
// Visual Script / задача (.vgraph) больше не создаются: старые файлы
// открываются и работают, но новых в этих форматах не бывает.
//
// ЗАЧЕМ СПИСКОМ. Окно «Создать…» знало ровно два языка, зашитых веткой
// switch: третий (визуальный скрипт, граф ИИ) создавался только в своём окне,
// а свой язык плагина не создавался вовсе. Теперь окно, меню панели Assets и
// кнопка «+» слота читают ОДИН список; новый язык — одна запись Register,
// а не правка трёх мест.
//
// Slot — какой слот ассета принимает созданное ("script" — компонент Script,
// "visual" — компонент Script Graph, "ai" — компонент AI). Слот «+»
// предлагает только свои виды. Group — раздел окна: "scripts" (игровая
// логика) или "ai" (поведение ИИ) — инструменты разного назначения стоят
// раздельно.
// ---------------------------------------------------------------------------
namespace sage::editor::logic {

struct Kind {
    std::string Id;        // "slua", "sgraph", "aigraph", "<плагин>.<имя>"
    std::string Title;     // название: «Script Graph»; ключ перевода T()
    std::string Note;      // несколько слов о назначении; ключ перевода T()
    std::string Group = "scripts";   // "scripts" или "ai"
    std::string DefaultName = "NewScript";
    std::string Ext;       // ".slua"
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

// Встроенные (SAGE Lua, Script Graph, AI Graph) — редактор
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
