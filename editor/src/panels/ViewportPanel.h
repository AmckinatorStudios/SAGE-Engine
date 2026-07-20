#pragma once

#include <utility>
#include <vector>

#include <glm/glm.hpp>

class EditorHost;

// Панель Viewport — картинка сцены (редакторская камера) + всё интерактивное:
// полёт камеры (ПКМ+WASDQE), гизмо ImGuizmo (W/E/R), пикинг ЛКМ. Режим гизмо,
// snap, пространство, сетка и режим рендера — ОБЩЕЕ состояние редактора
// (EditorHost), которым также управляет верхний тулбар; панель их лишь читает
// и применяет. Свои — только эфемерные флаги взаимодействия.
class ViewportPanel {
public:
    void Draw(EditorHost& host);

    // Просит вывести таб Viewport вперёд (несколько кадров SetNextWindowFocus —
    // одноразовый проигрывает раскладке доков). По умолчанию активно на старте:
    // Viewport — рабочая панель (пикинг/гизмо/аутлайн), редактор должен
    // открываться на ней, а не на «игровом окне» Game.
    void RequestFocus() { m_focusFrames = 3; }

private:
    bool m_cameraDriving = false; // ПКМ-полёт активен (перехватывает WASD у хоткеев гизмо)
    bool m_gizmoWasUsing = false; // фронт «начали таскать гизмо» -> одна запись undo
    int m_focusFrames = 3;        // >0 — просим фокус (стартовые кадры + после RequestFocus)

    // Мультивыделение: на старте перетаскивания гизмо запоминаем мировые матрицы
    // ВСЕХ выбранных (и первичной). Каждый кадр применяем накопленную дельту
    // первичной ко всем остальным (стабильно, без дрейфа).
    glm::mat4 m_dragStartPrimary{1.0f};
    std::vector<std::pair<int, glm::mat4>> m_dragStartWorlds;
};
