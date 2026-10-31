#include <algorithm>
#include <cmath>
#include <functional>
#include <random>
#include <string>

#include <glm/gtc/quaternion.hpp>

#include "sage/ai/GraphNodesDetail.h"
#include "sage/ai/World.h"

// ---------------------------------------------------------------------------
// Узлы данных: математика, сравнения, логика, преобразования, строки,
// списки, структуры, повороты. Все чистые — вычисляются, когда их спросили.
//
// АРИФМЕТИКА ПО ВИДУ ЗНАЧЕНИЯ, а не узел на каждый тип. «Add» складывает и
// целые, и дробные, и векторы (вектор с числом — покомпонентно): иначе в
// палитре было бы двенадцать «сложений», и человек выбирал бы не действие, а
// тип провода. Целое с целым даёт целое — счётчики остаются счётчиками.
// ---------------------------------------------------------------------------
namespace sage::ai::nodes {

using vars::Kind;
using vars::Value;

namespace {

int Rank(const Value& v) {
    switch (v.Type()) {
        case Kind::Vec3: return 3;
        case Kind::Color: return 4;
        case Kind::Vec2: return 2;
        case Kind::Int: return 0;
        case Kind::Bool: return 0;
        default: return 1;
    }
}

Value Arith(const Value& a, const Value& b, const std::function<float(float, float)>& op, bool intOp) {
    const int r = std::max(Rank(a), Rank(b));
    if (r == 0 && intOp) return Value((int)op((float)a.AsInt(), (float)b.AsInt()));
    if (r == 0 || r == 1) return Value(op(a.AsFloat(), b.AsFloat()));
    auto vec = [](const Value& v, int n) -> glm::vec4 {
        if (Rank(v) <= 1) return glm::vec4(v.AsFloat());
        glm::vec4 x = v.AsVec4();
        if (n == 2 && v.Type() == Kind::Vec2) x = glm::vec4(v.AsVec2(), 0.0f, 0.0f);
        if (v.Type() == Kind::Vec3) x = glm::vec4(v.AsVec3(), 0.0f);
        return x;
    };
    const glm::vec4 x = vec(a, r), y = vec(b, r);
    const glm::vec4 z(op(x.x, y.x), op(x.y, y.y), op(x.z, y.z), op(x.w, y.w));
    if (r == 2) return Value(glm::vec2(z));
    if (r == 3) return Value(glm::vec3(z));
    return Value(z);
}

Value Map(const Value& a, const std::function<float(float)>& f) {
    switch (a.Type()) {
        case Kind::Vec2: { glm::vec2 v = a.AsVec2(); return Value(glm::vec2(f(v.x), f(v.y))); }
        case Kind::Vec3: { glm::vec3 v = a.AsVec3(); return Value(glm::vec3(f(v.x), f(v.y), f(v.z))); }
        case Kind::Int: return Value((int)f((float)a.AsInt()));
        default: return Value(f(a.AsFloat()));
    }
}

std::mt19937& Rng() {
    static std::mt19937 rng(std::random_device{}());
    return rng;
}

// Короткие описания чистых узлов: (id, заголовок, раздел, входы, выходы, тело).
void Pure(NodeRegistry& r, const std::string& id, const std::string& title, const std::string& category,
          const std::string& description, std::vector<PinSpec> in, std::vector<PinSpec> out,
          std::function<void(GraphExec&)> body, std::vector<std::string> keywords = {}) {
    NodeType t = Node(id, title, category, description);
    t.Pure = true;
    t.Inputs = std::move(in);
    t.Outputs = std::move(out);
    t.Keywords = std::move(keywords);
    t.Run = [body = std::move(body)](GraphExec& ex) {
        body(ex);
        return NodeResult::Go("");
    };
    r.Register(std::move(t));
}

void Binary(NodeRegistry& r, const std::string& id, const std::string& title, std::function<float(float, float)> op,
            bool intOp, std::vector<std::string> keywords = {}) {
    Pure(r, "math." + id, title, "Math", "Works on numbers and vectors (a vector with a number — per component).",
         {P("a", type::Any, Value(0.0f)), P("b", type::Any, Value(0.0f))}, {P("result", type::Any)},
         [op, intOp](GraphExec& ex) { ex.Out("result", Arith(ex.In("a"), ex.In("b"), op, intOp)); },
         std::move(keywords));
}

void Unary(NodeRegistry& r, const std::string& id, const std::string& title, std::function<float(float)> f) {
    Pure(r, "math." + id, title, "Math", "Works on a number or a vector.", {P("x", type::Any, Value(0.0f))},
         {P("result", type::Any)}, [f](GraphExec& ex) { ex.Out("result", Map(ex.In("x"), f)); });
}

void Compare(NodeRegistry& r, const std::string& id, const std::string& title, std::function<bool(float, float)> f) {
    Pure(r, "compare." + id, title, "Logic", "Compares two numbers.",
         {P("a", type::Float, Value(0.0f)), P("b", type::Float, Value(0.0f))}, {P("result", type::Bool)},
         [f](GraphExec& ex) { ex.Out("result", Value(f(ex.In("a").AsFloat(), ex.In("b").AsFloat()))); });
}

void RegisterMath(NodeRegistry& r) {
    Binary(r, "add", "Add", [](float a, float b) { return a + b; }, true, {"plus", "+", "sum"});
    Binary(r, "sub", "Subtract", [](float a, float b) { return a - b; }, true, {"minus", "-"});
    Binary(r, "mul", "Multiply", [](float a, float b) { return a * b; }, true, {"times", "*", "scale"});
    Binary(r, "div", "Divide", [](float a, float b) { return b != 0.0f ? a / b : 0.0f; }, false, {"/"});
    Binary(r, "mod", "Modulo", [](float a, float b) { return b != 0.0f ? std::fmod(a, b) : 0.0f; }, true, {"%"});
    Binary(r, "min", "Min", [](float a, float b) { return std::min(a, b); }, true);
    Binary(r, "max", "Max", [](float a, float b) { return std::max(a, b); }, true);
    Binary(r, "pow", "Power", [](float a, float b) { return std::pow(a, b); }, false);
    Unary(r, "abs", "Abs", [](float x) { return std::fabs(x); });
    Unary(r, "neg", "Negate", [](float x) { return -x; });
    Unary(r, "sqrt", "Square Root", [](float x) { return x > 0.0f ? std::sqrt(x) : 0.0f; });
    Unary(r, "floor", "Floor", [](float x) { return std::floor(x); });
    Unary(r, "ceil", "Ceil", [](float x) { return std::ceil(x); });
    Unary(r, "round", "Round", [](float x) { return std::round(x); });
    Unary(r, "sin", "Sin", [](float x) { return std::sin(x); });
    Unary(r, "cos", "Cos", [](float x) { return std::cos(x); });
    Pure(r, "math.clamp", "Clamp", "Math", "Keeps a value between min and max.",
         {P("x", type::Float, Value(0.0f)), P("min", type::Float, Value(0.0f)), P("max", type::Float, Value(1.0f))},
         {P("result", type::Float)}, [](GraphExec& ex) {
             ex.Out("result", Value(std::clamp(ex.In("x").AsFloat(), ex.In("min").AsFloat(),
                                               std::max(ex.In("min").AsFloat(), ex.In("max").AsFloat()))));
         });
    Pure(r, "math.lerp", "Lerp", "Math", "Blends from a to b by t (0..1).",
         {P("a", type::Any, Value(0.0f)), P("b", type::Any, Value(1.0f)), P("t", type::Float, Value(0.5f))},
         {P("result", type::Any)}, [](GraphExec& ex) {
             const float t = ex.In("t").AsFloat();
             ex.Out("result", Arith(ex.In("a"), ex.In("b"), [t](float a, float b) { return a + (b - a) * t; }, false));
         }, {"mix", "interpolate"});
    Pure(r, "math.random", "Random", "Math", "A random number between min and max.",
         {P("min", type::Float, Value(0.0f)), P("max", type::Float, Value(1.0f))}, {P("result", type::Float)},
         [](GraphExec& ex) {
             const float a = ex.In("min").AsFloat(), b = ex.In("max").AsFloat();
             std::uniform_real_distribution<float> d(std::min(a, b), std::max(a, b));
             ex.Out("result", Value(d(Rng())));
         });
    Pure(r, "math.random_int", "Random Integer", "Math", "A random whole number from min to max inclusive.",
         {P("min", type::Int, Value(0)), P("max", type::Int, Value(10))}, {P("result", type::Int)},
         [](GraphExec& ex) {
             const int a = ex.In("min").AsInt(), b = ex.In("max").AsInt();
             std::uniform_int_distribution<int> d(std::min(a, b), std::max(a, b));
             ex.Out("result", Value(d(Rng())));
         });

    Pure(r, "vec.make3", "Make Vector3", "Math/Vectors", "A vector from three numbers.",
         {P("x", type::Float, Value(0.0f)), P("y", type::Float, Value(0.0f)), P("z", type::Float, Value(0.0f))},
         {P("vector", type::Vec3)}, [](GraphExec& ex) {
             ex.Out("vector", Value(glm::vec3(ex.In("x").AsFloat(), ex.In("y").AsFloat(), ex.In("z").AsFloat())));
         });
    Pure(r, "vec.make2", "Make Vector2", "Math/Vectors", "A vector from two numbers.",
         {P("x", type::Float, Value(0.0f)), P("y", type::Float, Value(0.0f))}, {P("vector", type::Vec2)},
         [](GraphExec& ex) { ex.Out("vector", Value(glm::vec2(ex.In("x").AsFloat(), ex.In("y").AsFloat()))); });
    Pure(r, "vec.break3", "Break Vector3", "Math/Vectors", "The three numbers of a vector.",
         {P("vector", type::Vec3, Value(glm::vec3(0.0f)))},
         {P("x", type::Float), P("y", type::Float), P("z", type::Float)}, [](GraphExec& ex) {
             const glm::vec3 v = ex.In("vector").AsVec3();
             ex.Out("x", Value(v.x));
             ex.Out("y", Value(v.y));
             ex.Out("z", Value(v.z));
         });
    Pure(r, "vec.length", "Length", "Math/Vectors", "Length of a vector.", {P("vector", type::Vec3)},
         {P("length", type::Float)}, [](GraphExec& ex) { ex.Out("length", Value(glm::length(ex.In("vector").AsVec3()))); });
    Pure(r, "vec.normalize", "Normalize", "Math/Vectors", "The same direction, length 1.", {P("vector", type::Vec3)},
         {P("vector", type::Vec3)}, [](GraphExec& ex) {
             const glm::vec3 v = ex.In("vector").AsVec3();
             const float l = glm::length(v);
             ex.Out("vector", Value(l > 1e-6f ? v / l : glm::vec3(0.0f)));
         });
    Pure(r, "vec.distance", "Distance", "Math/Vectors", "Distance between two points.",
         {P("a", type::Vec3), P("b", type::Vec3)}, {P("distance", type::Float)}, [](GraphExec& ex) {
             ex.Out("distance", Value(glm::distance(ex.In("a").AsVec3(), ex.In("b").AsVec3())));
         });
    Pure(r, "vec.direction", "Direction", "Math/Vectors", "Unit direction from one point to another.",
         {P("from", type::Vec3), P("to", type::Vec3)}, {P("direction", type::Vec3)}, [](GraphExec& ex) {
             const glm::vec3 d = ex.In("to").AsVec3() - ex.In("from").AsVec3();
             const float l = glm::length(d);
             ex.Out("direction", Value(l > 1e-6f ? d / l : glm::vec3(0.0f)));
         });
    Pure(r, "vec.dot", "Dot", "Math/Vectors", "Dot product.", {P("a", type::Vec3), P("b", type::Vec3)},
         {P("result", type::Float)},
         [](GraphExec& ex) { ex.Out("result", Value(glm::dot(ex.In("a").AsVec3(), ex.In("b").AsVec3()))); });
    Pure(r, "vec.cross", "Cross", "Math/Vectors", "Cross product.", {P("a", type::Vec3), P("b", type::Vec3)},
         {P("result", type::Vec3)},
         [](GraphExec& ex) { ex.Out("result", Value(glm::cross(ex.In("a").AsVec3(), ex.In("b").AsVec3()))); });
}

void RegisterLogic(NodeRegistry& r) {
    Pure(r, "compare.eq", "Equal", "Logic", "Whether two values are equal.", {P("a", type::Any), P("b", type::Any)},
         {P("result", type::Bool)}, [](GraphExec& ex) {
             const Value a = ex.In("a"), b = ex.In("b");
             const bool numeric = Rank(a) <= 1 && Rank(b) <= 1 && a.Type() != Kind::String && b.Type() != Kind::String;
             ex.Out("result", Value(numeric ? std::fabs(a.AsFloat() - b.AsFloat()) < 1e-6f : a == b));
         }, {"==", "same"});
    Pure(r, "compare.ne", "Not Equal", "Logic", "Whether two values differ.", {P("a", type::Any), P("b", type::Any)},
         {P("result", type::Bool)}, [](GraphExec& ex) { ex.Out("result", Value(!(ex.In("a") == ex.In("b")))); },
         {"!=", "differ"});
    Compare(r, "lt", "Less", [](float a, float b) { return a < b; });
    Compare(r, "le", "Less Or Equal", [](float a, float b) { return a <= b; });
    Compare(r, "gt", "Greater", [](float a, float b) { return a > b; });
    Compare(r, "ge", "Greater Or Equal", [](float a, float b) { return a >= b; });
    Pure(r, "logic.and", "And", "Logic", "True when both are true.", {P("a", type::Bool), P("b", type::Bool)},
         {P("result", type::Bool)}, [](GraphExec& ex) { ex.Out("result", Value(ex.In("a").AsBool() && ex.In("b").AsBool())); });
    Pure(r, "logic.or", "Or", "Logic", "True when either is true.", {P("a", type::Bool), P("b", type::Bool)},
         {P("result", type::Bool)}, [](GraphExec& ex) { ex.Out("result", Value(ex.In("a").AsBool() || ex.In("b").AsBool())); });
    Pure(r, "logic.xor", "Xor", "Logic", "True when exactly one is true.", {P("a", type::Bool), P("b", type::Bool)},
         {P("result", type::Bool)}, [](GraphExec& ex) { ex.Out("result", Value(ex.In("a").AsBool() != ex.In("b").AsBool())); });
    Pure(r, "logic.not", "Not", "Logic", "The opposite.", {P("value", type::Bool)}, {P("result", type::Bool)},
         [](GraphExec& ex) { ex.Out("result", Value(!ex.In("value").AsBool())); });

    Pure(r, "convert.to_string", "To String", "Convert", "Any value as text.", {P("value", type::Any)},
         {P("text", type::String)}, [](GraphExec& ex) { ex.Out("text", Value(ex.In("value").AsString())); });
    Pure(r, "convert.to_int", "To Integer", "Convert", "A value as a whole number.", {P("value", type::Any)},
         {P("result", type::Int)}, [](GraphExec& ex) { ex.Out("result", Value(ex.In("value").AsInt())); });
    Pure(r, "convert.to_float", "To Float", "Convert", "A value as a number.", {P("value", type::Any)},
         {P("result", type::Float)}, [](GraphExec& ex) { ex.Out("result", Value(ex.In("value").AsFloat())); });
    Pure(r, "convert.to_bool", "To Boolean", "Convert", "A value as yes/no.", {P("value", type::Any)},
         {P("result", type::Bool)}, [](GraphExec& ex) { ex.Out("result", Value(ex.In("value").AsBool())); });

    Pure(r, "string.concat", "Concatenate", "Strings", "Joins two texts.",
         {P("a", type::String, Value(std::string())), P("b", type::String, Value(std::string()))},
         {P("text", type::String)},
         [](GraphExec& ex) { ex.Out("text", Value(ex.In("a").AsString() + ex.In("b").AsString())); }, {"join", "append"});
    Pure(r, "string.contains", "Contains Text", "Strings", "Whether the text contains a part.",
         {P("text", type::String, Value(std::string())), P("part", type::String, Value(std::string()))},
         {P("result", type::Bool)}, [](GraphExec& ex) {
             ex.Out("result", Value(ex.In("text").AsString().find(ex.In("part").AsString()) != std::string::npos));
         });
    Pure(r, "string.length", "Text Length", "Strings", "Number of bytes in the text.",
         {P("text", type::String, Value(std::string()))}, {P("length", type::Int)},
         [](GraphExec& ex) { ex.Out("length", Value((int)ex.In("text").AsString().size())); });
}

void RegisterArrays(NodeRegistry& r) {
    {
        NodeType t = Node("array.make", "Make Array", "Arrays", "An array from separate values.");
        t.Pure = true;
        t.Settings = {S("count", "int", Value(2), "Items")};
        t.Outputs = {P("array", type::Array)};
        t.Pins = [](const GraphNode& n, const GraphEnv&, NodePins& p) {
            const int count = std::clamp(SettingIntOf(n, "count", 2), 0, 32);
            for (int i = 0; i < count; ++i) p.Inputs.push_back(P("item " + std::to_string(i), type::Any));
        };
        t.Run = [](GraphExec& ex) {
            std::vector<Value> items;
            for (const PinSpec& p : ex.Pins().Inputs) items.push_back(ex.In(p.Name));
            ex.Out("array", Value::Array(std::move(items)));
            return NodeResult::Go("");
        };
        t.Keywords = {"list"};
        r.Register(std::move(t));
    }
    Pure(r, "array.length", "Array Length", "Arrays", "Number of items.", {P("array", type::Array)},
         {P("length", type::Int)}, [](GraphExec& ex) { ex.Out("length", Value((int)ex.In("array").Items().size())); },
         {"count", "size"});
    Pure(r, "array.get", "Get Item", "Arrays", "The item at an index (from 0).",
         {P("array", type::Array), P("index", type::Int, Value(0))}, {P("item", type::Any), P("valid", type::Bool)},
         [](GraphExec& ex) {
             const Value a = ex.In("array");
             const int i = ex.In("index").AsInt();
             const bool ok = i >= 0 && i < (int)a.Items().size();
             ex.Out("item", ok ? a.Items()[i] : Value());
             ex.Out("valid", Value(ok));
         });
    Pure(r, "array.set", "Set Item", "Arrays", "A copy of the array with one item replaced.",
         {P("array", type::Array), P("index", type::Int, Value(0)), P("item", type::Any)}, {P("array", type::Array)},
         [](GraphExec& ex) {
             Value a = ex.In("array");
             const int i = ex.In("index").AsInt();
             if (i >= 0) {
                 std::vector<Value>& items = a.MutableItems();
                 if (i >= (int)items.size()) items.resize(i + 1);
                 items[i] = ex.In("item");
             }
             ex.Out("array", a);
         });
    Pure(r, "array.push", "Add Item", "Arrays", "A copy of the array with an item at the end.",
         {P("array", type::Array), P("item", type::Any)}, {P("array", type::Array)}, [](GraphExec& ex) {
             Value a = ex.In("array");
             a.MutableItems().push_back(ex.In("item"));
             ex.Out("array", a);
         }, {"append", "insert"});
    Pure(r, "array.remove", "Remove Item", "Arrays", "A copy of the array without the item at an index.",
         {P("array", type::Array), P("index", type::Int, Value(0))}, {P("array", type::Array)}, [](GraphExec& ex) {
             Value a = ex.In("array");
             const int i = ex.In("index").AsInt();
             if (i >= 0 && i < (int)a.Items().size()) a.MutableItems().erase(a.MutableItems().begin() + i);
             ex.Out("array", a);
         }, {"delete", "erase"});
    Pure(r, "array.find", "Find Item", "Arrays", "Index of the item, or -1.",
         {P("array", type::Array), P("item", type::Any)}, {P("index", type::Int), P("found", type::Bool)},
         [](GraphExec& ex) {
             const Value a = ex.In("array"), x = ex.In("item");
             int found = -1;
             for (size_t i = 0; i < a.Items().size(); ++i)
                 if (a.Items()[i] == x) { found = (int)i; break; }
             ex.Out("index", Value(found));
             ex.Out("found", Value(found >= 0));
         }, {"contains", "index"});
    Pure(r, "array.random", "Random Item", "Arrays", "One item picked at random.", {P("array", type::Array)},
         {P("item", type::Any), P("valid", type::Bool)}, [](GraphExec& ex) {
             const Value a = ex.In("array");
             if (a.Items().empty()) {
                 ex.Out("item", Value());
                 ex.Out("valid", Value(false));
                 return;
             }
             std::uniform_int_distribution<int> d(0, (int)a.Items().size() - 1);
             ex.Out("item", a.Items()[d(Rng())]);
             ex.Out("valid", Value(true));
         });
}

// Поля типа: из реестра (именованная структура) или пусто.
const TypeInfo* StructType(const GraphEnv& env, const GraphNode& n) {
    return env.Types ? env.Types->Find(SettingOf(n, "type")) : nullptr;
}

void RegisterStructs(NodeRegistry& r) {
    {
        NodeType t = Node("struct.make", "Make Struct", "Structs", "A structure of a type from its fields.");
        t.Pure = true;
        t.Settings = {S("type", "type", Value(std::string("transform")), "Type")};
        t.Pins = [](const GraphNode& n, const GraphEnv& env, NodePins& p) {
            const TypeInfo* info = StructType(env, n);
            const std::string id = info ? info->Id : std::string(type::Struct);
            if (info)
                for (const Param& f : info->Fields) p.Inputs.push_back(P(f.Name, TypeOfValue(env, f.Default), f.Default));
            p.Outputs.push_back(P("struct", id));
        };
        t.Run = [](GraphExec& ex) {
            const std::string type = ex.SettingText("type");
            Value s = ex.Types().Default(type);
            if (s.Type() != Kind::Struct) s = Value::Struct(type);
            for (const PinSpec& p : ex.Pins().Inputs) s.SetField(p.Name, ex.In(p.Name));
            ex.Out("struct", s);
            return NodeResult::Go("");
        };
        r.Register(std::move(t));
    }
    {
        NodeType t = Node("struct.break", "Break Struct", "Structs", "The fields of a structure.");
        t.Pure = true;
        t.Settings = {S("type", "type", Value(std::string("transform")), "Type")};
        t.Pins = [](const GraphNode& n, const GraphEnv& env, NodePins& p) {
            const TypeInfo* info = StructType(env, n);
            p.Inputs.push_back(P("struct", info ? info->Id : std::string(type::Struct)));
            if (info)
                for (const Param& f : info->Fields) p.Outputs.push_back(P(f.Name, TypeOfValue(env, f.Default), f.Default));
        };
        t.Run = [](GraphExec& ex) {
            const Value s = ex.In("struct");
            for (const PinSpec& p : ex.Pins().Outputs) {
                const Value* f = s.FindField(p.Name);
                ex.Out(p.Name, f ? *f : p.Default);
            }
            return NodeResult::Go("");
        };
        r.Register(std::move(t));
    }
    {
        NodeType t = Node("struct.get", "Get Field", "Structs", "One field of a structure by name.");
        t.Pure = true;
        t.Settings = {S("field", "string", Value(std::string()), "Field")};
        t.Inputs = {P("struct", type::Struct)};
        t.Outputs = {P("value", type::Any), P("found", type::Bool)};
        t.Run = [](GraphExec& ex) {
            const Value s = ex.In("struct");
            const Value* f = s.FindField(ex.SettingText("field"));
            ex.Out("value", f ? *f : Value());
            ex.Out("found", Value(f != nullptr));
            return NodeResult::Go("");
        };
        r.Register(std::move(t));
    }
    {
        NodeType t = Node("struct.set", "Set Field", "Structs", "A copy of the structure with one field changed.");
        t.Pure = true;
        t.Settings = {S("field", "string", Value(std::string()), "Field")};
        t.Inputs = {P("struct", type::Struct), P("value", type::Any)};
        t.Outputs = {P("struct", type::Struct)};
        t.Run = [](GraphExec& ex) {
            Value s = ex.In("struct");
            s.SetField(ex.SettingText("field"), ex.In("value"));
            ex.Out("struct", s);
            return NodeResult::Go("");
        };
        r.Register(std::move(t));
    }
}

void RegisterRotations(NodeRegistry& r) {
    const Value identity(glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    Pure(r, "rot.from_euler", "Rotation From Angles", "Math/Rotation", "A rotation from angles in degrees.",
         {P("angles", type::Vec3)}, {P("rotation", type::Rotation)},
         [](GraphExec& ex) { ex.Out("rotation", Value(glm::quat(glm::radians(ex.In("angles").AsVec3())))); });
    Pure(r, "rot.to_euler", "Rotation To Angles", "Math/Rotation", "Angles in degrees of a rotation.",
         {P("rotation", type::Rotation, identity)}, {P("angles", type::Vec3)},
         [](GraphExec& ex) { ex.Out("angles", Value(glm::degrees(glm::eulerAngles(ex.In("rotation").AsRotation())))); });
    Pure(r, "rot.rotate", "Rotate Vector", "Math/Rotation", "Turns a vector by a rotation.",
         {P("rotation", type::Rotation, identity), P("vector", type::Vec3, Value(glm::vec3(0, 0, -1)))},
         {P("vector", type::Vec3)},
         [](GraphExec& ex) { ex.Out("vector", Value(ex.In("rotation").AsRotation() * ex.In("vector").AsVec3())); });
    Pure(r, "rot.look", "Look Rotation", "Math/Rotation", "The rotation that looks along a direction.",
         {P("direction", type::Vec3, Value(glm::vec3(0, 0, -1)))}, {P("rotation", type::Rotation)},
         [](GraphExec& ex) {
             glm::vec3 d = ex.In("direction").AsVec3();
             const float l = glm::length(d);
             if (l < 1e-6f) {
                 ex.Out("rotation", Value(glm::quat(1.0f, 0.0f, 0.0f, 0.0f)));
                 return;
             }
             d /= l;
             const glm::vec3 up = std::fabs(d.y) > 0.999f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
             ex.Out("rotation", Value(glm::quatLookAt(d, up)));
         }, {"face", "towards"});
    Pure(r, "rot.combine", "Combine Rotations", "Math/Rotation", "First a, then b.",
         {P("a", type::Rotation, identity), P("b", type::Rotation, identity)}, {P("rotation", type::Rotation)},
         [](GraphExec& ex) { ex.Out("rotation", Value(ex.In("b").AsRotation() * ex.In("a").AsRotation())); });
    Pure(r, "rot.slerp", "Blend Rotations", "Math/Rotation", "Smooth blend from a to b by t.",
         {P("a", type::Rotation, identity), P("b", type::Rotation, identity), P("t", type::Float, Value(0.5f))},
         {P("rotation", type::Rotation)}, [](GraphExec& ex) {
             ex.Out("rotation", Value(glm::slerp(ex.In("a").AsRotation(), ex.In("b").AsRotation(), ex.In("t").AsFloat())));
         });
    Pure(r, "transform.point", "Transform Point", "Math/Rotation", "A local point of a transform in world space.",
         {P("transform", type::Transform), P("point", type::Vec3)}, {P("point", type::Vec3)}, [](GraphExec& ex) {
             const Value t = ex.In("transform");
             const Value* pos = t.FindField("position");
             const Value* rot = t.FindField("rotation");
             const Value* scl = t.FindField("scale");
             const glm::vec3 p = ex.In("point").AsVec3() * (scl ? scl->AsVec3() : glm::vec3(1.0f));
             ex.Out("point", Value((rot ? rot->AsRotation() : glm::quat(1, 0, 0, 0)) * p +
                                   (pos ? pos->AsVec3() : glm::vec3(0.0f))));
         });
}

} // namespace

void RegisterDataNodes(NodeRegistry& nodes) {
    RegisterMath(nodes);
    RegisterLogic(nodes);
    RegisterArrays(nodes);
    RegisterStructs(nodes);
    RegisterRotations(nodes);
}

} // namespace sage::ai::nodes
