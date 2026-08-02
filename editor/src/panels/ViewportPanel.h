#pragma once

#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include "imgui.h"

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

    // --- Раскладка видов ----------------------------------------------------
    //
    // Один вьюпорт заставляет крутить камеру ради каждой проверки «а ровно ли
    // стоит», и на глаз в перспективе это всё равно не проверить: у неё нет
    // прямых углов. Ортогональные виды сверху/спереди/сбоку отвечают на такие
    // вопросы сразу — за это их и держат во всех редакторах.
    enum class Layout { Single, TwoColumns, Quad };
    enum class ViewKind { Perspective, Top, Front, Side };

    struct OrthoView {
        // Центр видимой области в МИРЕ и высота кадра в мировых единицах.
        // Не камера с позицией и поворотом: у ортогонального вида нет ни того,
        // ни другого — есть плоскость, по которой ездят, и масштаб.
        glm::vec3 Center{0.0f};
        float Height = 20.0f;
    };

private:
    void DrawViewToolbar(EditorHost& host);
    // Рисует один вид раскладки в текущем регионе. Возвращает true, если курсор
    // над ним (для взаимодействия).
    bool DrawViewImage(EditorHost& host, int slot, ViewKind kind, ImVec2 size, ImVec2& outPos);
    static glm::mat4 OrthoViewMatrix(ViewKind kind, const glm::vec3& center);
    static const char* ViewKindName(ViewKind kind);

    Layout m_layout = Layout::Single;
    ViewKind m_kinds[4] = {ViewKind::Perspective, ViewKind::Top, ViewKind::Front, ViewKind::Side};
    OrthoView m_ortho[4];
    int m_activeSlot = 0;   // где сейчас работают гизмо и хоткеи

    bool m_cameraDriving = false;  // ПКМ-полёт активен (перехватывает WASD у хоткеев гизмо)
    // Куда вернуть курсор после захвата (см. ViewportPanel.cpp).
    ImVec2 m_captureReturnPos{0.0f, 0.0f};
    bool m_gizmoWasUsing = false; // фронт «начали таскать гизмо» -> одна запись undo
    int m_focusFrames = 3;        // >0 — просим фокус (стартовые кадры + после RequestFocus)

    // Мультивыделение: на старте перетаскивания гизмо запоминаем мировые матрицы
    // ВСЕХ выбранных (и первичной). Каждый кадр применяем накопленную дельту
    // первичной ко всем остальным (стабильно, без дрейфа).
    glm::mat4 m_dragStartPrimary{1.0f};
    std::vector<std::pair<int, glm::mat4>> m_dragStartWorlds;
};
