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
// ЧТО ЭТО ЗА МОДУЛЬ И ПОЧЕМУ ОН ОТДЕЛЬНЫЙ. Первое, что видит человек, запустив
// SAGE, — не редактор, а список своих проектов. Это отдельный продукт со своей
// задачей: найти нужный проект среди сотни, посмотреть, что в нём, создать
// новый, принести чужой. Ни вьюпорта, ни иерархии, ни инспектора здесь нет и
// быть не должно — от редактора этому экрану нужен ровно один переход:
//
//      Стартовое окно  ->  «Открыть проект»  ->  EditorHost  ->  редактор
//
// Поэтому модуль лежит в своей папке и знает о редакторе ровно столько,
// сколько объявлено в EditorHost: создать проект, открыть проект. Обратной
// зависимости нет: EditorLayer вызывает Draw() и не знает, что внутри.
//
// ЧТО БЫЛО ДО ЭТОГО. Одно окно на два столбца: слева список недавних строками,
// справа две формы — «создать» и «открыть». Оно решало ту же задачу, пока
// проектов было три: список без поиска, без отбора, без сортировки, без
// сведений о проекте (кроме пути) и без единой картинки. На двадцати проектах
// такой список превращается в поиск глазами по столбцу путей, а на сотне —
// перестаёт работать вовсе.
//
// ОТКУДА БЕРЁТСЯ СПИСОК. Из базы стартового окна (ProjectDatabase): проекты,
// которые человек открывал, создавал или принёс сам. Диск не сканируется —
// см. ProjectDatabase.h.
//
// ЧЕГО ЗДЕСЬ НЕТ. Работы без проекта. Окно закрывается только созданием или
// открытием проекта: половина редактора без проекта не работает по
// определению (пути ассетов, сборка игры, шаблоны), и «облегчённый режим»
// означал бы оговорку «а если проекта нет» в каждой новой возможности.
// ---------------------------------------------------------------------------
namespace Sage::Launcher {

// Разделы левой навигации. Расширяется без правки редактора — ровно ради этого
// стартовое окно и вынесено в свой модуль (Templates, Samples, Marketplace,
// Cloud Projects — это новый пункт здесь, а не новая ветка в EditorLayer).
enum class LauncherSection { Projects, Recent, Templates, Settings, Count };

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
    void DrawTopBar(bool canClose, bool& closeRequested);
    void DrawSidebar();
    void DrawBrowser(EditorHost& host, ProjectDatabase& db);
    void DrawTemplates();
    void DrawSettings(ProjectDatabase& db);
    void DrawDetails(EditorHost& host, ProjectDatabase& db);
    void DrawDialogs(EditorHost& host, ProjectDatabase& db);
    void HandleShortcuts(EditorHost& host, ProjectDatabase& db,
                         const std::vector<int>& shown);

    // Открыть проект (двойной клик, Enter, кнопка). Ошибку кладёт в m_error —
    // ранние выходы из кадра здесь запрещены: мы внутри двух окон.
    void OpenProject(EditorHost& host, ProjectDatabase& db, const std::string& path);
    void RunAction(EditorHost& host, ProjectDatabase& db, ProjectAction action,
                   const std::string& path);
    // Что мешает создать проект (пусто — можно). Считается каждый кадр: человек
    // должен видеть препятствие, а не натыкаться на него после нажатия.
    std::string CreateBlockedReason() const;

    void LoadPrefs();
    void SavePrefs() const;

    // --- состояние экрана ---
    LauncherSection m_section = LauncherSection::Projects;
    ProjectFilter m_filter = ProjectFilter::All;
    ProjectSort m_sort = ProjectSort::Recent;
    CardLayout m_layout = CardLayout::Grid;
    char m_search[128] = {0};
    bool m_focusSearch = false;
    std::string m_selected;     // путь выбранного проекта
    std::string m_error;
    std::string m_status;
    bool m_prefsLoaded = false;

    // Контекстное меню карточки: цель и просьба открыть (OpenPopup можно звать
    // только в кадре, а просьба приходит из карточки).
    std::string m_menuTarget;
    bool m_menuRequested = false;

    // --- модальные диалоги окна ---
    enum class Dialog { None, Create, Rename, Duplicate, Delete };
    Dialog m_dialog = Dialog::None;
    bool m_dialogOpening = false;
    std::string m_dialogTarget;      // над каким проектом
    char m_nameBuf[128] = "MyGame";
    char m_dirBuf[512] = {0};
    char m_descBuf[256] = {0};
    std::string m_templateId;
    ProjectKind m_newKind = ProjectKind::Game;

    // Файловый диалог общий на все поля окна: открыт он может быть только
    // один, а куда положить ответ — помнит указатель.
    FileBrowser m_browser;
    char* m_browseTarget = nullptr;
    size_t m_browseTargetSize = 0;
    bool m_browseImport = false;     // выбирали проект для импорта

    ProjectThumbnail m_thumbs;
};

} // namespace Sage::Launcher
