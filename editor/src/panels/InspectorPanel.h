#pragma once

class EditorHost;

// Панель Inspector — свойства выбранной сущности (имя/Transform/MeshRenderer/
// Material/Script) и редактор материала, выбранного в панели Assets
// (.sagemat правится живьём в разделяемом экземпляре, Save пишет на диск).
class InspectorPanel {
public:
    void Draw(EditorHost& host);

private:
    void DrawMaterialEditor(EditorHost& host);
    void DrawEntityProperties(EditorHost& host);
};
