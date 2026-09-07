#pragma once
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <entt/entt.hpp>

// ---------------------------------------------------------------------------
// ОПИСАНИЕ КОМПОНЕНТОВ СЦЕНЫ — что у объекта есть и из чего оно состоит.
//
// ЗАЧЕМ. Инспектор редактора был двумя тысячами строк, где каждое поле каждого
// компонента расписано руками: подпись, виджет, границы, отмена, «пометить
// сцену изменённой». Из этого следовало ровно то, чего и следовало ожидать:
// поле, добавленное в компонент, для человека не существовало, пока кто-то не
// вспомнит дописать его в инспектор; границы у одного и того же угла в двух
// местах разъезжались; а добавить компонент значило написать ещё сотню строк.
//
// Здесь компонент описан ОДИН раз — списком свойств, — и по этому описанию
// инспектор собирается сам. Забыть нельзя: свойство либо есть в таблице, и
// тогда оно есть в редакторе, либо его нет нигде.
//
// ПОЧЕМУ НЕ sage::ui::UIProperty. Форма описания у них действительно общая, но
// UIProperty живёт в интерфейсе, а сцена от интерфейса не зависит и не должна:
// движок обязан собираться и работать без единой строки UI. Общее у них —
// СЛОВАРЬ (тип, смещение, границы, перечисление), а не список полей, и именно
// списки полей дублировать нельзя.
//
// ЧЕГО ЗДЕСЬ НЕТ. Ни одного слова про то, КАК рисовать: «ползунок» и «путь к
// ассету» — это свойства самого поля (у угла есть границы, у пути есть
// расширения), а не решение редактора. Как выглядит ползунок, решает тот, кто
// его рисует.
// ---------------------------------------------------------------------------
namespace sage::scene {

// Смещение поля внутри компонента. Разностью адресов, а не offsetof: часть
// компонентов имеет нетривиальные члены, и offsetof для них формально не
// разрешён — компилятор честно ругается на каждую строку каждой таблицы.
#define SAGE_FIELD_OFFSET(Type, Field)                                          \
    ((size_t)((const char*)&(reinterpret_cast<const Type*>(1024)->Field)) - 1024u)

// Строка для человека, которую покажет редактор. В движке — обычный литерал:
// движок ничего не переводит. Пометка нужна сборщику переводов.
#ifndef SAGE_TEXT
#define SAGE_TEXT(s) s
#endif

struct Property {
    enum class Kind {
        Bool,
        Int,
        Float,
        Vec2,
        Vec3,
        Color,   // glm::vec3 в 0..1, правится палитрой
        String,
        Enum,    // перечисление, хранится как int
    };

    // Чем правят, когда обычного виджета по типу мало. Декларативно здесь, а не
    // списком исключений в редакторе.
    enum class Widget {
        Auto,
        Slider,    // число в границах Min..Max
        Angle,     // число — градусы
        Asset,     // строка — путь к ассету (см. AssetKinds)
        Multiline,
        ReadOnly,  // показать, но не давать править
    };

    const char* Key = nullptr;    // ключ поля, устойчивый: адресация из скриптов
    const char* Label = nullptr;  // подпись (через SAGE_TEXT)
    Kind Type = Kind::Float;
    size_t Offset = 0;
    float Min = 0.0f;
    float Max = 0.0f;             // Max <= Min — границы не заданы
    const char* const* EnumNames = nullptr;
    int EnumCount = 0;
    Widget Editor = Widget::Auto;
    // Подгруппа внутри компонента: «Тени», «Конус», «Затухание». Компонент из
    // двадцати полей без подгрупп нечитаем, а дробить его на пять компонентов
    // ради вида — врать про модель данных.
    const char* Group = nullptr;
    const char* Tooltip = nullptr;
    // Для Widget::Asset: расширения через запятую (".sagemat,.lua"). Пусто —
    // любой файл.
    const char* AssetKinds = nullptr;

    bool HasRange() const { return Max > Min; }
};

// Описание одного компонента: имя, значок и таблица свойств плюс четыре
// действия, которые нельзя выразить данными, — есть ли компонент, где он
// лежит, как его добавить и как убрать.
struct ComponentType {
    const char* Id = nullptr;      // устойчивый ключ: "MeshRenderer"
    const char* Label = nullptr;   // подпись: "Mesh Renderer"
    const char* Icon = nullptr;    // имя значка из набора SAGE
    const Property* Props = nullptr;
    int PropCount = 0;

    bool (*Has)(const entt::registry&, entt::entity) = nullptr;
    // Указатель на сам компонент — по нему таблица свойств читает и пишет
    // поля. nullptr, если компонента у сущности нет.
    void* (*Data)(entt::registry&, entt::entity) = nullptr;
    void (*Add)(entt::registry&, entt::entity) = nullptr;
    void (*Remove)(entt::registry&, entt::entity) = nullptr;

    // Компонент, без которого объект не объект: имя, положение. Его не
    // предлагают добавить и не дают убрать.
    bool Essential = false;

    const Property* FindProp(std::string_view key) const;
};

// Реестр. Один на процесс: список компонентов движка не зависит ни от сцены,
// ни от проекта.
class ComponentRegistry {
public:
    static ComponentRegistry& Instance();

    void Register(const ComponentType& type);
    const std::vector<ComponentType>& Types() const { return m_types; }
    const ComponentType* Find(std::string_view id) const;
    // Компоненты, которые ЕСТЬ у сущности, в порядке регистрации. Порядок
    // важен: инспектор обязан показывать объект одинаково от кадра к кадру, а
    // порядок хранения в ECS этого не обещает.
    std::vector<const ComponentType*> Of(entt::registry& reg, entt::entity e) const;

private:
    ComponentRegistry();
    std::vector<ComponentType> m_types;
};

// --- Доступ к полю ----------------------------------------------------------
//
// Отдельными помощниками, потому что читают их несколько (инспектор, скрипты,
// будущая анимация свойств), и приведение типов должно быть записано ОДИН раз.
template <class T> T& FieldAs(void* data, const Property& p) {
    return *reinterpret_cast<T*>(static_cast<char*>(data) + p.Offset);
}
template <class T> const T& FieldAs(const void* data, const Property& p) {
    return *reinterpret_cast<const T*>(static_cast<const char*>(data) + p.Offset);
}

// Числовой доступ: component — какая по счёту компонента у vec2/vec3/цвета
// (0..2), для Float/Int/Bool/Enum игнорируется.
bool PropertyGetFloat(const void* data, const Property& p, int component, float& out);
bool PropertySetFloat(void* data, const Property& p, int component, float value);
// Сколько числовых компонент у свойства (0 — не числовое).
int PropertyFloatCount(const Property& p);

// Регистрация встроенных компонентов движка. Зовётся сама при первом обращении
// к реестру — редактору не надо помнить об этом.
void RegisterBuiltinComponents(ComponentRegistry& registry);

} // namespace sage::scene
