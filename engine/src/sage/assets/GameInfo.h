#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

// ---------------------------------------------------------------------------
// СВЕДЕНИЯ ОБ ИГРЕ — то, чем собранная игра представляется игроку и системе:
// название (окно, имя exe), описание, автор, издатель, версия, копирайт и
// иконка. Хранятся в project.sageproj, в разделе "game" (рядом с прежними
// "name" и "startScene", которые не трогаются):
//
//   "game": {"name": "SAGE Craft", "description": "...", "author": "...",
//            "company": "...", "version": "1.0.0", "copyright": "...",
//            "icon": "assets/icon.png"}
//
// Читают их сборка (имя exe, иконка и сведения о файле в .exe на Windows),
// плеер (заголовок и иконка окна) и окно «Настройки игры» редактора. Пустые
// поля — честно пустые: имя по умолчанию — имя проекта, версия — 1.0.0.
// ---------------------------------------------------------------------------
namespace sage::assets {

struct GameInfo {
    std::string Name;
    std::string Description;
    std::string Author;
    std::string Company;
    std::string Version = "1.0.0";
    std::string Copyright;
    std::string Icon;            // путь картинки в проекте (PNG, лучше 256×256 и больше)

    // Из всего project.sageproj (раздел "game"; без него — имя проекта).
    static GameInfo FromProject(const nlohmann::json& project);
    // Записать раздел "game" в project.sageproj (остальные поля не трогаются).
    void WriteTo(nlohmann::json& project) const;

    // Имя файла игры: название без символов, которых не любит файловая
    // система (\ / : * ? " < > |), без точек и пробелов по краям.
    std::string FileName() const;
    // Версия числами для сведений о файле: «1.2.3» -> {1, 2, 3, 0}.
    void VersionNumbers(uint16_t out[4]) const;
};

// project.sageproj папки проекта (или из смонтированного пакета игры).
bool LoadGameInfo(const std::filesystem::path& projectDir, GameInfo& out);
// Прочитать-поправить-записать: чужие поля файла проекта сохраняются.
bool SaveGameInfo(const std::filesystem::path& projectDir, const GameInfo& info, std::string* error = nullptr);

// Картинка иконки в RGBA8. Квадрат любого размера; нужные размеры
// получаются уменьшением (IconSizes).
struct IconImage {
    int Width = 0, Height = 0;
    std::vector<uint8_t> Rgba;
};
// (Не LoadIcon: так называется макрос windows.h, и имя подменилось бы.)
// (Не LoadIcon: так называется макрос windows.h, и имя функции подменялось.)
bool LoadIconImage(const std::string& pathOrProjectRef, IconImage& out);
// Уменьшенная копия size×size (усреднение по площади: края пикселя-арта не
// плывут в мыло, как у билинейной выборки).
IconImage ResizeIcon(const IconImage& src, int size);

// Ресурсы Windows (байты — для UpdateResource; сами по себе кроссплатформенны
// и проверяются тестами на любой системе).
//   RT_ICON — PNG каждого размера, RT_GROUP_ICON — оглавление этих картинок.
struct IconResources {
    std::vector<std::vector<uint8_t>> Images;    // RT_ICON, номера FirstId, FirstId+1, ...
    std::vector<uint8_t> Group;                  // RT_GROUP_ICON
};
IconResources BuildIconResources(const IconImage& icon, uint16_t firstId = 1);
// RT_VERSION (VS_VERSIONINFO): то, что Проводник показывает в «Свойствах» exe.
std::vector<uint8_t> BuildVersionResource(const GameInfo& info, const std::string& exeFileName);

// Вшить иконку и сведения о файле в исполняемый файл Windows. Работает только
// на Windows (UpdateResource); на других системах возвращает false с причиной.
bool EmbedWindowsResources(const std::filesystem::path& exe, const GameInfo& info, const IconImage* icon,
                           std::string& error);

} // namespace sage::assets
