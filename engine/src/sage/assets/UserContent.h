#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "sage/assets/GameInfo.h"

// ---------------------------------------------------------------------------
// ПОЛЬЗОВАТЕЛЬСКИЕ ПАПКИ ИГРЫ — открытые папки для своих файлов игрока:
// скины, музыка, карты, моды. Всё прочее в собранной игре лежит в
// зашифрованном пакете, и положить туда своё нельзя; эти папки — нет.
//
// Игра объявляет папки в project.sageproj (GameInfo::UserFolders, окно
// «Настройки игры»). Движок:
//   * при СБОРКЕ создаёт папки location = "game" рядом с игрой, кладёт в
//     каждую README.txt (что сюда класть и какие файлы читаются) и примеры из
//     папки проекта userfolders/<имя>/ — в пакет эти примеры не идут;
//   * при ЗАПУСКЕ находит папку (рядом с игрой, а если там нельзя писать или
//     игра запущена не из сборки — в папке данных игры, рядом с saves),
//     создаёт её при первом обращении (с README и примерами);
//   * отдаёт скриптам список файлов — только нужных расширений, по имени — и
//     открывает папку в проводнике.
// Чужие имена (не объявленные в проекте) не открываются: скрипт не может
// читать произвольные места диска через этот вход.
// ---------------------------------------------------------------------------
namespace sage::assets::usercontent {

struct Entry {
    std::string Name;        // имя файла с расширением
    std::string Path;        // полный путь (UTF-8) — для texture.load, PlayMusic, …
    std::string Relative;    // путь внутри папки (с подпапками, через /)
    uint64_t Size = 0;
    long long Modified = 0;  // время изменения, секунды Unix
};

// Что объявила игра и где она запущена: builtGameDir — папка собранной игры
// (рядом с пакетом), пустая — запуск из проекта; projectDir — папка проекта
// (примеры для папок в разработке берутся из <проект>/userfolders/<имя>).
void Configure(const std::vector<UserFolderSpec>& folders, const std::filesystem::path& builtGameDir,
               const std::filesystem::path& projectDir);

const std::vector<UserFolderSpec>& Folders();
const UserFolderSpec* Find(const std::string& name);

// Папка name: создаётся при первом обращении (README, примеры). Пустой путь —
// такой папки игра не объявляла.
std::filesystem::path Resolve(const std::string& name);

// Файлы папки (с подпапками, если recursive), отфильтрованные по расширениям,
// по имени. Скрытые файлы и README.txt движка не попадают.
std::vector<Entry> List(const std::string& name, bool recursive = true);

// Отпечаток содержимого (имена, размеры, время): сменился — игра перечитывает.
std::string Signature(const std::string& name);

// Открыть папку в проводнике / Finder / файловом менеджере.
bool OpenInSystem(const std::string& name);

// Текст README.txt папки.
std::string Readme(const UserFolderSpec& spec);

// Сборка: папки location = "game" в gameDir с README и примерами из
// <projectDir>/userfolders/<имя>. Возвращает число созданных папок.
size_t PrepareForBuild(const std::filesystem::path& gameDir, const std::filesystem::path& projectDir,
                       const std::vector<UserFolderSpec>& folders);

// Имя папки допустимо (латиница, цифры, «-», «_», не пустое, без точек):
// оно же — каталог на диске и ключ скрипта.
bool ValidName(const std::string& name);

} // namespace sage::assets::usercontent
