#pragma once
#include <string>

#include <glm/glm.hpp>

#include "../EditorHost.h"
#include "sage/ui/NineSlice.h"

// ---------------------------------------------------------------------------
// ДЕВЯТИНА ЭЛЕМЕНТА в редакторе спрайтов.
//
// Инспектор открывает девятину КОНКРЕТНОГО вида конкретного элемента
// (картинка, подложка, вид при наведении, дорожка…). Её значения — свои у
// элемента (поля части: sliceBorder, заливки, «рисовать середину»), а не
// общая нарезка картинки: две кнопки на одной текстуре тянутся по-разному.
// Поэтому редактор правит ИХ, вживую и через историю сцены, а не лист или
// .sage9. Какие поля — находится по таблице части (ключ поля нарезки и его
// приставка), а не списком в редакторе: часть игры с девятиной работает сама.
// ---------------------------------------------------------------------------
namespace sage::editor::sprite {

struct BoundSlice {
    std::string Path;             // картинка вида (ссылка ассета)
    glm::vec4 Sprite{0.0f};       // кусок листа (w <= 0 — весь файл)
    sage::ui::NineSlice Slice;
    glm::vec2 ElementSize{0.0f};  // размер элемента — для предпросмотра
    std::string Title;            // «Кнопка › Подложка»
};

// Прочитать девятину вида элемента. false — элемента или вида больше нет.
bool ReadBound(EditorHost& host, const EditorHost::NineSliceTarget& target, BoundSlice& out);
// Записать девятину в вид элемента (и режим вида — «девятина»).
bool WriteBound(EditorHost& host, const EditorHost::NineSliceTarget& target, const sage::ui::NineSlice& slice);

} // namespace sage::editor::sprite
