#pragma once
#include <functional>
#include <string>
#include <vector>

#include "sage/scene/Light.h"

// ---------------------------------------------------------------------------
// СХЕМА ОКРУЖЕНИЯ СЦЕНЫ — что у неба, цикла суток, окружающего света и тумана
// вообще можно настроить, и КОГДА это имеет смысл.
//
// Раньше это знание жило только в коде панели: длинная функция с `if` на
// каждое поле, где «солнце показывать только у процедурного неба» было
// размазано по сотне строк. Добавить тип неба значило пройти всю функцию и
// угадать, какие из её веток к нему относятся.
//
// Теперь каждая СИСТЕМА (небо, цикл, свет, туман) описана данными: включатель,
// выбор ТИПА и у каждого типа — свои группы свойств. Инспектор рисует по схеме
// и не знает ни одного поля по имени; новый тип неба — это новая запись
// Variant здесь, а не правка панели. Схема — движковая, а не редакторская:
// по тем же ключам к полям можно обратиться из кода (FindProp), и описание
// «что где лежит» одно на всех.
//
// Схема ОПИСЫВАЕТ, а не хранит: значения лежат в LightingEnvironment, как и
// раньше, — сериализация и скриптовый API ходят к ним напрямую и ничем не
// ограничены тем, что показывает инспектор.
// ---------------------------------------------------------------------------
namespace sage::env {

enum class PropKind {
    Bool,
    Float,     // перетаскивание числа (Min/Max/Step/Format)
    Slider,    // ползунок в [Min, Max]
    Enum,      // int, подписи — Options
    Color,     // glm::vec3
    Vec2,      // glm::vec2
    Texture,   // std::string — путь к картинке (слот ассета)
    Folder,    // std::string — путь к папке
    Custom,    // рисует редактор по Key (статус, пресет, ссылка на солнце)
};

using Env = LightingEnvironment;
// Где лежит значение. Указатель, а не копия: инспектор правит на месте.
using FieldFn = std::function<void*(Env&)>;
using CondFn = std::function<bool(const Env&)>;

struct Prop {
    std::string Key;      // устойчивое имя: "sky.sun.size", "fog.density"
    std::string Label;    // по-английски; перевод — в редакторе (T)
    PropKind Kind = PropKind::Float;
    FieldFn Field;        // пусто у Custom
    float Min = 0.0f, Max = 0.0f, Step = 0.01f;
    std::string Format = "%.2f";
    std::vector<std::string> Options;   // Enum: подписи по порядку значений
    std::string Hint;
    CondFn Visible;       // пусто — виден всегда (внутри своей группы)
};

struct Group {
    std::string Id;
    std::string Label;    // пусто — без заголовка: поля идут строками системы
    std::vector<Prop> Props;
    // Включатель группы (облака, луна): выключен — поля группы не показываются.
    FieldFn Toggle;       // bool*
    std::string ToggleHint;
    bool Open = true;     // раскрыта ли по умолчанию
    CondFn Visible;
};

// Тип внутри системы: «процедурное небо», «линейный туман».
struct Variant {
    int Value = 0;        // значение перечисления в данных
    std::string Key;      // "procedural", "height" — для скриптов и тестов
    std::string Label;
    std::string Hint;
    std::vector<Group> Groups;
};

struct System {
    std::string Id;       // "sky", "cycle", "ambient", "fog"
    std::string Label;
    std::string Icon;
    std::string Hint;
    // Включатель системы; выключена — ничего, кроме включателя, не видно.
    FieldFn Enabled;      // bool*; пусто — система всегда включена
    // Выбор типа; пусто — у системы один тип (Variants[0]).
    std::function<int(const Env&)> GetType;
    std::function<void(Env&, int)> SetType;
    std::string TypeLabel = "Type";
    std::vector<Variant> Variants;
    // Группы, общие для всех типов (показываются после групп типа).
    std::vector<Group> Common;
    // Поправка после любой правки (конец тумана не раньше начала и т.п.).
    std::function<void(Env&)> Normalize;
};

// Все системы окружения в порядке показа.
const std::vector<System>& Systems();

// Текущий тип системы (или nullptr, если тип не найден).
const Variant* CurrentVariant(const System& system, const Env& env);
// Системы и свойства по ключу.
const System* FindSystem(const std::string& id);
const Prop* FindProp(const std::string& key);

// Видно ли свойство СЕЙЧАС: система включена, выбран его тип, группа включена
// и видна, собственное условие выполнено. Этим же вопросом пользуются тесты:
// «у одноцветного неба нет ползунка размера солнца».
bool IsPropActive(const std::string& key, const Env& env);

// Тип неба словом и обратно — для скриптов и файлов.
const char* SkySourceKey(SkyboxSettings::Source source);
bool SkySourceFromKey(const std::string& key, SkyboxSettings::Source& out);

} // namespace sage::env
