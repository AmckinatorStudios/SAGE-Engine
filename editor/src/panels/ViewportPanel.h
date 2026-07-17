#pragma once

class EditorHost;

// Панель Viewport — картинка сцены (редакторская камера) + всё интерактивное:
// полёт камеры (ПКМ+WASDQE), гизмо ImGuizmo (W/E/R, snap), пикинг ЛКМ,
// оверлей-тулбар (режим гизмо + snap + Play/Pause/Stop). Владеет настройками
// гизмо/сетки; ход Play-режима и undo — через EditorHost.
class ViewportPanel {
public:
    ViewportPanel();

    void Draw(EditorHost& host);

    bool& ShowGridRef() { return m_showGrid; } // галка в меню Window
    bool ShowGrid() const { return m_showGrid; }

private:
    int m_gizmoOp;              // значение ImGuizmo::OPERATION
    bool m_snap = false;
    bool m_showGrid = true;
    bool m_cameraDriving = false; // ПКМ-полёт активен (перехватывает WASD у хоткеев гизмо)
    bool m_gizmoWasUsing = false; // фронт «начали таскать гизмо» -> одна запись undo
};
