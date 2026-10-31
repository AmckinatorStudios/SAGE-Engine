#pragma once
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

// ---------------------------------------------------------------------------
// ЗНАЧЕНИЕ, О КОТОРОМ ДОГОВОРИЛИСЬ ВСЕ.
//
// Публичная переменная скрипта, ссылка на объект, аргумент события и поле
// части интерфейса — это одно и то же по сути: ИМЕНОВАННОЕ ТИПИЗИРОВАННОЕ
// ЗНАЧЕНИЕ, которое надо показать в инспекторе, записать в сцену, прочитать из
// сцены и отдать в Lua. Пока у каждой из этих четырёх задач свой тип, каждая
// новая возможность стоит четырёх правок в четырёх файлах — и расходятся они
// молча: поле сохраняется, но не читается.
//
// Здесь один тип на всех. Добавить новый вид значения — значит дописать его в
// Kind и в четыре свитча (запись, чтение, инспектор, Lua), и компилятор
// покажет все четыре: свитчи без default по построению.
//
// СОСТАВНЫЕ ЗНАЧЕНИЯ (Array, Struct). Их завели ради SAGE AI Graph: список
// целей, точки маршрута, «цель = {позиция, видна ли}» — без них граф не может
// хранить данные сложного поведения, а Lua не может отдать графу таблицу.
// Инспектор публичных переменных их по-прежнему не предлагает (AllKinds):
// одной строкой инспектора массив не покажешь, и составное состояние объекта
// сцены — это всё ещё отдельный объект со своими переменными. Хранятся они
// через общий указатель с копированием при записи: значения в графе
// копируются на каждом проводе, и копировать сотню элементов ради чтения
// одного — расточительство.
// ---------------------------------------------------------------------------
namespace sage::vars {

// Ссылка на объект сцены.
//
// ХРАНИТСЯ Id, А НЕ entt::entity, и это не деталь: entity — номер в реестре,
// он меняется при загрузке сцены, а Id переживает запись на диск (по нему уже
// связывается иерархия). Отдельный тип, а не голый int, чтобы «ссылка на
// объект» и «просто число» не путались ни в инспекторе, ни в Lua.
struct EntityRef {
    int Id = 0; // 0 — «ни на кого»
    bool Valid() const { return Id > 0; }
    bool operator==(const EntityRef& o) const { return Id == o.Id; }
};

// Ссылка на файл проекта: путь относительно корня проекта (см. Project::
// AssetRef). Отдельно от строки по той же причине: инспектор обязан дать ей
// слот с приёмом перетаскивания, а не поле для набора пути руками.
struct AssetRef {
    std::string Path;
    bool Valid() const { return !Path.empty(); }
    bool operator==(const AssetRef& o) const { return Path == o.Path; }
};

class Value;
struct ArrayBox;
struct StructBox;

// Вид значения. Порядок важен: он совпадает с порядком альтернатив варианта.
// Новые виды — только В КОНЕЦ.
enum class Kind {
    Bool = 0,
    Int,
    Float,
    String,
    Vec2,
    Vec3,
    Color,   // vec4, показывается палитрой
    Entity,  // ссылка на объект сцены
    Asset,   // ссылка на файл проекта
    Rotation,// кватернион
    Array,   // список значений
    Struct,  // именованные поля; Type — имя типа (зарегистрированного или нет)
};

using Storage = std::variant<bool, int, float, std::string, glm::vec2, glm::vec3, glm::vec4,
                             EntityRef, AssetRef, glm::quat, std::shared_ptr<ArrayBox>,
                             std::shared_ptr<StructBox>>;
using Field = std::pair<std::string, Value>;

// Одно значение: вид плюс данные. Вид хранится ОТДЕЛЬНО от варианта, потому
// что Color и Vec4 — одна и та же четвёрка с разным смыслом, и различить их по
// содержимому нельзя.
class Value {
public:
    Value() : m_data(false) {}
    explicit Value(bool v) : m_data(v) {}
    explicit Value(int v) : m_data(v) {}
    explicit Value(float v) : m_data(v) {}
    explicit Value(std::string v) : m_data(std::move(v)) {}
    explicit Value(glm::vec2 v) : m_data(v) {}
    explicit Value(glm::vec3 v) : m_data(v) {}
    explicit Value(glm::vec4 v) : m_data(v) {}
    explicit Value(EntityRef v) : m_data(v) {}
    explicit Value(AssetRef v) : m_data(std::move(v)) {}
    explicit Value(glm::quat v) : m_data(v) {}

    static Value Array(std::vector<Value> items = {});
    static Value Struct(std::string type, std::vector<Field> fields = {});

    Kind Type() const { return (Kind)m_data.index(); }

    // Чтение с ПОДСТАНОВКОЙ, а не с исключением: значение приходит из файла и
    // из инспектора, и «в сцене лежит число там, где скрипт ждёт строку» —
    // обычная история после переименования переменной, а не повод падать.
    bool AsBool(bool fallback = false) const;
    int AsInt(int fallback = 0) const;
    float AsFloat(float fallback = 0.0f) const;
    std::string AsString(const std::string& fallback = {}) const;
    glm::vec2 AsVec2(glm::vec2 fallback = glm::vec2(0.0f)) const;
    glm::vec3 AsVec3(glm::vec3 fallback = glm::vec3(0.0f)) const;
    glm::vec4 AsVec4(glm::vec4 fallback = glm::vec4(1.0f)) const;
    EntityRef AsEntity() const;
    AssetRef AsAsset() const;
    // Поворот: из кватерниона, из углов Эйлера в градусах (Vec3) или из
    // четвёрки x, y, z, w.
    glm::quat AsRotation() const;

    // --- Составные ---------------------------------------------------------
    // Чтение — без копий; пустой список/полей нет, если вид другой.
    const std::vector<Value>& Items() const;
    // Запись — со своей копией, если список разделён с другим значением.
    // Значение другого вида сначала становится пустым списком.
    std::vector<Value>& MutableItems();
    const std::string& StructType() const;
    const std::vector<Field>& Fields() const;
    const Value* FindField(const std::string& name) const;
    void SetField(const std::string& name, const Value& value);

    // Указатель на данные ровно того вида — для инспектора, который правит
    // значение на месте. nullptr, если вид другой.
    template <class T>
    T* Get() { return std::get_if<T>(&m_data); }
    template <class T>
    const T* Get() const { return std::get_if<T>(&m_data); }

    // Составные сравниваются по содержимому, а не по указателю.
    bool operator==(const Value& o) const;
    bool operator!=(const Value& o) const { return !(*this == o); }

    // Пустое значение нужного вида — с него начинается новая переменная.
    static Value Default(Kind kind);

    // Приведение к другому виду С СОХРАНЕНИЕМ смысла, насколько возможно:
    // человек в инспекторе меняет тип переменной, и обнулять её при этом —
    // терять уже введённое.
    static Value Convert(const Value& from, Kind to);

private:
    Storage m_data;
};

struct ArrayBox {
    std::vector<Value> Items;
};
struct StructBox {
    std::string Type;
    std::vector<Field> Fields;
};

// Имена видов для файла и для интерфейса. Именами, а не числами: номер вида в
// файле сцены сломался бы от вставки нового вида в середину перечисления.
const char* KindId(Kind kind);
bool ParseKind(const std::string& id, Kind& out);
// Виды, которые предлагает инспектор публичных переменных (без составных).
const std::vector<Kind>& AllKinds();

} // namespace sage::vars
