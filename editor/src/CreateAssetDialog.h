#pragma once
#include <filesystem>
#include <functional>
#include <string>

class EditorHost;

// ---------------------------------------------------------------------------
// ОКНО «СОЗДАТЬ…» — одно на весь редактор: меню панели Assets и кнопка «+»
// в любом слоте ассета (материал, скрипт, сцена) открывают ЕГО.
//
// ЗАЧЕМ. В меню панели стояло шесть пунктов «Новый …» — и два из них были
// одним действием: «Новый скрипт (.lua)» и «Новый скрипт SAGE Lua (.slua)».
// Человеку, который хочет скрипт, приходилось выбирать язык по расширению в
// пункте меню, ещё не видя, чем они отличаются. Теперь пункт один — «Скрипт…»,
// а язык выбирается в окне, с объяснением. Так же папка получает цвет сразу
// при создании, а не вторым заходом в меню.
//
// А слот без этого окна умел только НАЗНАЧИТЬ готовое: чтобы дать объекту
// новый скрипт, надо было уйти в панель Assets, создать файл, вернуться и
// перетащить его в слот.
// ---------------------------------------------------------------------------
namespace sage::editor::createasset {

enum class What { Folder, Script, Scene, Material, TextFile };
enum class Language { Lua, SageLua };

struct Request {
    What Kind = What::Script;
    std::filesystem::path Dir;     // куда положить; пусто — папка ассетов по виду (DefaultDir)
    std::string Name;              // предложенное имя (без расширения)
    // Что сделать с созданным (назначить в слот, выделить в панели). Зовётся
    // один раз, после успешного создания.
    std::function<void(const std::filesystem::path& created)> Done;
};

void Open(const Request& request);
// Рисует окно, если оно открыто. Зовётся раз за кадр на уровне окна-хоста.
void Draw(EditorHost& host);
bool IsOpen();
// Для самопроверки: ширина окна в прошлом кадре (0 — закрыто) и закрыть его.
float WindowWidth();
void RequestClose();

// Куда по умолчанию кладётся ассет этого вида: assets/scripts, assets/materials,
// assets/Scenes. Папка — от корня ассетов.
inline std::filesystem::path DefaultDir(const std::filesystem::path& assetsDir, What kind) {
    switch (kind) {
        case What::Script: return assetsDir / "scripts";
        case What::Material: return assetsDir / "materials";
        case What::Scene: return assetsDir / "Scenes";
        default: return assetsDir;
    }
}

// Имя файла, который получится: «player» + Lua -> «player.lua». Окно
// показывает его до нажатия «Создать», чтобы не было сюрприза с расширением.
inline std::string FileName(What kind, Language lang, const std::string& name) {
    switch (kind) {
        case What::Folder: return name;
        case What::Scene: return name + ".sage";
        case What::Material: return name + ".sagemat";
        case What::TextFile: return name + ".txt";
        case What::Script:
        default: return name + (lang == Language::SageLua ? ".slua" : ".lua");
    }
}

} // namespace sage::editor::createasset
