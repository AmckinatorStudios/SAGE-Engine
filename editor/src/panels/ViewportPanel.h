#pragma once

class EditorHost;

// Панель Viewport — картинка сцены (редакторская камера) + всё интерактивное:
// полёт камеры (ПКМ+WASDQE), гизмо ImGuizmo (W/E/R), пикинг ЛКМ. Режим гизмо,
// snap, пространство, сетка и режим рендера — ОБЩЕЕ состояние редактора
// (EditorHost), которым также управляет верхний тулбар; панель их лишь читает
// и применяет. Свои — только эфемерные флаги взаимодействия.
class ViewportPanel {
public:
    void Draw(EditorHost& host);

private:
    bool m_cameraDriving = false; // ПКМ-полёт активен (перехватывает WASD у хоткеев гизмо)
    bool m_gizmoWasUsing = false; // фронт «начали таскать гизмо» -> одна запись undo
};
