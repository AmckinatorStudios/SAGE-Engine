# Аудит старой системы и переезд

## Что было

| Файл | Что делал | Решение |
|---|---|---|
| `sage/ui/UI.h/.cpp` | математика раскладки, заготовки | **MIGRATE** → `layout/`, `widgets/` |
| `sage/ui/UIRenderer.*` | immediate-mode рисование примитивов | **KEEP** — стал одним из бэкендов (`UIClassicBackend`) |
| `sage/ui/UISceneSystem.*` | обход сцены: рисование, ввод, попадание | **REWRITE** → `UILayoutSolver` + `UIDrawBuilder` + `UIInputRouter` |
| `sage/ui/UIPart.*`, `UIParts.cpp` | реестр частей и их отрисовка | **REWRITE** → `UIComponentRegistry` + `UIDrawRegistry` |
| `sage/ui/components/*` | Transform, Fill, Label, Image, Bar, Icon, Mask, Layout, Canvas, Group, Interactable, Range, TextInput | **MIGRATE** → `layout/`, `visual/`, `mask/`, `input/`, `widgets/` |
| `sage/ui/UIPresets.*` | заготовки элементов | **MIGRATE** → `UIWidgetRegistry` |
| `sage/ui/UILayoutTools.*` | привязки и выравнивание для редактора | **KEEP** — инструменты вёрстки, не зависят от модели |
| `sage/ui/UIIcons.*` | векторные значки | **KEEP** — используются новым `UIIcon` через материал |
| `sage/ui/UIAnchor.h` | девять якорей и `UIRect` | **KEEP частично** — `UIRect` общий, якорь стал долями |
| `sage/ui/UIElement.h`, `Widgets.h` | immediate-mode виджеты движка | **KEEP** — не игровой UI, а служебные подписи движка |
| `sage/ui/UILegacy.*` | чтение старого плоского формата | **KEEP** — временный слой чтения старых сцен |
| `sage/ui/UIDemos.*`, `UIShowcase.*` | демо-экраны на старой системе | **DELETE** после переезда игр → `showcase/UIShowcaseDocument` |
| `editor/src/UICanvas.*` | вёрстка мышью во вьюпорте | **KEEP** — для сцен на старой системе |
| `editor/src/UIElementProperties.*` | инспектор частей | **KEEP** — то же |
| `editor/src/UILayoutOps.*` | операции над выделением | **KEEP** — то же |
| `editor/src/panels/UIEditorPanel.*` | окно вёрстки сцены | **KEEP** — переходное |

Новое окно — **UIDocumentPanel** — работает с документами и ни от чего из
списка выше не зависит.

## Правило сосуществования

Новое ядро **не зависит** от старого. Обратное — тоже: старая система работает
ровно как работала, ни один существующий проект не сломался.

Старое остаётся временным слоем миграции: изолированным, помеченным и
подлежащим удалению, когда игры переедут.

## Как перевести сцену

```cpp
#include "sage/ui/serialization/UIMigration.h"

sage::ui::UIDocument doc;
sage::ui::UIMigrationReport report = sage::ui::UIMigrateSceneUI(scene, doc);
sage::ui::UISaveDocument(doc, "assets/ui/hud.uidoc");
for (const std::string& w : report.Warnings) LOG_WARN("UI") << w;
```

### Что переводится точно

| Было | Стало |
|---|---|
| девять якорей | доли `AnchorMin`/`AnchorMax` |
| `Stretch::Horizontal/Vertical/Both` | `UISizeMode::Stretch` + якоря |
| `Transform::Pivot` (сдвиг на долю) | `UITransform::Pivot` (точка на якоре) + якорь |
| `Transform::Layer` | `UINode::Order` |
| `Fill` | `UIFill` |
| `Fill::BorderThickness/Color` | отдельный `UIBorder` |
| `Fill::Gradient` | `UIGradient` с двумя остановками |
| `Fill::ShadowSize/Color` | эффект `UIDropShadow` |
| `Label` | `UIText` (`Scale × 8` → кегль) |
| `Image` | `UIImage` |
| `Bar` | `UIProgress` |
| `Icon` | `UIIcon` |
| `Mask` (флаг обрезки) | `UIMask` с формой и скруглением |
| `Layout` | `UILayout` |
| `Group::Alpha` | `UINode::Opacity` |
| `Interactable::Action` | `UIInteraction::Command` |
| `Range` | `UIRangeValue` |
| `TextInput` | `UITextField` |
| `Canvas` | `UICanvasSettings` документа |

### Что не переводится — и почему

* **Связи событий** (`Interactable::Events`) — это часть игровой сцены, а не
  документа интерфейса. Новый UI сообщает КОМАНДУ; кто её слушает, решает игра.
  Переезд выписывает предупреждение с именем команды.
* **Спрайты состояний** (`Image::SpriteHover/SpritePressed`) — в новой системе
  это дело стиля состояния, и так их можно задать любому свойству, а не только
  спрайту.

Всё, чему не нашлось точного соответствия, попадает в
`UIMigrationReport::Warnings` с именем объекта и причиной. Молча не теряется
ничего.

## Чего переезд намеренно НЕ делает

Он не тащит в новую систему старые ограничения. Якоря дальше правятся как доли,
маска — как маска, тень — как эффект в стеке. Обратного перевода нет и не
планируется: смысл перехода в том, чтобы перестать быть ограниченным.
