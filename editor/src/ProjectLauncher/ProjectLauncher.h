#pragma once
#include <string>
#include <vector>

#include "../FileBrowser.h"
#include "ProjectCard.h"
#include "ProjectDatabase.h"
#include "ProjectThumbnail.h"

class EditorHost;

// ---------------------------------------------------------------------------
// СТАРТОВОЕ ОКНО ПРОЕКТОВ — самостоятельный экран, а не панель редактора.
//
// Первое, что видит человек, запустив SAGE, — список его проектов. Задача
// одна: открыть нужный проект или создать новый. Поэтому экран один, без
// разделов, фильтров и панели подробностей: шапка (поиск, «Открыть», «Новый
// проект») и список по времени последнего открытия.
//
// Модуль знает о редакторе ровно столько, сколько объявлено в EditorHost:
// создать проект, открыть проект. EditorLayer вызывает Draw() и не знает, что
// внутри.
//
// ОТКУДА БЕРЁТСЯ СПИСОК. Из базы стартового окна (ProjectDatabase): проекты,
// которые человек открывал, создавал или принёс сам. Диск не сканируется.
//
// ЧЕГО ЗДЕСЬ НЕТ. Работы без проекта: окно закрывается только созданием или
// открытием проекта (половина редактора без проекта не работает по
// определению).
// ---------------------------------------------------------------------------
namespace Sage::Launcher {

class ProjectLauncher {
public:
    // Рисует окно на весь главный вьюпорт. Возвращает false, когда человек
    // попросил его закрыть (кнопка «назад» — она есть только тогда, когда
    // проект уже открыт и закрывать окно есть куда).
    bool Draw(EditorHost& host, ProjectDatabase& db);

    // Отпустить обложки, ПОКА ГРАФИЧЕСКИЙ КОНТЕКСТ ЖИВ (см. ProjectThumbnail).
    void Shutdown() { m_thumbs.Shutdown(); }

    // Файлы, брошенные в окно из проводника. Папка проекта добавляется в
    // список; всё остальное — понятный отказ, а не молчание.
    //
    // Возвращает число принятых проектов. Отдельным методом (а не «внутри
    // Draw»), потому что перетаскивание приходит от окна, а не от кадра, и
    // потому что так его можно проверить без интерфейса.
    int AcceptDroppedFiles(const std::vector<std::string>& paths, ProjectDatabase& db);

    // Последняя ошибка окна — пусто, если всё в порядке. Нужна самопроверке:
    // «показали ли мы отказ» иначе проверяется только глазами.
    const std::string& LastError() const { return m_error; }
    const std::string& LastStatus() const { return m_status; }

private:
    void DrawTopBar(EditorHost& host, ProjectDatabase& db, bool canClose, bool& closeRequested);
    void DrawList(EditorHost& host, ProjectDatabase& db);
    void DrawEmpty(bool searching);
    void DrawDialogs(EditorHost& host, ProjectDatabase& db);
    void HandleShortcuts(EditorHost& host, ProjectDatabase& db,
                         const std::vector<int>& shown);

    // Открыть проект (двойной клик, Enter, кнопка). Ошибку кладёт в m_error —
    // ранние выходы из кадра здесь запрещены: мы внутри окна.
    void OpenProject(EditorHost& host, ProjectDatabase& db, const std::string& path);
    void RunAction(EditorHost& host, ProjectDatabase& db, ProjectAction action,
                   const std::string& path);
    void StartCreate();
    void StartOpenBrowser();
    // Что мешает создать проект (пусто — можно). Считается каждый кадр: человек
    // должен видеть препятствие, а не натыкаться на него после нажатия.
    std::string CreateBlockedReason() const;

    void LoadPrefs();
    void SavePrefs() const;

    // --- состояние экрана ---
    char m_search[128] = {0};
    bool m_focusSearch = false;
    std::string m_selected;     // путь выбранного проекта
    std::string m_error;
    std::string m_status;
    bool m_prefsLoaded = false;
    float m_colW = 0.0f;        // ширина колонки списка в этом кадре

    // Контекстное меню строки: цель и просьба открыть (OpenPopup можно звать
    // только в кадре, а просьба приходит из строки).
    std::string m_menuTarget;
    bool m_menuRequested = false;

    // --- модальные диалоги окна ---
    enum class Dialog { None, Create, Rename, Duplicate, Delete };
    Dialog m_dialog = Dialog::None;
    bool m_dialogOpening = false;
    bool m_focusName = false;        // курсор в поле имени при открытии «Нового проекта»
    std::string m_dialogTarget;      // над каким проектом
    char m_nameBuf[128] = "My Game";
    char m_dirBuf[512] = {0};

    // Файловый диалог общий на все поля окна: открыт он может быть только
    // один, а куда положить ответ — помнит указатель.
    FileBrowser m_browser;
    char* m_browseTarget = nullptr;
    size_t m_browseTargetSize = 0;
    bool m_browseOpen = false;       // выбирали проект для открытия

    ProjectThumbnail m_thumbs;
};

} // namespace Sage::Launcher
