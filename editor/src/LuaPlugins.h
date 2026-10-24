#pragma once
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "sage/scripting/Plugins.h"

class EditorHost;

// ---------------------------------------------------------------------------
// ПЛАГИНЫ РЕДАКТОРА НА LUA — часть «editor» плагина (sage/scripting/Plugins.h).
//
// Файл editor.lua выполняется в СВОЁМ состоянии Lua (не в игровом): у
// редактора и у игры разное время жизни, и плагин, сломавшийся в редакторе, не
// должен ронять Play, а скрипт игры — видеть меню редактора. Что доступно:
//
//   editor.menu("Расставить деревья", function() ... end)  -- меню «Плагины»
//   editor.panel("Мой инструмент", function() ui.text("…") end)
//   editor.on("selection", function(obj) ... end)   -- update/selection/play/stop/project
//   editor.create("shape.cube"), editor.selected(), editor.find("Игрок") …
//   ui.button("Жми") / ui.slider("Сила", v, 0, 10) / ui.input("Имя", s) …
//
// Полный список — docs/plugins.md. Ошибка в плагине пишется в консоль и в окно
// «Плагины», а сам плагин замолкает до перезагрузки: падающий каждый кадр
// обработчик забил бы консоль за секунду. Правка editor.lua подхватывается на
// лету — сохранил файл, и плагин перезапущен.
// ---------------------------------------------------------------------------
namespace sage::editor::luaplugins {

// Откуда плагин. От этого зависит, где хранится «включён ли»: у плагина
// проекта — в его plugin.json (он уезжает в игру вместе с проектом), у
// встроенного и «моего» — в настройках редактора (папка встроенных может быть
// только для чтения, а «мой» плагин общий для всех проектов).
enum class Source { Builtin, User, Project };

struct Entry;   // внутреннее состояние загруженного плагина

struct PluginInfo {
    sage::plugins::Manifest Manifest;
    Source From = Source::User;
    bool Enabled = true;
    bool Running = false;     // часть редактора загружена и работает
    std::string Error;        // последняя ошибка (загрузка или обработчик)
    int Menus = 0, Panels = 0;
};

// Папки, где ищутся плагины.
std::filesystem::path BuiltinDir();                 // <редактор>/plugins
std::filesystem::path UserDir();                    // <настройки>/plugins
std::filesystem::path ProjectDir(EditorHost& host); // <проект>/plugins

// Пересканировать все папки и (пере)загрузить части редактора включённых
// плагинов. Зовётся при старте и при смене проекта.
void Rescan(EditorHost& host);
// Раз за кадр: перезагрузка изменённых файлов, события update/selection/play/stop.
void Tick(EditorHost& host, float dt);
// Пункты плагинов в меню «Плагины» и их окна.
void DrawMenuItems(EditorHost& host);
// Окна плагинов рисует реестр панелей редактора (PanelRegistry.h): они живут
// в меню «Окно» → «Плагины», в палитре команд и в доке, как встроенные.
// Перед выходом из редактора (до разрушения ImGui).
void Shutdown();

std::vector<PluginInfo> List();
// Выполнить пункт меню плагина (номер — в порядке editor.menu). Для самопроверки:
// щелчок по меню из неё не сделать, а проверить нужно ровно то, что он делает.
bool RunMenu(EditorHost& host, const std::string& id, int index);
bool SetEnabled(EditorHost& host, const std::string& id, bool enabled, std::string& error);
bool Reload(EditorHost& host, const std::string& id);
// Настройки плагина (plugin.json): правит окно «Плагины».
bool SaveSettings(EditorHost& host, const std::string& id, const sage::vars::Table& settings,
                  std::string& error);

// Новый плагин по шаблону: папка, plugin.json и файлы Lua с рабочим примером.
// where — UserDir() или ProjectDir(). Возвращает папку плагина.
enum class Template { Editor, Runtime, Both };
std::filesystem::path Create(const std::filesystem::path& where, const std::string& name,
                             const std::string& author, Template kind, std::string& error);
// Шаблон текста — отдельно, чтобы его проверял тест (пример обязан работать).
std::string TemplateEditorLua(const std::string& name);
std::string TemplateRuntimeLua(const std::string& id, const std::string& name);

// Установить плагин из папки или .zip (внутри — папка с plugin.json или сам
// plugin.json в корне). Копируется в where. Возвращает id.
std::string Install(const std::filesystem::path& from, const std::filesystem::path& where,
                    std::string& error);
// Удалить плагин с диска (папку целиком).
bool Remove(EditorHost& host, const std::string& id, std::string& error);

// Выполнить editor.lua без редактора вокруг — для теста: ошибки разбора и
// регистрация меню/окон. host может быть nullptr (тогда API сцены молчит).
struct DryRun {
    bool Ok = false;
    std::string Error;
    std::vector<std::string> Menus;
    std::vector<std::string> Panels;
};
DryRun Try(const std::string& source, const std::string& chunkName);

} // namespace sage::editor::luaplugins
