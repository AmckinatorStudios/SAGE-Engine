#pragma once

#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include "imgui.h"

#include "UICanvas.h"

class EditorHost;

// Панель Viewport — картинка сцены (редакторская камера) + всё интерактивное:
// полёт камеры (ПКМ+WASDQE), гизмо ImGuizmo (W/E/R), пикинг ЛКМ. Режим гизмо,
// snap, пространство, сетка и режим рендера — ОБЩЕЕ состояние редактора
// (EditorHost), которым также управляет верхний тулбар; панель их лишь читает
// и применяет. Свои — только эфемерные флаги взаимодействия.
class ViewportPanel {
public:
    void Draw(EditorHost& host, bool* open);

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
    // Бросок ассета во вьюпорт: приём сидит на item'е картинки, а постановка
    // происходит ниже по кадру, где уже посчитаны матрицы активного вида.
    struct PendingDrop {
        bool Active = false;
        std::string Path;
        ImVec2 Pos{0.0f, 0.0f};
    };
    PendingDrop m_pendingDrop;

    // Инструменты — ВИДЖЕТОМ ПОВЕРХ вьюпорта (ViewportTools.cpp): гизмо,
    // привязка, показ, раскладка видов. origin — левый верхний угол области
    // видов в экранных координатах.
    void DrawToolsOverlay(EditorHost& host, ImVec2 origin);
    // Рисует один вид раскладки в текущем регионе. Возвращает true, если курсор
    // над ним (для взаимодействия).
    bool DrawViewImage(EditorHost& host, int slot, ViewKind kind, ImVec2 size, ImVec2& outPos);
    static glm::mat4 OrthoViewMatrix(ViewKind kind, const glm::vec3& center);
    static const char* ViewKindName(ViewKind kind);

    Layout m_layout = Layout::Single;
    ViewKind m_kinds[4] = {ViewKind::Perspective, ViewKind::Top, ViewKind::Front, ViewKind::Side};
    OrthoView m_ortho[4];
    int m_activeSlot = 0;   // где сейчас работают гизмо и хоткеи
    // Слот, ЗАХВАТИВШИЙ мышь на время перетаскивания (-1 — никто).
    //
    // В раскладке из четырёх видов всё вело себя по наведению: панорама,
    // масштаб и гизмо слушались того вида, над которым курсор СЕЙЧАС. Стоило,
    // ведя мышь с зажатой кнопкой, пересечь границу — и вид сверху продолжал
    // тянуть уже другой вид, а объект прыгал. Мышь принадлежит тому виду, в
    // котором нажали, и до отпускания кнопки не переходит никуда.
    int m_dragSlot = -1;

    // --- Виджет инструментов поверх картинки ---------------------------------
    //
    // СОСТАВ НАСТРАИВАЕТСЯ. Инструментов набралось на две строки, а две строки
    // поверх картинки — это отрезанный сверху кусок сцены и вечный вопрос, что
    // из этого вообще нужно. Нужное у всех разное: кто-то собирает уровень
    // (гизмо, привязка, посадка), кто-то смотрит материалы (режим отрисовки), а
    // ортогональные виды многим не нужны ни разу. Поэтому строка ОДНА, а группы
    // включаются и выключаются в её же настройках (шестерёнка слева) и
    // запоминаются между запусками (editor_prefs.json — настройка человека, не
    // проекта).
    enum class ToolGroup {
        Gizmo,        // режим манипулятора
        Space,        // оси гизмо: объекта или мира
        Snap,         // привязка к шагу и сам шаг
        Selection,    // кадрировать / посадить / выровнять
        Display,      // сетка, габариты, режим вёрстки
        RenderMode,   // чем показывать сцену
        Views,        // раскладка видов и проекция
        Count
    };
    static const char* ToolGroupKey(ToolGroup g);   // ключ в настройках
    static const char* ToolGroupTitle(ToolGroup g); // как называется в списке
    bool ToolGroupOn(ToolGroup g);                  // с ленивой загрузкой
    void SetToolGroupOn(ToolGroup g, bool on);
    void DrawToolsSettingsPopup();

    bool m_toolGroups[(int)ToolGroup::Count] = {};
    bool m_toolGroupsLoaded = false;
    // Экранный прямоугольник виджета, снятый в КОНЦЕ прошлого кадра. Нужен до
    // того, как виджет нарисован: мышь над ним не должна ни выбирать объект, ни
    // хватать гизмо, а ImGuizmo про окна ImGui ничего не знает и считает
    // попадание по голым координатам.
    ImVec2 m_toolsMin{0.0f, 0.0f};
    ImVec2 m_toolsMax{0.0f, 0.0f};
    bool m_toolsHovered = false;

    UICanvas m_uiCanvas;   // вёрстка интерфейса мышью (режим UI)
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
