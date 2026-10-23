#pragma once
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "sage/lang/SageLexer.h"

// ---------------------------------------------------------------------------
// SAGE Lua: ДЕРЕВО РАЗБОРА.
//
// Одно дерево на весь конвейер: парсер его строит, проверка типов читает и
// дописывает выводы (Inferred), генератор превращает в Lua. Узлы — простые
// структуры с общими полями, а не иерархия классов с visitor'ами: языковых
// конструкций три десятка, и switch по виду узла читается проще, чем двойная
// диспетчеризация, размазанная по сорока файлам.
//
// Каждый узел помнит позицию в исходнике: на ней стоит И диагностика («файл:
// строка:столбец»), И генератор, который кладёт Lua-код на ту же строку, —
// поэтому ошибка Lua во время игры указывает на строку .slua, а не на
// сгенерированный текст.
// ---------------------------------------------------------------------------
namespace sage::lang {

struct TypeExpr;
using TypePtr = std::shared_ptr<TypeExpr>;
struct Expr;
using ExprPtr = std::shared_ptr<Expr>;
struct Stmt;
using StmtPtr = std::shared_ptr<Stmt>;
struct Block;
using BlockPtr = std::shared_ptr<Block>;
struct FuncBody;
using FuncPtr = std::shared_ptr<FuncBody>;

// --- Запись типа, как её написал человек ------------------------------------
struct TypeExpr {
    enum class K {
        Name,       // number, Entity, Pool<Enemy>, map<K,V> (Args — параметры)
        Optional,   // T?           Args[0]
        Array,      // T[]          Args[0]
        Union,      // A | B        Args
        Tuple,      // (A, B)       Args — несколько возвращаемых значений
        Function,   // function(a: A): R   Args — параметры, Ret
        Table,      // { a: A, b: B }       Fields
    };
    K Kind = K::Name;
    std::string Name;
    std::vector<TypePtr> Args;
    TypePtr Ret;
    std::vector<std::pair<std::string, TypePtr>> Fields;
    Pos P;
};

// --- Аннотация: @field(editable = true, range = {...}) ----------------------
struct Annotation {
    std::string Name;
    Pos P;
    bool HasParens = false;
    std::vector<std::pair<std::string, ExprPtr>> Named;   // key = value
    std::vector<ExprPtr> Positional;                       // @require(RigidBody)
};

struct Param {
    std::string Name;
    TypePtr Type;
    ExprPtr Default;
    Pos P;
};

struct FuncBody {
    std::vector<Param> Params;
    bool Vararg = false;
    std::string VarargName;   // ...values: number[] — имя; пусто — обычный ...
    TypePtr VarargType;
    TypePtr Ret;
    bool HasRet = false;
    BlockPtr Body;
    ExprPtr ArrowExpr;        // (x) => x * x — тело выражением
    bool Async = false;
    bool IsMethod = false;    // self — неявный первый параметр
    std::vector<std::string> TypeParams;
    Pos P;
    Pos EndP;
};

struct Expr {
    enum class K {
        Nil, True, False, Number, String, Vararg,
        Function,    // Fn
        Table,       // Fields
        Binary,      // Op, A, B
        Unary,       // Op, A
        Name,        // Text
        Index,       // A[B]
        Field,       // A.Text
        Call,        // A(Args)
        MethodCall,  // A:Text(Args)
        Paren,       // (A)
        Await,       // await A
        Generic,     // A<TypeArgs> — стирается генератором
        Super,       // super — только внутри метода класса
    };
    K Kind = K::Nil;
    Pos P;
    std::string Text;
    Tok Op = Tok::Eof;
    ExprPtr A, B;
    std::vector<ExprPtr> Args;
    FuncPtr Fn;
    struct TField {
        enum class K { Positional, Named, Keyed };
        K Kind = K::Positional;
        std::string Name;
        ExprPtr Key, Value;
        Pos P;
    };
    std::vector<TField> Fields;
    std::vector<TypePtr> TypeArgs;
};

struct Block {
    std::vector<StmtPtr> Stmts;
    Pos EndP;
};

// Член класса, интерфейса или структуры.
struct Member {
    enum class K { Field, Method };
    K Kind = K::Field;
    std::string Name;
    Pos P;
    std::string Access = "public";
    bool AccessWritten = false;
    bool Static = false;
    bool Override = false;
    bool Final = false;
    bool Const = false;
    TypePtr Type;
    ExprPtr Default;
    FuncPtr Fn;
    bool Abstract = false;   // метод интерфейса без тела
    std::vector<Annotation> Annotations;
};

struct ClassDecl {
    enum class K { Class, Interface, Struct };
    K Kind = K::Class;
    std::string Name;
    std::vector<std::string> TypeParams;
    TypePtr Extends;                 // у интерфейса — первый из Implements
    std::vector<TypePtr> Implements; // у интерфейса — родительские интерфейсы
    std::vector<Member> Members;
    std::vector<Annotation> Annotations;
    bool Final = false;
    Pos P;
    Pos EndP;
};

struct EnumDecl {
    std::string Name;
    struct Item {
        std::string Name;
        ExprPtr Value;
        Pos P;
    };
    std::vector<Item> Items;
    Pos P;
    Pos EndP;
};

struct EventDecl {
    std::string Name;
    std::vector<Param> Params;
    Pos P;
};

struct Stmt {
    enum class K {
        Local,          // local/const/var/:= — Names, Types, Attribs, Exprs, Decl
        Assign,         // Targets = Exprs
        Call,           // CallE
        Do,             // Body
        While,          // Cond, Body
        Repeat,         // Body, Cond
        If,             // Clauses, Else
        NumFor,         // Names[0] = Exprs[0], Exprs[1], [Exprs[2]] do Body
        GenFor,         // Names in Exprs do Body
        Function,       // function a.b:c() — FuncName, Method, Fn
        LocalFunction,  // local function f()
        Return,         // Exprs
        Break,
        Goto,           // Text
        Label,          // Text
        Class,          // Class (класс, интерфейс, структура)
        Enum,           // Enum
        Event,          // Event
        TypeAlias,      // Text = Alias
        Import,         // Imports from From
    };
    enum class Decl { Local, Const, Var, Infer };
    K Kind = K::Do;
    Pos P;
    std::vector<std::string> Names;
    std::vector<TypePtr> Types;
    std::vector<std::string> Attribs;
    std::vector<ExprPtr> Exprs;
    Decl DeclKind = Decl::Local;
    std::vector<ExprPtr> Targets;
    ExprPtr Cond;
    ExprPtr CallE;
    BlockPtr Body;
    std::vector<std::pair<ExprPtr, BlockPtr>> Clauses;
    BlockPtr Else;
    std::vector<std::string> FuncName;   // a.b.c
    std::string Method;                  // :c
    FuncPtr Fn;
    std::string Text;
    bool Exported = false;
    std::shared_ptr<ClassDecl> Class;
    std::shared_ptr<EnumDecl> Enum;
    std::shared_ptr<EventDecl> Event;
    TypePtr Alias;
    std::vector<std::string> TypeParams;
    struct ImportName {
        std::string Name;
        std::string As;
        Pos P;
    };
    std::vector<ImportName> Imports;
    std::string ImportAll;   // import * as M from "..."
    std::string From;
    Pos FromP;
    std::vector<Annotation> Annotations;   // аннотации перед функцией
};

struct Chunk {
    BlockPtr Body;
};

} // namespace sage::lang
