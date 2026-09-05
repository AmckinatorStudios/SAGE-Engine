#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "sage/ui/core/UIComponent.h"
#include "sage/ui/input/UIInteraction.h"
#include "sage/ui/style/UIDesignTokens.h"

// ---------------------------------------------------------------------------
// СТИЛИ (§58, §59, §106 ТЗ).
//
// Стиль — это НАБОР ЗНАЧЕНИЙ СВОЙСТВ, наложенный на узел: «у кнопки такой фон,
// такое скругление, такой кегль». Не отдельный вид узла и не наследование
// классов: любой узел может носить любой стиль.
//
// ПРИОРИТЕТ ЯВНЫЙ И ДОКУМЕНТИРОВАННЫЙ (§59), потому что «почему у этой кнопки
// другой цвет» — второй по частоте вопрос после «почему её не видно»:
//
//   1. значения по умолчанию компонента          — самое слабое
//   2. стиль темы для типа виджета               ("Button")
//   3. именованный стиль узла                    (UIStyled::Style)
//   4. стиль состояния                           ("Button:hover")
//   5. локальные значения, заданные прямо в узле — самое сильное
//
// Пятый пункт сильнее всех намеренно: правка «вот здесь и вот сейчас» обязана
// работать без разбирательства с темой. Чтобы это не превращалось в хаос,
// узел помнит, какие свойства заданы локально (Overrides) — редактор их
// подсвечивает и умеет сбросить.
// ---------------------------------------------------------------------------
namespace sage::ui {

class UIDocument;
class UINode;

// Одно значение свойства в стиле. Хранится как строка ключа компонента +
// строка ключа свойства + значение: стиль не должен зависеть от того, какие
// компоненты существуют в этой сборке (§96).
struct UIStyleValue {
    std::string Component; // "fill", "text", "border"
    std::string Property;  // "Color", "Radius"
    // Значение в текстовом виде или числами — что подошло по типу свойства.
    // Тексты нужны токенам: "@Color.Accent" разрешается темой при применении.
    std::string Text;
    glm::vec4 Numbers{0.0f};
    bool IsText = false;
};

struct UIStyle {
    std::string Name;
    std::string Parent; // наследование стилей: "ButtonPrimary" от "Button"
    std::vector<UIStyleValue> Values;
    // Значения для состояний (§106): ключ — "hover", "pressed", "disabled",
    // "focused", "selected", "checked".
    std::unordered_map<std::string, std::vector<UIStyleValue>> States;
};

// Компонент «узел носит стиль».
struct UIStyled : UIComponentOf<UIStyled> {
    static const UIComponentType& StaticType();
    std::string Style;
    // Свойства, заданные локально и защищённые от темы: "fill.Color".
    std::vector<std::string> Overrides;

    bool IsOverridden(const std::string& path) const;
    void SetOverride(const std::string& path, bool on);

    bool SaveCustom(void* jsonObject) const override;
    bool LoadCustom(const void* jsonObject) override;
};

// ---------------------------------------------------------------------------
// ТЕМА (§105) — стили плюс токены плюс типографика, одним ресурсом.
// ---------------------------------------------------------------------------
class UITheme {
public:
    UIDesignTokens Tokens;
    std::unordered_map<std::string, UIStyle> Styles;
    std::string Name;

    const UIStyle* Find(const std::string& name) const;
    UIStyle& Ensure(const std::string& name);

    // Применить тему к документу: пройти узлы и положить в них значения стилей,
    // не трогая локальные правки. Возвращает число изменённых узлов.
    int Apply(UIDocument& doc) const;
    // Применить к одному узлу с учётом его текущих визуальных состояний.
    void ApplyTo(UINode& node, uint32_t stateFlags) const;

    // Тема по умолчанию — нейтральная тёмная. Ровно одна и в движке, и в
    // редакторе: «как это будет выглядеть в игре» не должно зависеть от того,
    // где смотришь.
    static UITheme Default();

    // Разрешить ссылку на токен ("@Color.Accent") в значение.
    bool ResolveColor(const std::string& ref, UIColor& out) const;
    bool ResolveNumber(const std::string& ref, float& out) const;
};

} // namespace sage::ui
