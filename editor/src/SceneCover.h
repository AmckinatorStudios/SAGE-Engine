#pragma once
#include <filesystem>
#include <string>
#include <system_error>

// ---------------------------------------------------------------------------
// ОБЛОЖКА СЦЕНЫ — снимок того, как сцена выглядит.
//
// ЗАЧЕМ. У шаблона обложка была (assets/templates/<id>.png), а у самой сцены —
// нет: в панели ассетов файл .sage выглядел значком, одинаковым у всех, а в
// стартовом окне проект показывался рисунком, собранным из его ИМЕНИ. То есть
// ровно там, где человек выбирает, куда вернуться, картинки его работы не
// было — ни у проекта, ни у сцены. Узнать «какая из трёх это та самая» можно
// было только открыв каждую.
//
// ГДЕ ЛЕЖИТ. В служебной папке проекта .sage/covers/<имя сцены>.png. Не рядом
// со сценой в scenes/ — иначе рядом с каждой сценой появляется PNG, который
// человек видит, не понимает и однажды удалит (а обложку он не заводил).
// .sage/ — это кэш редактора: его можно снести целиком, и ничего, кроме
// картинок, не потеряется.
//
// И ОДНА ОБЩАЯ — .sage/thumbnail.png: её показывает стартовое окно (см.
// ProjectDatabase::FindThumbnail, путь в его списке уже был). Это обложка
// ПОСЛЕДНЕЙ сохранённой сцены, и это честно: проект в списке узнают по тому,
// над чем в нём работали в последний раз, а не по алфавитно первой сцене.
// ---------------------------------------------------------------------------
namespace scenecover {

// Папка обложек проекта. Пусто — проекта нет (демо-сцена редактора), и класть
// обложку некуда.
inline std::filesystem::path Dir(const std::filesystem::path& projectDir) {
    if (projectDir.empty()) return {};
    return projectDir / ".sage" / "covers";
}

// Обложка КОНКРЕТНОЙ сцены. Имя — по имени файла сцены: две сцены с одним
// именем в разных папках проекта — случай редкий, а путь в имени файла
// обложки означал бы имена вида scenes_levels_main.png.
inline std::filesystem::path For(const std::filesystem::path& projectDir,
                                 const std::filesystem::path& scenePath) {
    if (projectDir.empty() || scenePath.empty()) return {};
    return Dir(projectDir) / (scenePath.stem().string() + ".png");
}

// Обложка ПРОЕКТА для стартового окна.
inline std::filesystem::path ProjectShot(const std::filesystem::path& projectDir) {
    if (projectDir.empty()) return {};
    return projectDir / ".sage" / "thumbnail.png";
}

// Есть ли обложка у этой сцены. Отдельной функцией, потому что спрашивают её
// из панели ассетов на каждый файл в папке, и там уместно ровно «есть или нет».
inline bool Exists(const std::filesystem::path& projectDir,
                   const std::filesystem::path& scenePath) {
    const std::filesystem::path p = For(projectDir, scenePath);
    if (p.empty()) return false;
    std::error_code ec;
    return std::filesystem::exists(p, ec) && std::filesystem::is_regular_file(p, ec);
}

} // namespace scenecover
