#pragma once

class EditorHost;

// Панель Lighting — окружение сцены (Scene::Lighting): полусферический
// ambient (небо/земля/сила) и направленное «солнце». Точечные света — это
// СУЩНОСТИ с LightComponent (Inspector > Light, Entity > Create Light), их
// панель только перечисляет. Всё сериализуется со сценой, правки попадают в
// undo через TrackLastImGuiItem.
class LightingPanel {
public:
    void Draw(EditorHost& host);
};
