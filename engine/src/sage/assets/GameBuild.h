#pragma once
#include <filesystem>
#include <functional>
#include <string>

// ---------------------------------------------------------------------------
// СБОРКА ИГРЫ — одна функция на всех: окно «File > Build Game» редактора (в
// своём потоке, с полосой) и консольный tools/sage_build (CI, выпуск игры без
// редактора). Две копии одной упаковки разошлись бы при первой правке, и
// игра из CI отличалась бы от собранной руками.
//
// Что попадает в игру (одним пакетом game.sagepak рядом с exe):
//   • ВЕСЬ проект — сцены, скрипты, модели, текстуры, звук, материалы,
//     сайдкары импорта (.sageimport); не едут только .meta и папки сборок;
//   • оглавление базы ассетов (GUID -> путь), иначе ссылки сцены по GUID в
//     игре не находились бы;
//   • ресурсы движка (шейдеры, шрифт) из assets/ рядом с плеером.
// Рядом с пакетом — только исполняемый файл, его библиотеки (.dll) и sage.cfg
// (его игрок вправе править сам).
//
// Этапы и ошибки — английскими КЛЮЧАМИ (движок не знает языка интерфейса):
// редактор переводит их своим каталогом, консоль печатает как есть.
// ---------------------------------------------------------------------------
namespace sage::assets {

struct GameBuildRequest {
    std::filesystem::path ProjectDir;
    std::string ProjectName;
    std::filesystem::path OutputDir;      // игра ляжет в OutputDir/ProjectName
    std::filesystem::path Player;         // собранный SagePlayer (его assets/ — рядом)
    std::string AssetIndex;               // AssetDatabase::ExportIndex()
    bool CopyProjectFile = false;         // см. EngineConfig::BuildProjectFile
};

struct GameBuildHooks {
    std::function<void(float)> Progress;                                   // 0..1
    std::function<void(const std::string& key, const std::string& detail)> Stage;
    std::function<bool()> Cancelled;
};

struct GameBuildResult {
    bool Ok = false;
    std::string ErrorKey;      // английский ключ («Player copy failed: »)
    std::string ErrorDetail;   // подробность как есть (путь, текст системы)
    std::filesystem::path GameDir;
    size_t Files = 0;          // файлов в пакете
};

GameBuildResult BuildGame(const GameBuildRequest& request, const GameBuildHooks& hooks = {});

// Оглавление базы ассетов проекта без редактора: просканировать папку и
// выгрузить индекс (для консольной сборки).
std::string ExportProjectAssetIndex(const std::filesystem::path& projectDir);

} // namespace sage::assets
