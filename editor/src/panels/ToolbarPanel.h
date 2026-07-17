#pragma once

class EditorHost;

// Тулбар редактора — горизонтальная панель под меню-баром (не оверлей во
// вьюпорте). Слева: режим гизмо (Move/Rotate/Scale), snap, пространство
// (Local/World). По центру: Play/Pause/Stop. Справа: режим рендера
// (Shaded/Wireframe/Unlit/Normals) и сетка. Всё состояние — общее (EditorHost),
// разделяется со вьюпортом.
class ToolbarPanel {
public:
    // Рисует бар высотой height внутри текущего окна-хоста (child-регион).
    void Draw(EditorHost& host, float height);
};
