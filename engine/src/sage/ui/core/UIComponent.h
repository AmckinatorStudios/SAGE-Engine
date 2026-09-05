#pragma once
#include <memory>
#include <string>
#include <vector>

#include "sage/ui/core/UIProperty.h"

// ---------------------------------------------------------------------------
// КОМПОНЕНТ УЗЛА — единица, из которых узел собирается (§7 ТЗ).
//
// Узел интерфейса не бывает «кнопкой» или «картинкой»: он бывает узлом, к
// которому приложены компоненты. Картинка — это узел с UIImage. Кнопка — узел
// с подложкой, надписью и взаимодействием. Своя штука разработчика — узел с
// ЕГО компонентом, зарегистрированным снаружи движка.
//
// Что даёт компонент системе:
//   • тип (UIComponentType) с ключом, названием, порядком рисования и таблицей
//     свойств — по ней работают сериализация, инспектор и анимация;
//   • копию себя (Clone) — дублирование узла, префабы, отмена правки;
//   • при желании — вклад в размер по содержимому (Measure) и в рисование
//     (эмиссию команд отрисовки; см. render/UIDrawBuilder).
//
// ЧЕГО КОМПОНЕНТ НЕ ДЕЛАЕТ. Не двигает соседей, не лезет к детям, не рисует
// через GPU и не знает, кто его нарисует. Нужен значок слева от текста — это
// не «поле значка внутри текста», а два УЗЛА в контейнере с раскладкой.
// ---------------------------------------------------------------------------
namespace sage::ui {

class UIComponent;
class UINode;
class UIDocument;
struct UIComponentType;
struct UIContext;

// Куда компонент попадает в инспекторе (§71: разделы, а не один список из
// сотен полей).
enum class UIComponentCategory {
    Transform,
    Layout,
    Appearance,
    Text,
    Image,
    Mask,
    Effects,
    Interaction,
    Advanced,
};

const char* const* UIComponentCategoryNames();
int UIComponentCategoryCount();

struct UIComponentType {
    std::string Id;      // ключ в файле и в реестре: "transform", "image"
    std::string Title;   // название для человека (через SAGE_UI_TEXT)
    std::string Hint;    // одна строка «зачем это» для редактора
    const char* Icon = nullptr;
    UIComponentCategory Category = UIComponentCategory::Appearance;

    // Порядок рисования ВНУТРИ узла: меньше — ниже. Тень 0, подложка 10,
    // картинка 20, форма 25, шкала 30, значок 50, текст 60, рамка 80. Шаг в
    // десять — чтобы чужой компонент мог встать между своими, не переписывая
    // чужие числа.
    int Order = 100;

    // Можно ли повесить компонент на узел дважды. Две надписи на одном узле —
    // это два узла, а не два компонента: иначе у них не может быть разных
    // прямоугольников, и вся раскладка рассыпается.
    bool Unique = true;
    // Компонент нельзя снять (UITransform). Не «редактор такой не показывает»,
    // а правило самой модели: узел без прямоугольника негде рисовать.
    bool Essential = false;

    std::vector<UIProperty> Props;

    std::unique_ptr<UIComponent> (*Create)() = nullptr;
};

class UIComponent {
public:
    virtual ~UIComponent() = default;

    virtual const UIComponentType& Type() const = 0;
    virtual std::unique_ptr<UIComponent> Clone() const = 0;

    // Вклад компонента в размер по содержимому (§14). available — сколько места
    // дал родитель (0 по оси — «не ограничен»). По умолчанию компонент ничего
    // не требует: подложка, маска и взаимодействие размера не задают.
    virtual glm::vec2 Measure(const UIContext& ctx, const UINode& node,
                              glm::vec2 available) const {
        (void)ctx; (void)node; (void)available;
        return glm::vec2(0.0f);
    }

    // Свойства, которые компонент хранит не полями (списки, вложенные объекты),
    // сериализуются им самим. Пусто по умолчанию: подавляющее большинство
    // компонентов — плоские структуры, и общая таблица покрывает их целиком.
    // Возвращает true, если что-то записал/прочитал.
    virtual bool SaveCustom(void* jsonObject) const { (void)jsonObject; return false; }
    virtual bool LoadCustom(const void* jsonObject) { (void)jsonObject; return false; }

    // Адрес компонента как данных: по нему таблица свойств читает и пишет поля.
    virtual void* Data() = 0;
    const void* Data() const { return const_cast<UIComponent*>(this)->Data(); }
};

// Заготовка для конкретного компонента: избавляет от пяти одинаковых
// переопределений в каждом.
//
//   struct UIImage : UIComponentOf<UIImage> {
//       static const UIComponentType& StaticType();
//       ...поля...
//   };
template <class T>
class UIComponentOf : public UIComponent {
public:
    const UIComponentType& Type() const override { return T::StaticType(); }
    std::unique_ptr<UIComponent> Clone() const override {
        return std::make_unique<T>(static_cast<const T&>(*this));
    }
    void* Data() override { return static_cast<T*>(this); }
};

} // namespace sage::ui
