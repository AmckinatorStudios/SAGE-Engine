#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "AssetPreview.h"
#include "FileBrowser.h"

class EditorHost;
class Project;

// Панель Assets — браузер файлов проекта: breadcrumb, сетка цветных тайлов по
// типу файла, поиск, создание (New Folder/Script/Text/Material по ПКМ),
// rename/delete. Владеет своим UI-состоянием и своими модалками; наружу
// отдаёт только выбранный файл (Inspector использует его для назначения
// материалов) и операцию CreateAsset (переиспользуется self-test'ом).
class AssetsPanel {
public:
    enum class CreateKind { None, Folder, Script, TextFile, Material };

    void Draw(EditorHost& host);

    // Освободить GPU-ресурсы превью, пока контекст жив (см. AssetPreview).
    void Shutdown() { m_preview.Shutdown(); }

    const std::filesystem::path& Selected() const { return m_selected; }
    // Выбрать ассет программно — нужно headless-прогонам и переходу «показать
    // в Assets» из других панелей.
    void Select(const std::filesystem::path& p) { m_selected = p; }

    // Создаёт ассет kind с именем name в папке dir (расширение дописывается).
    // false + err при ошибке. Публично: используется и модалкой, и self-test'ом.
    static bool CreateAsset(CreateKind kind, const std::string& name,
                            const std::filesystem::path& dir,
                            std::filesystem::path& outCreated, std::string& err);

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
    // ЗАЧЕМ ЭТО ОТДЕЛЬНАЯ ОПЕРАЦИЯ, А НЕ «просто скопируй файл». Модель почти
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

private:
    // Превью карточек. Материалы, префабы и модели рендерятся по одному за кадр
    // и запоминаются (см. ThumbnailFor): один такой рендер — полный проход
    // сцены со светом.
    uint64_t ThumbnailFor(const std::filesystem::path& path, bool isDir);
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

    void DrawBreadcrumb(EditorHost& host);
    void DrawTile(EditorHost& host, const std::filesystem::path& path, bool isDir);
    void DrawModals(EditorHost& host); // Create/Rename/Delete — в ID-скоупе окна панели

    // Перенос файла в папку броском (вместе с сайдкарами .meta/.sageimport).
    void MoveIntoFolder(EditorHost& host, const std::filesystem::path& source,
                        const std::filesystem::path& folder);

    // Внесение чужих файлов в проект: диалог + отчёт в статусную строку.
    void DrawImportButton(EditorHost& host);
    FileBrowser m_importBrowser;

    char m_search[128] = "";
    std::filesystem::path m_selected;      // выделенный тайл
    std::filesystem::path m_renameTarget;  // пусто — модалка Rename не активна
    char m_renameBuf[256] = "";
    std::filesystem::path m_deleteTarget;  // ждёт подтверждения в модалке Delete
    CreateKind m_createKind = CreateKind::None;
    char m_createName[128] = "";
    std::string m_error; // ошибка текущей модалки
};
