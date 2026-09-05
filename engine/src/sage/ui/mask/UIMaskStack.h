#pragma once
#include <vector>

#include "sage/ui/core/UITypes.h"
#include "sage/ui/mask/UIMask.h"

class Texture;

// ---------------------------------------------------------------------------
// СТЕК МАСОК (§32) — то, во что превращается цепочка масок над узлом.
//
// Считается один раз за проход раскладки и хранится ПЛОСКИМ СПИСКОМ: узел
// ссылается на запись номером. Так вложенные маски не заставляют рисующего
// подниматься по дереву, а одинаковые состояния маски естественно попадают в
// один батч (§85).
// ---------------------------------------------------------------------------
namespace sage::ui {

// Одна маска, уже переведённая в экранные пиксели.
struct UIMaskEntry {
    UIMask::Shape Form = UIMask::Shape::Rect;
    UIMask::Compose Mode = UIMask::Compose::Intersect;
    UIRect Rect{};
    UICorners Radius{0.0f};
    float Softness = 0.0f;
    bool Invert = false;
    const Texture* Tex = nullptr;
    UIMask::Channel Source = UIMask::Channel::Alpha;
    float GradientAngle = 0.0f;
    float GradientStart = 0.0f;
    float GradientEnd = 1.0f;
    bool AffectsHitTest = true;
    // Номер узла-источника: нужен отладке («какая именно маска это срезала»).
    uint32_t Owner = 0;
};

// Состояние маски для узла: прямоугольник ножниц (пересечение всех
// прямоугольных вкладов) плюс список записей, которые рисующий обязан отдать
// шейдеру.
struct UIMaskState {
    UIRect Scissor{0.0f, 0.0f, 0.0f, 0.0f};
    bool HasScissor = false;
    // Индексы в общем списке UIMaskEntry. Пусто — маски нет, и узел не платит
    // за неё ничем (§130).
    std::vector<int> Entries;

    bool Empty() const { return !HasScissor && Entries.empty(); }
};

class UIMaskStack {
public:
    void Clear() { m_entries.clear(); m_states.clear(); }

    // Добавить состояние-потомка: к состоянию родителя добавляется маска mask.
    // Возвращает номер нового состояния.
    int Push(int parentState, const UIMaskEntry& mask);
    // Состояние без масок (корень).
    int Root();

    const UIMaskState& State(int index) const { return m_states[(size_t)index]; }
    const UIMaskEntry& Entry(int index) const { return m_entries[(size_t)index]; }
    const std::vector<UIMaskEntry>& Entries() const { return m_entries; }
    size_t StateCount() const { return m_states.size(); }

    // Значение маски в точке (0..1) — общий ответ и для рисования, и для
    // попадания курсором. Одна функция на двоих ровно затем, чтобы «видно» и
    // «кликается» не могли разойтись.
    float Sample(int state, glm::vec2 point, bool forHitTest) const;

private:
    std::vector<UIMaskEntry> m_entries;
    std::vector<UIMaskState> m_states;
};

// Значение одной маски в точке. Открыто наружу: по нему же считает попадание
// курсором и отладочный слой.
float UIMaskValue(const UIMaskEntry& e, glm::vec2 point);

} // namespace sage::ui
