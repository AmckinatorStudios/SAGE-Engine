#pragma once
#include <filesystem>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Файловый диалог редактора.
//
// ЗАЧЕМ. До этого КАЖДЫЙ путь в редакторе вводился руками в текстовое поле:
// открыть проект, сохранить сцену, собрать игру, выбрать модель, выбрать
// текстуру. Это не неудобство, а неработоспособность: человек не помнит
// абсолютный путь к своей модели, а ошибиться в нём на один символ — обычное
// дело, и единственной обратной связью была строчка в консоли.
//
// ПОЧЕМУ СВОЙ, А НЕ СИСТЕМНЫЙ. Системный диалог (GetOpenFileName / GTK / Cocoa)
// выглядит роднее, но тянет за собой по реализации на платформу, на Linux ещё и
// зависимость от того, какой тулкит стоит у пользователя, а в headless-прогоне
// (CI, автотесты редактора) не открывается вовсе. Диалог на ImGui рисуется тем
// же кодом на всех платформах, работает в offscreen-прогоне и не может
// заблокировать поток ожиданием ответа оконной системы.
//
// Использование:
//     FileBrowser::Config cfg;
//     cfg.Title = "Открыть проект";
//     cfg.Mode  = FileBrowser::PickMode::OpenFile;
//     cfg.Filters = {".sageproj"};
//     browser.Open(cfg);
//     ...
//     if (browser.Draw()) { /* browser.Result() — выбранный путь */ }
// ---------------------------------------------------------------------------
class FileBrowser {
public:
    enum class PickMode {
        OpenFile,    // выбрать существующий файл
        SaveFile,    // указать имя (файла может ещё не быть)
        PickFolder,  // выбрать каталог
    };

    struct Config {
        std::string Title = "Выбор файла";
        PickMode Mode = PickMode::OpenFile;
        std::filesystem::path StartDir;      // пусто — текущий каталог
        std::string DefaultName;             // для SaveFile
        // Расширения с точкой (".sage", ".png"). Пусто — показывать все файлы.
        std::vector<std::string> Filters;
        std::string FilterLabel;             // подпись фильтра, напр. "Сцены (*.sage)"
    };

    void Open(const Config& config);
    bool IsOpen() const { return m_open; }

    // Рисует диалог. true — пользователь подтвердил выбор; путь в Result().
    bool Draw();

    const std::filesystem::path& Result() const { return m_result; }

private:
    struct Entry {
        std::string Name;
        bool IsDir = false;
        uintmax_t Size = 0;
    };

    void Refresh();
    void GoTo(const std::filesystem::path& dir);
    bool PassesFilter(const std::filesystem::path& p) const;
    void DrawPlaces();
    void DrawBreadcrumbs();

    Config m_cfg;
    bool m_open = false;
    bool m_needsOpen = false;   // отложенный OpenPopup: звать его можно только в кадре
    std::filesystem::path m_dir;
    std::filesystem::path m_result;
    std::vector<Entry> m_entries;
    std::string m_error;
    char m_name[512] = {0};
    char m_search[128] = {0};
    char m_newFolder[128] = {0};
    int m_selected = -1;
    bool m_showHidden = false;
    // Куда можно прыгнуть одним кликом. Считается при открытии: перечитывать
    // список дисков каждый кадр незачем, а на Windows это ещё и обращение к
    // диску, которое на пустом приводе даёт заметную паузу.
    // Пункт быстрого доступа. Заголовок группы — тот же тип с пустым путём:
    // отдельный список групп пришлось бы держать в согласии с этим, а порядок
    // всё равно один.
    struct Place {
        std::string Label;
        std::filesystem::path Path;
        bool IsGroup = false;
    };
    std::vector<Place> m_places;
};
