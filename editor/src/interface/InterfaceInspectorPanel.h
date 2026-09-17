#pragma once
#include <string>

class EditorHost;
class AssetPreview;

#include "../AssetPreview.h"
#include "../FileBrowser.h"

// ---------------------------------------------------------------------------
// СВОЙСТВА ЭЛЕМЕНТА ИНТЕРФЕЙСА — докуемая панель.
//
// Показывает выбранный элемент и его компоненты ТЕМ ЖЕ модулем, что и инспектор
// сцены (UIElementProperties): второй набор полей рядом означал бы, что новый
// компонент появляется в одном месте и не появляется в другом.
//
// Инструменты выравнивания стоят ВЫШЕ свойств: они занимают три строки и нужны
// постоянно, а свойств три десятка, и уехав под них, выравнивание оказалось бы
// за пределами экрана.
// ---------------------------------------------------------------------------
class InterfaceInspectorPanel {
public:
    void Draw(EditorHost& host, bool& open);
    void RequestFocus() { m_focusFrames = 3; }
    void ForgetProject() { m_preview.ForgetProject(); }

private:
    void DrawAlignTools(EditorHost& host);

    AssetPreview m_preview;    // обложки для слотов ассетов в свойствах
    FileBrowser m_browser;     // выбор картинки элемента
    std::string* m_browseTarget = nullptr;
    int m_focusFrames = 0;
};
