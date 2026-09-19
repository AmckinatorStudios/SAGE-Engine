#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <string>
#include <unordered_map>
#include <vector>

#include "imgui.h"

#include "AssetPreview.h"
#include "FileBrowser.h"
#include "../RectSelect.h"

class EditorHost;
class Project;

// Панель Assets — браузер файлов проекта: breadcrumb, сетка цветных тайлов по
// типу файла, поиск, создание (New Folder/Script/Text/Material по ПКМ),
// rename/delete. Владеет своим UI-состоянием и своими модалками; наружу
// отдаёт только выбранный файл (Inspector использует его для назначения
// материалов) и операцию CreateAsset (переиспользуется self-test'ом).
class AssetsPanel {
public:
    // Корень панели — assets/ проекта: выше неё панель не поднимается (почему
    // именно так — в AssetsPanel.cpp у самой функции). Открыты ради
    // самопроверки: выход наружу ломается тихо, и увидеть его без проверки
    // можно только глазами.
    static std::filesystem::path AssetsRoot(EditorHost& host);
    static void ClampCwd(EditorHost& host);

    enum class CreateKind { None, Folder, Script, TextFile, Material };

    // ИМЯ ОКНА приходит снаружи: у каждого рабочего пространства оно своё
    // (см. PanelWindowId.h). Панель одна, а окон два — иначе раскладки сцены и
    // вёрстки спорили бы за одно окно, и проигравшая теряла бы панель.
    void Draw(EditorHost& host, bool* open, const std::string& windowId);

    // ДОЛГАЯ РАБОТА ПАНЕЛИ — ПО КАДРАМ, а не одним куском. Зовётся каждый кадр
    // из EditorLayer, независимо от того, открыта ли панель: перевод папки в
    // свои форматы занимает минуты, и сделать его одним вызовом значит
    // повесить редактор наглухо — без кадра, без полосы и без ответа на
    // вопрос, сколько осталось (см. Progress.h).
    void Tick(EditorHost& host);

    // Просит вывести вкладку Assets вперёд. Нужно самопроверке: панель делит
    // док с консолью, и щёлкать по карточке, пока впереди консоль, не по чему.
    void RequestFocus() { m_focusFrames = 3; }

    // Сколько карточек нарисовано в этом кадре, где их середина и ждёт ли
    // ответа вопрос об удалении. Нужно самопроверке: она выбирает файл
    // настоящим щелчком и жмёт настоящий Delete.
    int TileCount() const { return (int)m_tileCenters.size(); }
    ImVec2 TileCenter(int i) const { return m_tileCenters[(size_t)i]; }
    bool DeletePending() const { return !m_deleteTargets.empty(); }

    // Освободить GPU-ресурсы превью, пока контекст жив (см. AssetPreview).
    void Shutdown() { m_preview.Shutdown(); }

    const std::filesystem::path& Selected() const { return m_selected; }
    // ВЕСЬ набор выбранных файлов (включая первичный). Отдельно от Selected():
    // инспектор показывает один файл — тот, на который смотрят, — а удаление и
    // подсветка работают по набору, который обвели рамкой.
    const std::vector<std::filesystem::path>& SelectionSet() const { return m_multi; }
    // Выбрать ассет программно — нужно headless-прогонам и переходу «показать
    // в Assets» из других панелей.
    void Select(const std::filesystem::path& p) { m_selected = p; m_multi = {p}; }

    // Создаёт ассет kind с именем name в папке dir (расширение дописывается).
    // false + err при ошибке. Публично: используется и модалкой, и self-test'ом.
    static bool CreateAsset(CreateKind kind, const std::string& name,
                            const std::filesystem::path& dir,
                            std::filesystem::path& outCreated, std::string& err);

    // Значок папки: ПУСТАЯ и НЕПУСТАЯ выглядят по-разному.
    //
    // Одинаковый значок у всех каталогов означает, что «где мои модели» решается
    // только заходом внутрь: в проекте, разложенном по assets/models, /textures,
    // /sounds, /scenes, /prefabs, половина папок обычно ещё пустая, и отличить
    // их от полных было нечем. Пара «контур/заливка» читается мгновенно и не
    // требует подписи — в отличие от «закрытая/открытая», где открытая означает
    // «в неё вошли», а не «в ней что-то есть».
    //
    // Публично: это поведение, а значит его проверяет самопроверка редактора.
    static const char* FolderIcon(const std::filesystem::path& dir);

    // Переименование и удаление ассета ВМЕСТЕ С САЙДКАРАМИ (.meta, .sageimport).
    //
    // Отдельными функциями, а не пятью строками внутри модалки: сайдкар .meta —
    // это личность ассета (GUID, по которому его находят сцены), и если он
    // отстанет при переименовании, файл станет для проекта новым, а все ссылки
    // на него — битыми. Такое надо проверять тестом, а модалку тест не нажмёт.
    static bool RenameAsset(const std::filesystem::path& path, const std::string& newName,
                            std::filesystem::path& outRenamed, std::string& err);
    static void DeleteAsset(const std::filesystem::path& path);

    // Что получилось при внесении файла в проект.
    struct ImportReport {
        bool Ok = false;
        std::filesystem::path Created;               // главный файл в проекте
        std::vector<std::filesystem::path> Extra;    // спутники (.mtl, .bin, текстуры)
        std::vector<std::string> Missing;            // спутники, которых не нашлось
        std::string Error;
    };

    // Вносит ЧУЖОЙ файл в проект: копирует его в destDir вместе со спутниками и
    // регистрирует в базе ассетов.
    //
    // Отдельная операция, а не копирование файла: Модель почти
    // никогда не один файл: у .obj рядом лежит .mtl, у него — картинки, у
    // .gltf — .bin с геометрией и те же картинки. Скопировать один .obj значит
    // получить в проекте модель, которая грузится без материала и без текстур,
    // причём молча. Здесь спутники разбираются и переезжают вместе с ним, а те,
    // что не нашлись, попадают в отчёт — «чего-то не хватает» человек должен
    // узнать сейчас, а не по виду модели в сцене.
    //
    // Публично: используется и панелью, и self-test'ом редактора.
    static ImportReport ImportAsset(const std::filesystem::path& source,
                                    const std::filesystem::path& destDir);

    // ТО ЖЕ, НО ДВУМЯ ПОЛОВИНАМИ — потому что внесение идёт В ФОНОВОМ ПОТОКЕ.
    //
    // Копирование — работа файловой системы, и её можно (и нужно) вести не в
    // кадре: папка ассетов с маркетплейса — это сотни файлов и сотни мегабайт,
    // а всё это время редактор стоял бы без единого кадра, то есть выглядел бы
    // повисшим. База ассетов при этом остаётся ЗА ГЛАВНЫМ ПОТОКОМ: её читает
    // каждый кадр панель, и писать в неё из фона значит гонку.
    //
    // tell — куда сообщать ход дела: доля (отрицательная — неизвестна) и имя
    // текущего файла. Зовётся из рабочего потока.
    static ImportReport CopyIntoProject(
        const std::filesystem::path& source, const std::filesystem::path& destDir,
        const std::function<void(float, const std::string&)>& tell = {});
    static void RegisterImported(const ImportReport& report);

    // Смена проекта: отпустить обложки прошлого. Ключ обложки — путь ассета,
    // а он в каждом проекте свой и ведёт к другому файлу.
    void ForgetProject() { m_preview.ForgetProject(); }

private:
    // Превью карточек. Материалы, префабы и модели рендерятся по одному за кадр
    // и запоминаются (см. ThumbnailFor): один такой рендер — полный проход
    // сцены со светом.
    uint64_t ThumbnailFor(EditorHost& host, const std::filesystem::path& path, bool isDir);
    AssetPreview m_preview;
    struct Thumb {
        uint64_t Id = 0;      // 0 — превью не получилось (негативный кэш)
        long long Stamp = 0;  // время правки файла на момент съёмки
    };
    // Ключ — путь к файлу. Штамп времени в значении: правка материала или
    // пересохранение префаба обязаны обновить обложку, иначе в панели остаётся
    // картинка того, чего в файле уже нет.
    std::unordered_map<std::string, Thumb> m_thumbs;
    bool m_thumbRenderedThisFrame = false;

    // Конвертация в свои форматы движка (sage/assets/import/Convert.h).
    void ConvertOne(EditorHost& host, const std::filesystem::path& path);
    void ConvertFolderHere(EditorHost& host);

    // Фоновое внесение файлов в проект: поток, его результат и карточка.
    std::future<ImportReport> m_import;
    uint64_t m_importTask = 0;

    // Очередь пакетной конвертации: что осталось перевести и что уже вышло.
    // Файлы разбираются по нескольку за кадр (см. Tick) — кадр при этом живой,
    // а модальное окно показывает, где счёт.
    std::vector<std::filesystem::path> m_convertQueue;
    size_t m_convertAt = 0;
    size_t m_convertOk = 0, m_convertFailed = 0;
    size_t m_convertSrcBytes = 0, m_convertOutBytes = 0;
    uint64_t m_convertTask = 0;

    void DrawBreadcrumb(EditorHost& host);
    // Дерево папок проекта слева. Сетка отвечает на вопрос «что лежит здесь», а
    // дерево — на вопрос «что вообще есть в проекте»: без него единственным
    // способом узнать состав было ходить по папкам вверх-вниз, каждый раз теряя
    // из виду то, откуда пришёл.
    void DrawFolderTree(EditorHost& host);
    // Строки дерева папок этого кадра — по ним рисуются линии связи (см.
    // DrawFolderNode): у встроенных в ImGui горизонталь обрывается далеко от
    // значка.
    struct TreeRow {
        float Y = 0.0f;
        float IconX = 0.0f;
        int Depth = 0;
    };
    std::vector<TreeRow> m_treeRows;
    void DrawFolderNode(EditorHost& host, const std::filesystem::path& dir, int depth);
    void DrawTile(EditorHost& host, const std::filesystem::path& path, bool isDir);
    void DrawModals(EditorHost& host); // Create/Rename/Delete — в ID-скоупе окна панели

    // Перенос файла в папку броском (вместе с сайдкарами .meta/.sageimport).
    void MoveIntoFolder(EditorHost& host, const std::filesystem::path& source,
                        const std::filesystem::path& folder);

    // Внесение чужих файлов в проект: диалог + отчёт в статусную строку.
    void DrawImportButton(EditorHost& host);
    void FinishImport(EditorHost& host);
    FileBrowser m_importBrowser;

    char m_search[128] = "";
    // Ширина дерева. Правится перетаскиванием разделителя и живёт до
    // перезапуска: у кого-то имена папок длинные, у кого-то экран узкий.
    float m_treeWidth = 190.0f;
    std::filesystem::path m_selected;      // первичный выделенный тайл (под инспектор)
    std::vector<std::filesystem::path> m_multi; // весь набор: рамка, Ctrl-клик
    // ЯКОРЬ ДИАПАЗОНА и порядок карточек НА ЭКРАНЕ — для выбора с Shift «от и
    // до». Диапазон считается по тому порядку, который человек ВИДИТ (папки,
    // потом файлы, с учётом поиска), а не по порядку файлов на диске: выделяя
    // от одного до другого, смотрят на сетку.
    std::filesystem::path m_anchor;
    std::vector<std::filesystem::path> m_visibleOrder;
    std::vector<ImVec2> m_tileCenters;   // где карточки на экране (для самопроверки)
    bool m_deleteAsked = false;          // вопрос об удалении уже показывали
    int m_focusFrames = 0;               // >0 — просим вывести вкладку вперёд
    sage::editor::rectselect::State m_rect;     // рамка выделения (см. RectSelect.h)
    std::vector<std::filesystem::path> m_rectHits; // кого рамка задела в этом кадре
    bool m_rectActive = false;
    std::filesystem::path m_renameTarget;  // пусто — модалка Rename не активна
    char m_renameBuf[256] = "";
    std::vector<std::filesystem::path> m_deleteTargets; // ждут подтверждения в модалке Delete
    CreateKind m_createKind = CreateKind::None;
    char m_createName[128] = "";
    std::string m_error; // ошибка текущей модалки
};
