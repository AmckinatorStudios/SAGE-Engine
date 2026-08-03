#pragma once
#include <filesystem>
#include <memory>
#include <string>

#include "AssetPreview.h"
#include "FileBrowser.h"


class EditorHost;
class GameObject;
class Texture;
struct MeshRendererComponent;

// Панель Inspector — свойства выбранной сущности (имя/Transform/MeshRenderer/
// Material/компоненты с Add/Remove) и редактор выбранного в Assets файла
// (.sagemat правится живьём в разделяемом экземпляре, Save пишет на диск).
//
// Это ДВА РАЗНЫХ ПРЕДМЕТА ПРАВКИ, и панель держит их в разных вкладках. Раньше
// они шли одной простынёй, разделённые чертой: сверху материал из Assets, снизу
// свойства сущности. Читалось как один список свойств одного объекта — при том
// что область действия у них разная (файл материала общий для всех, кто им
// покрашен, а Transform принадлежит одной сущности), и человек не понимал, что
// именно он сейчас меняет.
class InspectorPanel {
public:
    void Draw(EditorHost& host);

    // Освободить GPU-ресурсы превью, пока контекст жив (см. AssetPreview).
    void Shutdown() { m_preview.Shutdown(); }

private:
    // Что за файл выбран в Assets — по нему выбирается редактор.
    enum class AssetKind { None, Material, Prefab, Model, Other };
    // Какая вкладка открыта. Следует за последним выбором человека, иначе
    // разделение стоило бы лишнего клика на каждое переключение.
    enum class Focus { Object, Asset };

    static AssetKind ClassifyAsset(const std::filesystem::path& path);
    static void DrawSectionHeader(const char* icon, const char* kind, const std::string& name,
                                  const std::string& subtitle);
    void DrawObjectSection(EditorHost& host);
    void DrawAssetSection(EditorHost& host, AssetKind kind);

    Focus m_focus = Focus::Object;
    // Одноразовый: переключить вкладку СЕЙЧАС. Постоянный SetSelected залипает.
    bool m_forceFocus = false;
    int m_lastEntityId = -1;
    std::string m_lastAssetPath;

    void DrawMaterialEditor(EditorHost& host);
    // Префаб: вращаемая 3D-обложка + постановка в сцену (см. AssetPreview::RenderPrefab).
    void DrawPrefabPreview(EditorHost& host);

    // Слот текстуры: превью, путь, «Обзор…», «Из Assets», «Очистить».
    //
    // Раньше на месте слота стояло голое поле ввода, применявшееся по Enter, —
    // то есть путь к текстуре надо было ЗНАТЬ и напечатать без опечатки, а
    // единственной обратной связью была строчка в консоли. Слот показывает саму
    // картинку: назначено ли что-то и то ли это, что хотели, видно сразу.
    void DrawTextureSlot(EditorHost& host, const char* label, std::string& path,
                         const std::shared_ptr<Texture>& tex, const char* tooltip);

    AssetPreview m_preview;
    FileBrowser m_browser;
    // Куда положить результат обзора. Указатель на строку материала: сам
    // материал живёт в кэше ресурсов и переживает кадры, а диалог отвечает
    // через кадр.
    std::string* m_browseTarget = nullptr;
    bool m_browseIsShader = false;   // после выбора шейдера нужен ShaderPtr.reset()
    bool m_browseIsMesh = false;     // после выбора модели её надо загрузить
    bool m_browseIsMaterial = false; // после выбора материала его надо подгрузить
    bool m_pendingMeshLoad = false;  // загрузку делаем в кадре, а не из колбэка диалога
    void DrawModelImportEditor(EditorHost& host); // настройки импорта выбранной модели

    // Mesh Renderer одной секцией, в порядке самой структуры компонента:
    // ЧТО рисуем -> ЧЕМ красим -> чем ЭТОТ экземпляр отличается от других.
    // Раньше первое и третье лежали вперемешку под заголовком «Mesh Renderer»,
    // а второе — отдельным заголовком «Material», как будто это другой
    // компонент; см. комментарий на месте вызова.
    void DrawMeshSlot(EditorHost& host, MeshRendererComponent& mr);
    void DrawMaterialSlot(EditorHost& host, MeshRendererComponent& mr);
    void DrawInstanceOverrides(EditorHost& host, MeshRendererComponent& mr);
    // Материал модели — вместе с моделью: .sagemat рядом с файлом модели, если
    // его ещё нет, и назначение его сущности.
    void AutoAssignModelMaterial(EditorHost& host, MeshRendererComponent& mr);
    void DrawEntityProperties(EditorHost& host);
    void DrawAddComponentMenu(EditorHost& host, GameObject obj);
};
