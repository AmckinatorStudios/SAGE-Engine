#pragma once
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "sage/ui/core/UIComponent.h"
#include "sage/ui/core/UITypes.h"

// ---------------------------------------------------------------------------
// ЭФФЕКТЫ (§37–43 ТЗ) — отдельная универсальная система со СТЕКОМ.
//
// ПОЧЕМУ СТЕК, А НЕ ПОЛЯ. Прежняя тень была двумя полями внутри заливки: одна
// тень, всегда снизу, всегда у подложки. Ни второй тени, ни свечения, ни
// размытия, ни порядка их применения выразить было нельзя, а «стеклянная
// панель» (размытый фон + полупрозрачная заливка + рамка + свечение) не
// собиралась в принципе.
//
// Здесь эффекты — список, и ПОРЯДОК В НЁМ ЗНАЧИМ: размытие после подкраски и
// подкраска после размытия дают разный результат. Свой эффект добавляется
// регистрацией в UIEffectRegistry, без единой правки в ядре рисования (§98).
//
// ЯВНАЯ СТОИМОСТЬ (§131). Каждый эффект честно говорит, что ему нужно:
//   • Modulate — только умножение цвета, лишних проходов нет вовсе;
//   • Behind/Front — дополнительная геометрия до или после узла (тень, свечение);
//   • Offscreen — промежуточная цель рисования (размытие, цветокоррекция
//     поддерева).
// Узел без эффектов не платит ничего: ни цели, ни прохода, ни ветки в шейдере.
// ---------------------------------------------------------------------------
namespace sage::ui {

class UIEffect;

// Что эффекту нужно от рисующего.
enum class UIEffectStage {
    Modulate, // изменить цвет/прозрачность — без дополнительных проходов
    Behind,   // дорисовать геометрию ПОД узлом (тень)
    Front,    // дорисовать геометрию ПОВЕРХ узла (внутренняя тень, блик)
    Offscreen // отрисовать поддерево в цель и обработать её (размытие)
};

struct UIEffectType {
    std::string Id;
    std::string Title;
    std::string Hint;
    const char* Icon = nullptr;
    UIEffectStage Stage = UIEffectStage::Modulate;
    std::vector<UIProperty> Props;
    std::unique_ptr<UIEffect> (*Create)() = nullptr;
};

class UIEffect {
public:
    virtual ~UIEffect() = default;
    virtual const UIEffectType& Type() const = 0;
    virtual std::unique_ptr<UIEffect> Clone() const = 0;
    virtual void* Data() = 0;
    const void* Data() const { return const_cast<UIEffect*>(this)->Data(); }

    bool Enabled = true;
};

template <class T>
class UIEffectOf : public UIEffect {
public:
    const UIEffectType& Type() const override { return T::StaticType(); }
    std::unique_ptr<UIEffect> Clone() const override {
        return std::make_unique<T>(static_cast<const T&>(*this));
    }
    void* Data() override { return static_cast<T*>(this); }
};

// --- Встроенные эффекты (§38) ----------------------------------------------

// Падающая тень (§39). Одной геометрией с мягким краем, а не десятком
// прямоугольников с падающей прозрачностью: десяток прямоугольников — это
// десяток квадов на каждую панель и заметная ступенчатость на большом радиусе.
struct UIDropShadow : UIEffectOf<UIDropShadow> {
    static const UIEffectType& StaticType();
    glm::vec2 Offset{0.0f, 4.0f};
    float Blur = 12.0f;
    float Spread = 0.0f;
    UIColor Color{0.0f, 0.0f, 0.0f, 0.45f};
};

// Внутренняя тень (§40) — обязательна для полей ввода, вдавленных панелей и
// карточек: без неё «вдавленность» изображают второй рамкой другого цвета.
struct UIInnerShadow : UIEffectOf<UIInnerShadow> {
    static const UIEffectType& StaticType();
    glm::vec2 Offset{0.0f, 2.0f};
    float Blur = 8.0f;
    float Spread = 0.0f;
    UIColor Color{0.0f, 0.0f, 0.0f, 0.5f};
};

// Свечение (§41). Вокруг ФАКТИЧЕСКОЙ формы узла, а не вокруг его
// прямоугольника: свечение квадрата вокруг круглой иконки сразу выдаёт подделку.
struct UIGlow : UIEffectOf<UIGlow> {
    static const UIEffectType& StaticType();
    UIColor Color{1.0f, 0.85f, 0.35f, 0.8f};
    float Radius = 16.0f;
    float Intensity = 1.0f;
    bool Inner = false; // свечение внутрь (подсветка края)
};

// Размытие (§42). Единственный из встроенных эффектов, которому нужна
// промежуточная цель, — и это видно прямо в его Stage.
struct UIBlur : UIEffectOf<UIBlur> {
    static const UIEffectType& StaticType();
    float Radius = 8.0f;
    int Passes = 2;
    // Размывать не себя, а ТО, ЧТО ПОД собой (§43: «стекло»). Без этого
    // полупрозрачная панель поверх сцены остаётся просто мутным пятном.
    bool Backdrop = false;
};

// Цветокоррекция (§38): подкраска, яркость, контраст, насыщенность.
struct UIColorEffect : UIEffectOf<UIColorEffect> {
    static const UIEffectType& StaticType();
    UIColor Tint{1.0f, 1.0f, 1.0f, 1.0f};
    float Brightness = 1.0f;
    float Contrast = 1.0f;
    float Saturation = 1.0f;
    float Opacity = 1.0f;
};

// --- Стек эффектов узла -----------------------------------------------------
struct UIEffects : UIComponentOf<UIEffects> {
    static const UIComponentType& StaticType();

    std::vector<std::unique_ptr<UIEffect>> Items;

    UIEffects() = default;
    UIEffects(const UIEffects& o) { *this = o; }
    UIEffects& operator=(const UIEffects& o);

    UIEffect* Add(std::string_view typeId);
    template <class T> T& Ensure() {
        for (auto& e : Items)
            if (&e->Type() == &T::StaticType()) return *static_cast<T*>(e.get());
        Items.push_back(std::make_unique<T>());
        return *static_cast<T*>(Items.back().get());
    }
    template <class T> T* Get() const {
        for (auto& e : Items)
            if (&e->Type() == &T::StaticType()) return static_cast<T*>(e.get());
        return nullptr;
    }
    void Remove(int index);
    void MoveItem(int from, int to);

    // Нужна ли узлу промежуточная цель рисования. Спрашивают перед тем, как её
    // создавать: «нет» здесь означает ноль дополнительной работы (§131).
    bool NeedsOffscreen() const;

    bool SaveCustom(void* jsonObject) const override;
    bool LoadCustom(const void* jsonObject) override;
};

class UIEffectRegistry {
public:
    static UIEffectRegistry& Instance();
    void Register(const UIEffectType& type);
    const UIEffectType* Find(std::string_view id) const;
    const std::vector<const UIEffectType*>& All() const;
    std::unique_ptr<UIEffect> Create(std::string_view id) const;

private:
    UIEffectRegistry() = default;
    void EnsureBuiltins() const;
    mutable std::vector<const UIEffectType*> m_types;
    mutable std::vector<const UIEffectType*> m_sorted;
    mutable bool m_builtinsDone = false;
};

void RegisterBuiltinUIEffects();

} // namespace sage::ui
