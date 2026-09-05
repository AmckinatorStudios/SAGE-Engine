# Расширение

Главный критерий качества этой архитектуры: разработчик должен уметь построить
интерфейс, которого авторы движка не предусмотрели, **не меняя десятки
центральных классов**.

## Свой компонент

Одна структура, одна таблица свойств, одна регистрация:

```cpp
struct MyRadar : sage::ui::UIComponentOf<MyRadar> {
    static const sage::ui::UIComponentType& StaticType();
    float Range = 100.0f;
    sage::ui::UIColor Sweep{0.3f, 1.0f, 0.4f, 1.0f};
};

const sage::ui::UIComponentType& MyRadar::StaticType() {
    static sage::ui::UIComponentType t = [] {
        sage::ui::UIComponentType d;
        d.Id = "my_radar";                 // ключ в файле — не меняется никогда
        d.Title = SAGE_UI_TEXT("Radar");
        d.Category = sage::ui::UIComponentCategory::Appearance;
        d.Order = 40;                      // порядок рисования внутри узла
        d.Create = [] { return std::unique_ptr<sage::ui::UIComponent>(new MyRadar()); };
        d.Props = {
            {"Range", SAGE_UI_TEXT("Range"), sage::ui::UIProperty::Kind::Float,
             SAGE_UI_OFFSET(MyRadar, Range), 0.0f, 500.0f},
            {"Sweep", SAGE_UI_TEXT("Sweep color"), sage::ui::UIProperty::Kind::Color,
             SAGE_UI_OFFSET(MyRadar, Sweep)},
        };
        return d;
    }();
    return t;
}

sage::ui::UIComponentRegistry::Instance().Register(MyRadar::StaticType());
```

Одной этой регистрацией компонент получает:

* сохранение и чтение в `.uidoc`;
* инспектор редактора с правильными виджетами и подгруппами;
* копирование при дублировании узла и в префабах;
* адресацию свойств для анимации и связывания данных
  (`"Panel/Radar.my_radar.Range"`).

Ни строки правок в движке и редакторе. Это проверяется тестом
(`UIx_custom_component_gets_everything_from_one_registration`).

## Как компонент рисуется

Компонент **не рисует себя через GPU**. Он описывает свойства, а превращает их в
фигуры отдельный проход. Регистрируется это отдельно, потому что компонент может
существовать в сборке без рисования вовсе (инструмент, тесты, проверка вёрстки):

```cpp
sage::ui::UIDrawRegistry::Instance().Register("my_radar",
    [](const sage::ui::UIDrawContext& ctx, const sage::ui::UIComponent& c) {
        const MyRadar& r = static_cast<const MyRadar&>(c);
        sage::ui::UIRenderCommand& cmd = ctx.Begin(sage::ui::UIPrimitive::Ring);
        cmd.Color = r.Sweep;
        cmd.Thickness = 3.0f;
    });
```

`ctx.Begin()` отдаёт команду с уже заполненным общим состоянием (прямоугольник,
маска, режим наложения, ключ сортировки): пусть эмиттер заполняет только своё.
Иначе каждый повторяет восемь присваиваний и однажды забывает маску.

## Свой размер по содержимому

```cpp
glm::vec2 Measure(const UIContext& ctx, const UINode& node, glm::vec2 available) const override;
```

Ноль означает «размера не требую» — так отвечают подложка, маска и
взаимодействие, и это ничего не стоит.

## Свой эффект

```cpp
struct MyRipple : sage::ui::UIEffectOf<MyRipple> {
    static const sage::ui::UIEffectType& StaticType();   // Id, Stage, Props
    float Speed = 1.0f;
};
sage::ui::UIEffectRegistry::Instance().Register(MyRipple::StaticType());
```

`Stage` честно объявляет стоимость: `Modulate` (бесплатно), `Behind`/`Front`
(лишняя геометрия), `Offscreen` (промежуточная цель).

## Свой виджет

Виджет — это **рецепт**, а не тип данных:

```cpp
sage::ui::UIWidgetType t;
t.Id = "minimap";
t.Title = SAGE_UI_TEXT("Minimap");
t.Category = SAGE_UI_TEXT("Basic");
t.Build = [](sage::ui::UIDocument& doc, sage::ui::UINodeId parent) {
    sage::ui::UINode& n = *doc.Create("Minimap", parent);
    n.Ensure<sage::ui::UIMask>().Form = sage::ui::UIMask::Shape::Ellipse;
    n.Ensure<MyRadar>();
    return n.Id;
};
sage::ui::UIWidgetRegistry::Instance().Register(std::move(t));
```

Виджет сразу появляется в меню «Создать» редактора. Собранное поддерево — самое
обычное: его можно разобрать, переставить части, добавить свои.

## Свой бэкенд рисования

Один класс с тремя методами — см. [rendering.md](rendering.md).

## Подмена встроенного

Повторная регистрация с тем же ключом **заменяет** прежнюю. Это не оплошность:
так игра подменяет встроенный компонент, эффект или виджет своим, не трогая
движок.

## Локализация строк

Названия компонентов, эффектов, свойств и виджетов, которые увидит человек в
редакторе, оборачиваются в `SAGE_UI_TEXT("...")`. Движок ничего не переводит —
пометка нужна сборщику переводов (`scripts/check_localization.py`), который
обходит весь модуль интерфейса. Без неё русский у новой части молча не
появился бы.
