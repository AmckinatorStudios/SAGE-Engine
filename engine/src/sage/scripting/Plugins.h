#pragma once
#include <filesystem>
#include <string>
#include <vector>

#include "sage/vars/Table.h"

class Scene;

// ---------------------------------------------------------------------------
// ПЛАГИНЫ НА LUA — папка с plugin.json и одним-двумя файлами Lua.
//
//   plugins/hello/
//     plugin.json   — имя, версия, автор, описание, «включён ли»
//     runtime.lua   — часть ДВИЖКА: работает в Play и в собранной игре
//     editor.lua    — часть РЕДАКТОРА: меню, окна, команды (editor/src/LuaPlugins.h)
//
// ЗАЧЕМ ТАК. Плагин C++ (editor/src/PluginAPI.h) требует компилятора, сборки
// под каждую платформу и пересборки с каждой версией редактора, и касается
// только редактора. Человеку, который хочет «кнопку, расставляющую деревья» или
// «счётчик монет для всех уровней», это непосильный порог. Lua уже знаком —
// на нём пишутся скрипты объектов, — и плагин на нём не нужно собирать вовсе.
//
// ЧАСТЬ ДВИЖКА — обычный скрипт (тот же язык, тот же API, те же Start/Update),
// только объект у него служебный: его создаёт сама система скриптов при
// запуске каждой сцены (AttachPlugins), в списке сцены и в файле его нет.
// Настройки плагина — переменные верхнего уровня runtime.lua (как у скрипта
// объекта): `speed = 5` в файле — это поле «speed» в окне «Плагины».
// Своё API плагин отдаёт скриптам игры через plugins.Register / plugins.Get.
//
// Где лежат: <проект>/plugins/ (уезжают в сборку игры вместе с проектом) и
// папка плагинов редактора (общая для всех проектов, только редактор).
// ---------------------------------------------------------------------------
namespace sage::plugins {

struct Manifest {
    std::string Id;            // имя папки, если в файле не указано
    std::string Name;
    std::string Version = "1.0.0";
    std::string Author;
    std::string Description;
    std::string Runtime;       // файл части движка (относительно папки), пусто — нет
    std::string Editor;        // файл части редактора, пусто — нет
    bool Enabled = true;
    sage::vars::Table Settings; // значения настроек, поверх объявленных в runtime.lua
    std::filesystem::path Dir;  // папка плагина
    std::string Error;          // plugin.json не прочитался — почему (плагин не грузится)

    bool HasRuntime() const { return !Runtime.empty(); }
    bool HasEditor() const { return !Editor.empty(); }
    std::filesystem::path RuntimePath() const { return Dir / Runtime; }
    std::filesystem::path EditorPath() const { return Dir / Editor; }
    const std::string& Title() const { return Name.empty() ? Id : Name; }
};

constexpr const char* kManifestFile = "plugin.json";

// Разбор plugin.json (текст уже прочитан). dir — папка плагина: из неё берётся
// Id, если в файле его нет. Ошибка разбора — в out.Error, а не исключением:
// один сломанный плагин не должен прятать остальные.
Manifest ParseManifest(const std::string& text, const std::filesystem::path& dir);
std::string ManifestToText(const Manifest& m);

// Читает <dir>/plugin.json (через vfs: в собранной игре плагины лежат в пакете).
bool ReadManifest(const std::filesystem::path& dir, Manifest& out);
// Записывает plugin.json на диск (включение, настройки).
bool WriteManifest(const Manifest& m, std::string& error);

// Все плагины в папке (каждая подпапка с plugin.json), по имени.
std::vector<Manifest> Scan(const std::filesystem::path& pluginsDir);

// Id из имени, которое ввёл человек: «Мой Плагин!» -> «moy_plagin». Id — это
// имя папки и ключ plugins.Get, поэтому только латиница, цифры и «_».
std::string MakeId(const std::string& name);

// Служебный объект плагина: по нему его не пишут в файл сцены, не показывают
// в иерархии и не путают с объектами игры.
struct PluginHostComponent {
    std::string Id;
};

} // namespace sage::plugins
