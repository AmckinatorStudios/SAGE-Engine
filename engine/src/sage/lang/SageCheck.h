#pragma once
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "sage/lang/SageAst.h"
#include "sage/lang/SageDiagnostics.h"

// ---------------------------------------------------------------------------
// SAGE Lua: СЕМАНТИЧЕСКИЙ АНАЛИЗ.
//
// Типизация ПОСТЕПЕННАЯ, как требует совместимость с Lua: что объявлено с
// типом — проверяется строго, что без типа — считается any и проверок не
// мешает. `local x = 5` остаётся обычным Lua (x можно потом сделать строкой),
// `local x: number = 5` и `local x := 5` — уже обещание, и его держит
// компилятор.
//
// Что проверяется до запуска игры:
//   * типы объявлений, присваиваний, аргументов, возвращаемых значений;
//   * const не переприсваивается, readonly-поле — только в init;
//   * доступ private/protected, override/final, совместимость подписей;
//   * класс реализует все методы своих интерфейсов;
//   * у перечисления нет значения, к которому обращаются;
//   * аннотация стоит перед тем, к чему применима, и её параметры имеют
//     правильные имена, типы и диапазоны;
//   * событие отправляют с аргументами, которые оно объявило;
//   * импорт находит модуль и имя в нём; циклический импорт — ошибка.
// ---------------------------------------------------------------------------
namespace sage::lang {

struct ClassInfo;
struct EnumInfo;
struct EventInfo;
struct FuncSig;

struct Type;
using TypeRef = std::shared_ptr<const Type>;

struct Type {
    enum class K {
        Any, Nil, Boolean, Number, Integer, String, Table, Void,
        Function,   // Sig
        Instance,   // экземпляр класса/структуры/интерфейса: Class, Args — аргументы обобщения
        ClassRef,   // сам класс как значение (Player, Player())
        EnumValue,  // значение перечисления
        EnumRef,    // само перечисление (GameState)
        EventRef,   // объявленное событие
        Array,      // Args[0]
        Map,        // Args[0] -> Args[1]
        Optional,   // Args[0] | nil
        Union,      // Args
        Tuple,      // несколько значений
        Param,      // параметр обобщения (T)
        Record,     // { a: A } — Fields
    };
    K Kind = K::Any;
    std::string Name;
    std::vector<TypeRef> Args;
    std::vector<std::pair<std::string, TypeRef>> Fields;
    const ClassInfo* Class = nullptr;
    const EnumInfo* Enum = nullptr;
    const EventInfo* Event = nullptr;
    std::shared_ptr<const FuncSig> Sig;
};

struct ParamInfo {
    std::string Name;
    TypeRef T;
    bool HasDefault = false;
};

struct FuncSig {
    std::vector<ParamInfo> Params;
    bool Vararg = false;
    TypeRef VarargType;
    TypeRef Ret;          // nullptr — не объявлен (any); Void — ничего не возвращает
    bool Async = false;
    std::vector<std::string> TypeParams;
};

// Метаданные поля из @field(...) — уже проверенные.
struct FieldMeta {
    bool Editable = false;
    std::string Label;
    std::string Tooltip;
    std::string Group;
    bool HasRange = false;
    double Min = 0, Max = 0, Step = 0;
    bool Replicated = false;
    bool Serialize = true;
};

struct FieldInfo {
    std::string Name;
    TypeRef T;
    std::string Access = "public";
    bool Static = false;
    bool Const = false;
    const ClassInfo* Owner = nullptr;
    Pos P;
    FieldMeta Meta;
    ExprPtr Default;
    bool DefaultIsLiteral = false;   // число/строка/логическое/nil — можно держать в классе
};

struct MethodInfo {
    std::string Name;
    std::shared_ptr<const FuncSig> Sig;
    std::string Access = "public";
    bool Static = false;
    bool Override = false;
    bool Final = false;
    bool Abstract = false;
    bool Virtual = false;            // хук жизненного цикла движка: override не обязателен
    const ClassInfo* Owner = nullptr;
    Pos P;
    std::string Deprecated;
    std::string Side;                // "server" / "client" / ""
};

struct ClassInfo {
    enum class K { Class, Interface, Struct, Builtin };
    K Kind = K::Class;
    std::string Name;
    std::string Module;              // файл объявления
    const ClassInfo* Parent = nullptr;
    std::vector<const ClassInfo*> Interfaces;
    std::vector<std::string> TypeParams;
    std::vector<FieldInfo> Fields;   // в порядке объявления
    std::vector<MethodInfo> Methods;
    bool Open = false;               // встроенный тип: неизвестный член — any, а не ошибка
    bool Final = false;
    bool Serializable = false;
    bool Exported = false;
    std::vector<std::string> Requires;   // @require(RigidBody)
    std::string DisplayName, Description, Category;   // @class(...)
    std::string Side;
    std::shared_ptr<ClassDecl> Decl;
    std::string EngineName;          // у встроенного компонента движка — его имя (Audio, Camera…)

    const FieldInfo* OwnField(const std::string& n) const {
        for (const FieldInfo& f : Fields)
            if (f.Name == n) return &f;
        return nullptr;
    }
    const MethodInfo* OwnMethod(const std::string& n) const {
        for (const MethodInfo& m : Methods)
            if (m.Name == n) return &m;
        return nullptr;
    }
    const FieldInfo* FindField(const std::string& n) const {
        for (const ClassInfo* c = this; c; c = c->Parent)
            if (const FieldInfo* f = c->OwnField(n)) return f;
        return nullptr;
    }
    const MethodInfo* FindMethod(const std::string& n) const {
        for (const ClassInfo* c = this; c; c = c->Parent) {
            if (const MethodInfo* m = c->OwnMethod(n)) return m;
            for (const ClassInfo* i : c->Interfaces)
                if (const MethodInfo* m = i->FindMethod(n)) return m;
        }
        return nullptr;
    }
    bool IsOpen() const {
        for (const ClassInfo* c = this; c; c = c->Parent)
            if (c->Open) return true;
        return false;
    }
    bool DerivesFrom(const ClassInfo* other) const {
        for (const ClassInfo* c = this; c; c = c->Parent) {
            if (c == other) return true;
            for (const ClassInfo* i : c->Interfaces)
                if (i == other || i->DerivesFrom(other)) return true;
        }
        return false;
    }
    bool IsComponent() const;
};

struct EnumInfo {
    std::string Name;
    std::vector<std::string> Items;
    bool Exported = false;
    bool Has(const std::string& n) const {
        for (const std::string& s : Items)
            if (s == n) return true;
        return false;
    }
};

struct EventInfo {
    std::string Name;
    std::vector<ParamInfo> Params;
    bool Exported = false;
};

// Что видно под именем в области видимости.
struct Symbol {
    enum class K { Var, Class, Enum, Event, Alias, Module, TypeParam };
    K Kind = K::Var;
    TypeRef T;
    bool Const = false;
    bool Typed = false;       // объявлен с типом: присваивания проверяются
    Pos P;
    const ClassInfo* Class = nullptr;
    const EnumInfo* Enum = nullptr;
    const EventInfo* Event = nullptr;
    TypeRef Alias;
    std::map<std::string, Symbol> Exports;   // Module
};

// Модуль после разбора: что он экспортирует (для import в других файлах).
struct ModuleInfo {
    std::string Path;
    std::map<std::string, Symbol> Exports;
    std::vector<std::shared_ptr<ClassInfo>> Classes;
    std::vector<std::shared_ptr<EnumInfo>> Enums;
    std::vector<std::shared_ptr<EventInfo>> Events;
    BlockPtr Ast;
    bool Ok = true;
};

// Загрузчик модулей: путь из import -> файл и текст. false — не найден.
using ModuleLoader = std::function<bool(const std::string& importPath, const std::string& fromFile,
                                        std::string& resolvedPath, std::string& text)>;

// Общий кэш модулей одного прогона компиляции (и защита от циклов).
struct ModuleCache {
    ModuleLoader Loader;
    std::map<std::string, std::shared_ptr<ModuleInfo>> Done;
    std::vector<std::string> Loading;   // стек — для сообщения о цикле
};

// Результат проверки файла: всё, что нужно генератору и редактору.
struct ProgramInfo {
    std::vector<std::shared_ptr<ClassInfo>> Classes;   // свои классы файла, в порядке объявления
    std::vector<std::shared_ptr<EnumInfo>> Enums;
    std::vector<std::shared_ptr<EventInfo>> Events;
    std::vector<std::string> Imports;                  // разрешённые пути модулей
    std::map<const ClassDecl*, const ClassInfo*> ByDecl;
    const ClassInfo* Find(const std::string& name) const {
        for (const auto& c : Classes)
            if (c->Name == name) return c.get();
        return nullptr;
    }
};

// Проверяет файл. Ошибки и предупреждения — в diag.
ProgramInfo Check(const BlockPtr& ast, const std::string& path, DiagnosticSink& diag, ModuleCache& modules);

// Текст типа для сообщений: number, Entity?, map<string, number>.
std::string TypeName(const TypeRef& t);

// Встроенные типы движка (Component, Entity, Vector3…) — общие для всех модулей.
const ClassInfo* BuiltinClass(const std::string& name);

} // namespace sage::lang
