#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>

#include "AssetPreview.h"

class EditorHost;

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

private:
    // Превью карточек. Материалы рендерятся по одному за кадр и запоминаются
    // (см. ThumbnailFor): один такой рендер — полный проход сцены со светом.
    uint64_t ThumbnailFor(const std::filesystem::path& path, bool isDir);
    AssetPreview m_preview;
    std::unordered_map<std::string, uint64_t> m_matThumbs;
    bool m_thumbRenderedThisFrame = false;

    // Конвертация в свои форматы движка (sage/assets/import/Convert.h).
    void ConvertOne(EditorHost& host, const std::filesystem::path& path);
    void ConvertFolderHere(EditorHost& host);

    void DrawBreadcrumb(EditorHost& host);
    void DrawTile(EditorHost& host, const std::filesystem::path& path, bool isDir);
    void DrawModals(EditorHost& host); // Create/Rename/Delete — в ID-скоупе окна панели

    char m_search[128] = "";
    std::filesystem::path m_selected;      // выделенный тайл
    std::filesystem::path m_renameTarget;  // пусто — модалка Rename не активна
    char m_renameBuf[256] = "";
    std::filesystem::path m_deleteTarget;  // ждёт подтверждения в модалке Delete
    CreateKind m_createKind = CreateKind::None;
    char m_createName[128] = "";
    std::string m_error; // ошибка текущей модалки
};
