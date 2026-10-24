#pragma once
#include <filesystem>
#include <string>

class EditorHost;
class PluginManager;
class EditorPluginContext;

// ---------------------------------------------------------------------------
// ОКНО «ПЛАГИНЫ» — всё о плагинах в одном месте: список (Lua и C++), включить
// и выключить галкой, настройки, перезагрузить, открыть файлы, установить из
// папки или архива, удалить и СОЗДАТЬ новый по шаблону.
//
// ЗАЧЕМ. Раньше плагины были только C++, включались переменной окружения
// SAGE_EDITOR_PLUGINS=1 и не показывались нигде: понять, загрузился ли плагин
// и почему нет, можно было лишь по логу. Человеку, который хочет «кнопку,
// расставляющую деревья», нужен путь в три щелчка: «Создать плагин…», имя,
// «Создать» — и открытый в редакторе кода файл с рабочим примером.
// ---------------------------------------------------------------------------
namespace sage::editor::pluginswindow {

struct Native {
    PluginManager* Manager = nullptr;
    EditorPluginContext* Context = nullptr;
    std::filesystem::path Dir;   // каталог нативных плагинов (.so/.dll)
};

void Open();
bool IsOpen();
// Окно «Создать плагин» — сразу, минуя список (пункт меню «Создать плагин…»).
void OpenCreate();
// Выделить плагин в списке (по id).
void Select(const std::string& id);
void Draw(EditorHost& host, const Native& native);

// Включён ли нативный плагин (настройки редактора). По умолчанию — нет: код
// C++ работает без песочницы, и включать его должен человек, а не сам факт
// появления файла в папке.
bool NativeEnabled(const std::filesystem::path& file);

} // namespace sage::editor::pluginswindow
