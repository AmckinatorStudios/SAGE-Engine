#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "sage/ai/Task.h"

// ---------------------------------------------------------------------------
// ТИПЫ ДАННЫХ SAGE AI GRAPH.
//
// Тип пина, переменной графа и поля структуры — СТРОКОЙ ("float", "vec3",
// "Target"), а не перечислением: пользовательский тип регистрируется из Lua во
// время работы, и перечисление C++ о нём узнать не может. Хранится значение
// всё тем же sage::vars::Value — тип графа говорит, КАК его понимать и с чем
// его можно соединить, а не как хранить.
//
// СОВМЕСТИМОСТЬ — правила, а не таблица пар. Встроенные: тот же тип; «any» с
// чем угодно; целое и дробное друг в друга; всё (кроме exec) — в строку;
// векторы между собой; Vec3 в поворот (углы Эйлера); одно значение — в
// список. Свои правила ставятся ВПЕРЕДИ встроенных (AddConnectRule): так
// проект может и разрешить своё («Target» в «vec3» — его позиция), и
// запретить встроенное.
// ---------------------------------------------------------------------------
namespace sage::ai {

namespace type {
inline constexpr const char* Any = "any";
inline constexpr const char* Exec = "exec";
inline constexpr const char* Bool = "bool";
inline constexpr const char* Int = "int";
inline constexpr const char* Float = "float";
inline constexpr const char* String = "string";
inline constexpr const char* Vec2 = "vec2";
inline constexpr const char* Vec3 = "vec3";
inline constexpr const char* Color = "color";
inline constexpr const char* Rotation = "rotation";
inline constexpr const char* Transform = "transform";
inline constexpr const char* Object = "object";
inline constexpr const char* Asset = "asset";
inline constexpr const char* Array = "array";
inline constexpr const char* Struct = "struct";
inline constexpr const char* Task = "task";   // ручка выполнения: номер Run
} // namespace type

struct TypeInfo {
    std::string Id;
    std::string Title;        // для человека (по-английски, переводит редактор)
    std::string Description;
    vars::Value Default;
    // Поля структурного типа. Непустые — тип структурный: Default — структура
    // с этими полями, struct.make/struct.break дают пины по ним.
    std::vector<Param> Fields;
    uint32_t Color = 0xFFB0B0B0;   // цвет пина (ABGR, как у ImGui)
    std::string Origin = "C++";
};

// 1 — можно, 0 — нельзя, -1 — «не моё дело», решают следующие правила.
using ConnectRule = std::function<int(const std::string& from, const std::string& to)>;
// Своё приведение: true — приведено в out.
using ConvertRule = std::function<bool(const vars::Value& in, const std::string& to, vars::Value& out)>;

class TypeRegistry {
public:
    TypeRegistry();

    void Register(TypeInfo info);
    const TypeInfo* Find(const std::string& id) const;
    std::vector<const TypeInfo*> All() const;

    void AddConnectRule(ConnectRule rule) { m_connect.insert(m_connect.begin(), std::move(rule)); }
    void AddConvertRule(ConvertRule rule) { m_convert.insert(m_convert.begin(), std::move(rule)); }

    // Можно ли провести провод из выхода типа from во вход типа to. why —
    // причина отказа для подсказки у курсора.
    bool CanConnect(const std::string& from, const std::string& to, std::string* why = nullptr) const;
    // Привести значение к типу пина. Неизвестный тип — значение как есть.
    vars::Value Convert(const vars::Value& value, const std::string& to) const;
    vars::Value Default(const std::string& id) const;
    // Тип значения: вид для простых, имя типа для именованной структуры.
    std::string Of(const vars::Value& value) const;
    bool IsStruct(const std::string& id) const;
    uint64_t Version() const { return m_version; }

private:
    uint64_t m_version = 1;
    std::map<std::string, TypeInfo> m_types;
    std::vector<ConnectRule> m_connect;
    std::vector<ConvertRule> m_convert;
};

} // namespace sage::ai
