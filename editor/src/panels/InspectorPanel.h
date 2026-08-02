#pragma once
#include <memory>
#include <string>

#include "AssetPreview.h"
#include "FileBrowser.h"


class EditorHost;
class GameObject;
class Texture;

// Панель Inspector — свойства выбранной сущности (имя/Transform/MeshRenderer/
// Material/компоненты с Add/Remove) и редактор материала, выбранного в панели
// Assets (.sagemat правится живьём в разделяемом экземпляре, Save пишет на диск).
class InspectorPanel {
public:
    void Draw(EditorHost& host);

    // Освободить GPU-ресурсы превью, пока контекст жив (см. AssetPreview).
    void Shutdown() { m_preview.Shutdown(); }

private:
    void DrawMaterialEditor(EditorHost& host);

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
    bool m_pendingMeshLoad = false;  // загрузку делаем в кадре, а не из колбэка диалога
    void DrawModelImportEditor(EditorHost& host); // настройки импорта выбранной модели
    void DrawEntityProperties(EditorHost& host);
    void DrawAddComponentMenu(EditorHost& host, GameObject obj);
};
