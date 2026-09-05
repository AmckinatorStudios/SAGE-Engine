#pragma once
#include <functional>
#include <string>
#include <vector>

#include "sage/ui/core/UIDocument.h"

// ---------------------------------------------------------------------------
// АНИМИРУЕМЫЕ ЗНАЧЕНИЯ И ВНЕШНИЕ ДАННЫЕ (§47, §48 ТЗ).
//
// ЧТО ЗДЕСЬ ЕСТЬ. Механизм «свойство → значение»: адрес свойства узла и
// возможность его прочитать и записать. Ничего больше.
//
// ЧЕГО ЗДЕСЬ НЕТ И БЫТЬ НЕ ДОЛЖНО. Ни системы анимационных сценариев, ни
// таймлайнов, ни игровых источников данных. Интерфейс не ходит за данными сам
// (§48): он предоставляет РОЗЕТКУ, в которую снаружи втыкают значение. Иначе
// интерфейс начинает знать про здоровье и патроны — ровно то, что запрещено.
//
// АДРЕС СВОЙСТВА — строка "Panel/Health/Fill.fill.Color.r":
//   путь к узлу . ключ компонента . ключ свойства [. компонента числа]
// Строкой, потому что адрес обязан переживать сохранение и приходить снаружи
// (из анимации, из скрипта, из редактора).
// ---------------------------------------------------------------------------
namespace sage::ui {

// Разобранный адрес свойства.
struct UIPropertyPath {
    std::string NodePath;
    std::string Component;
    std::string Property;
    int Channel = -1; // -1 — всё значение, иначе номер компоненты (x/y/z/w)

    bool Valid() const { return !Component.empty() && !Property.empty(); }
    std::string ToString() const;
    static UIPropertyPath Parse(const std::string& s);
};

// Привязка к конкретному свойству конкретного документа. Разрешается один раз,
// дальше читает и пишет напрямую — иначе анимация на шестьдесят кадров в
// секунду разбирала бы строку шестьдесят раз в секунду.
class UIPropertyBinding {
public:
    bool Bind(UIDocument& doc, const UIPropertyPath& path);
    bool Bind(UIDocument& doc, const std::string& path) {
        return Bind(doc, UIPropertyPath::Parse(path));
    }

    bool Valid() const { return m_component != nullptr && m_prop != nullptr; }

    bool Get(float& out) const;
    bool Set(float value);
    bool GetVec4(glm::vec4& out) const;
    bool SetVec4(const glm::vec4& v);
    // Строковые свойства (текст надписи, путь к картинке) — самый частый вид
    // связывания данных: «сюда положить число патронов» это именно строка.
    bool SetString(const std::string& v);
    bool GetString(std::string& out) const;

    UINodeId Node() const { return m_node; }
    // Какой флаг «устарело» поднять после записи: смена текста — не то же
    // самое, что смена цвета (§87).
    uint32_t DirtyFlags() const { return m_dirty; }

private:
    UIDocument* m_doc = nullptr;
    UINodeId m_node = kUIInvalidNode;
    class UIComponent* m_component = nullptr;
    const struct UIProperty* m_prop = nullptr;
    int m_channel = -1;
    uint32_t m_dirty = 0;
};

// Анимируемое значение: текущее, целевое и способ приближения. Ровно тот
// «механизм property → value», которого требует §47, — без сценариев.
struct UIAnimatedValue {
    float Current = 0.0f;
    float Target = 0.0f;
    float Speed = 0.0f;    // единиц в секунду; 0 — мгновенно
    float Smoothing = 0.0f;// доля приближения за секунду (экспоненциально)

    void Step(float dt);
    void Snap(float v) { Current = Target = v; }
};

// ИСТОЧНИК ДАННЫХ (§48) — то, чем игра кормит интерфейс. Интерфейс дёргает
// источник, а не наоборот: так он остаётся ведомым и не знает, откуда число.
class IUIDataSource {
public:
    virtual ~IUIDataSource() = default;
    virtual bool Number(const std::string& key, float& out) const { (void)key; (void)out; return false; }
    virtual bool Text(const std::string& key, std::string& out) const { (void)key; (void)out; return false; }
};

// Набор привязок «свойство узла ← ключ источника». Живёт рядом с документом, а
// не внутри него: документ — данные, а привязки — соединение данных с игрой.
class UIBindings {
public:
    void Add(const std::string& propertyPath, const std::string& sourceKey, bool asText = false);
    void Clear();
    // Прочитать источник и разложить значения по документу. Возвращает, сколько
    // свойств реально изменилось (§88: не менялось — не пересчитываем).
    int Apply(UIDocument& doc, const IUIDataSource& source);

private:
    struct Entry {
        std::string Path;
        std::string Key;
        bool AsText = false;
        UIPropertyBinding Binding;
        bool Bound = false;
    };
    std::vector<Entry> m_entries;
};

} // namespace sage::ui
