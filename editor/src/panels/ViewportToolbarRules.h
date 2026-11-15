#pragma once
// ---------------------------------------------------------------------------
// ПРАВИЛА ПАНЕЛИ ИНСТРУМЕНТОВ И ИНФОРМАЦИИ ВЬЮПОРТА — чистая логика без
// отрисовки, чтобы её проверяли модульные тесты (tests/test_viewport_toolbar.cpp):
//   * перенос групп строки инструментов, когда вид узкий;
//   * что показывает информационный слой и где он встаёт, не накрывая ни
//     инструменты, ни гизмо ориентации, ни превью камеры, ни предупреждения;
//   * когда кнопка правки коллайдера доступна;
//   * какие слои информации включены (привычка человека — EditorPrefs, не
//     проект и не сцена).
// ---------------------------------------------------------------------------
#include <string>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

namespace sage::editor::vptools {

// --- Перенос групп ----------------------------------------------------------
//
// Ширины групп по порядку; номер строки для каждой. Первая группа (основные
// инструменты) — всегда первой строкой: к ним доступ не теряется никогда.
// Группа шире maxWidth получает свою строку, а не вылезает за край.
std::vector<int> FlowRows(const std::vector<float>& widths, float gap, float maxWidth);

// --- Информационный слой ----------------------------------------------------
struct OverlayPrefs {
    bool Enabled = true;     // всё сразу
    bool View = true;        // вид: перспектива/сверху/спереди/сбоку, номер вида
    bool Selection = true;   // что выделено
    bool Camera = false;     // положение и угол обзора камеры (у ортогонального — масштаб)
    bool Stats = false;      // FPS, вызовы отрисовки, треугольники, объекты, память
    void Load();
    void Save() const;
};

struct OverlayInput {
    std::string ViewName;        // ViewKindName вида
    int Slot = 0, SlotCount = 1; // номер вида и сколько их в раскладке
    bool Active = true;          // в этом виде работают гизмо и хоткеи
    bool Perspective = true;
    glm::vec3 CameraPos{0.0f};
    float Fov = 60.0f;
    float OrthoHeight = 20.0f;   // высота ортогонального вида в метрах (OrthoView::Height)
    int SelectedCount = 0;
    std::string SelectedName;
    // Статистика — та же, что в строке состояния (EditorHost::SceneStats).
    float Fps = 0.0f;
    double MemoryGb = 0.0;
    int DrawCalls = 0;
    long long Triangles = 0;
    int Objects = 0;
};

// Строки слоя сверху вниз. Выделение, камера и статистика — только у
// активного вида: в четырёх видах одно и то же четырежды — шум.
std::vector<std::string> OverlayLines(const OverlayPrefs& prefs, const OverlayInput& in);

// --- Размещение -------------------------------------------------------------
struct Box {
    float X0 = 0, Y0 = 0, X1 = 0, Y1 = 0;
    bool Empty() const { return X1 <= X0 || Y1 <= Y0; }
    bool Overlaps(const Box& o) const {
        return !Empty() && !o.Empty() && X0 < o.X1 && o.X0 < X1 && Y0 < o.Y1 && o.Y0 < Y1;
    }
};
// Блок w×h в левом нижнем углу вида (отступ pad). Мешает что-то из avoid —
// блок поднимается над ним; не помещается в вид — пустая коробка (слой не
// рисуется, а не ложится поверх).
Box PlaceBottomLeft(const Box& cell, float w, float h, float pad, const std::vector<Box>& avoid);

// --- Правка коллайдера ------------------------------------------------------
// Можно ли тянуть ручки коллайдера у объекта: есть коллайдер, и его форма
// редактируется ручками (коробка, сфера, капсула без составных частей).
bool ColliderEditAvailable(const entt::registry& reg, entt::entity e);

// Подпись поля шагов привязки: «1 · 15° · 0.1» или «1/2/0.5 · 15° · 0.1».
std::string SnapLabel(const glm::vec3& move, float rotate, float scale);

} // namespace sage::editor::vptools
