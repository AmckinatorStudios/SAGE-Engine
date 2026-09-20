#include "sage/scripting/lua/LuaInternal.h"

#include <algorithm>
#include <cmath>
#include <functional>

// Кватернионные помощники GLM помечены экспериментальными — включаем явно, как
// это уже делают другие файлы движка, работающие с поворотами.
#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif

#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

// ---------------------------------------------------------------------------
// МАТЕМАТИКА ДЛЯ СКРИПТА: Vector2, Vector3, Quaternion, Color, Math.
//
// ТИП ОДИН, ИМЁН ДВА. Значения — это glm::vec2/vec3/vec4, те же самые, что
// ходят по движку; `Vector3` — это НЕ второй тип, а таблица с конструктором и
// статическими функциями поверх него. Завести свой «скриптовый вектор»
// означало бы переводить туда-сюда на каждой границе и однажды перепутать
// порядок компонент в одном из переводов — молча.
//
// Имена `Vec3` (прежние) и `Vector3` (нынешние) указывают на ОДНО И ТО ЖЕ:
// скрипт, написанный вчера, продолжает работать, а новый читается как в любом
// современном движке.
// ---------------------------------------------------------------------------
namespace sage::scripting::lua {

namespace {

glm::vec3 Normalize(const glm::vec3& v) {
    const float len = glm::length(v);
    return len > 1e-6f ? v / len : glm::vec3(0.0f);
}

// Таблица-«тип»: вызывается как конструктор, хранит статические функции.
sol::table MakeTypeTable(sol::state& lua, const std::function<sol::object(sol::variadic_args)>& ctor) {
    sol::table t = lua.create_table();
    sol::table mt = lua.create_table();
    mt[sol::meta_function::call] = [ctor](sol::table, sol::variadic_args args) { return ctor(args); };
    t[sol::metatable_key] = mt;
    return t;
}

float Arg(sol::variadic_args& args, size_t index, float fallback = 0.0f) {
    if (index >= args.size()) return fallback;
    sol::object o = args[index];
    return o.is<float>() ? o.as<float>() : fallback;
}

} // namespace

void RegisterMath(Backend& backend) {
    sol::state& lua = backend.Lua();

    // Базовые usertype'ы регистрируются, только если их ещё нет: в режиме
    // совместимости их уже завёл прежний API, и вторая регистрация того же
    // типа заменила бы его метатаблицу — вместе со всей арифметикой, на
    // которой стоят существующие игры.
    if (!lua["Vec3"].valid()) {
        lua.new_usertype<glm::vec3>("Vec3",
            sol::constructors<glm::vec3(), glm::vec3(float, float, float)>(),
            sol::call_constructor, sol::constructors<glm::vec3(), glm::vec3(float, float, float)>(),
            "x", &glm::vec3::x, "y", &glm::vec3::y, "z", &glm::vec3::z,
            sol::meta_function::addition, [](const glm::vec3& a, const glm::vec3& b) { return a + b; },
            sol::meta_function::subtraction, [](const glm::vec3& a, const glm::vec3& b) { return a - b; },
            sol::meta_function::unary_minus, [](const glm::vec3& a) { return -a; },
            sol::meta_function::multiplication, [](const glm::vec3& a, float s) { return a * s; },
            sol::meta_function::division, [](const glm::vec3& a, float s) { return a / s; },
            "Length", [](const glm::vec3& a) { return glm::length(a); },
            "Normalized", [](const glm::vec3& a) { return Normalize(a); });
    }
    if (!lua["Vec2"].valid()) {
        lua.new_usertype<glm::vec2>("Vec2",
            sol::constructors<glm::vec2(), glm::vec2(float, float)>(),
            sol::call_constructor, sol::constructors<glm::vec2(), glm::vec2(float, float)>(),
            "x", &glm::vec2::x, "y", &glm::vec2::y,
            sol::meta_function::addition, [](const glm::vec2& a, const glm::vec2& b) { return a + b; },
            sol::meta_function::subtraction, [](const glm::vec2& a, const glm::vec2& b) { return a - b; },
            sol::meta_function::multiplication, [](const glm::vec2& a, float s) { return a * s; },
            "Length", [](const glm::vec2& a) { return glm::length(a); });
    }
    if (!lua["Vec4"].valid()) {
        lua.new_usertype<glm::vec4>("Vec4",
            sol::constructors<glm::vec4(), glm::vec4(float, float, float, float)>(),
            sol::call_constructor,
            sol::constructors<glm::vec4(), glm::vec4(float, float, float, float)>(),
            "x", &glm::vec4::x, "y", &glm::vec4::y, "z", &glm::vec4::z, "w", &glm::vec4::w);
    }
    if (!lua["Quat"].valid()) {
        // Кватернион — ПОВОРОТ БЕЗ ГРАНИЦ. Углы Эйлера, которыми живёт
        // Transform, читаются человеком, но складываются неправильно: два
        // поворота подряд через них дают не тот результат, а на взгляде строго
        // вверх теряют одну ось целиком (gimbal lock). Для «повернуть на
        // столько-то от текущего» нужен именно он.
        lua.new_usertype<glm::quat>("Quat",
            sol::constructors<glm::quat()>(),
            "x", &glm::quat::x, "y", &glm::quat::y, "z", &glm::quat::z, "w", &glm::quat::w,
            sol::meta_function::multiplication,
            sol::overload([](const glm::quat& a, const glm::quat& b) { return a * b; },
                          [](const glm::quat& q, const glm::vec3& v) { return q * v; }));
    }

    // --- Vector3 -------------------------------------------------------------
    sol::table v3 = MakeTypeTable(lua, [&lua](sol::variadic_args args) {
        sol::variadic_args a = args;
        return sol::make_object(lua, glm::vec3(Arg(a, 0), Arg(a, 1), Arg(a, 2)));
    });
    v3["Dot"] = [](const glm::vec3& a, const glm::vec3& b) { return glm::dot(a, b); };
    v3["Cross"] = [](const glm::vec3& a, const glm::vec3& b) { return glm::cross(a, b); };
    v3["Normalize"] = [](const glm::vec3& a) { return Normalize(a); };
    v3["Length"] = [](const glm::vec3& a) { return glm::length(a); };
    v3["Distance"] = [](const glm::vec3& a, const glm::vec3& b) { return glm::length(b - a); };
    v3["Lerp"] = [](const glm::vec3& a, const glm::vec3& b, float t) {
        return a + (b - a) * std::clamp(t, 0.0f, 1.0f);
    };
    // Шаг К цели не дальше maxDelta: движение с постоянной скоростью, которое
    // САМО останавливается, дойдя, а не проскакивает и не дрожит туда-сюда.
    v3["MoveTowards"] = [](const glm::vec3& from, const glm::vec3& to, float maxDelta) {
        const glm::vec3 d = to - from;
        const float len = glm::length(d);
        if (len <= maxDelta || len < 1e-6f) return to;
        return from + d / len * maxDelta;
    };
    v3["Reflect"] = [](const glm::vec3& dir, const glm::vec3& normal) {
        return glm::reflect(dir, normal);
    };
    v3["Angle"] = [](const glm::vec3& a, const glm::vec3& b) {
        const float la = glm::length(a), lb = glm::length(b);
        if (la < 1e-6f || lb < 1e-6f) return 0.0f;
        return glm::degrees(std::acos(std::clamp(glm::dot(a, b) / (la * lb), -1.0f, 1.0f)));
    };
    v3["zero"] = glm::vec3(0.0f);
    v3["one"] = glm::vec3(1.0f);
    v3["up"] = glm::vec3(0.0f, 1.0f, 0.0f);
    v3["right"] = glm::vec3(1.0f, 0.0f, 0.0f);
    v3["forward"] = glm::vec3(0.0f, 0.0f, 1.0f);
    lua["Vector3"] = v3;

    // --- Vector2 -------------------------------------------------------------
    sol::table v2 = MakeTypeTable(lua, [&lua](sol::variadic_args args) {
        sol::variadic_args a = args;
        return sol::make_object(lua, glm::vec2(Arg(a, 0), Arg(a, 1)));
    });
    v2["Dot"] = [](const glm::vec2& a, const glm::vec2& b) { return glm::dot(a, b); };
    v2["Length"] = [](const glm::vec2& a) { return glm::length(a); };
    v2["Distance"] = [](const glm::vec2& a, const glm::vec2& b) { return glm::length(b - a); };
    v2["Normalize"] = [](const glm::vec2& a) {
        const float len = glm::length(a);
        return len > 1e-6f ? a / len : glm::vec2(0.0f);
    };
    v2["Lerp"] = [](const glm::vec2& a, const glm::vec2& b, float t) {
        return a + (b - a) * std::clamp(t, 0.0f, 1.0f);
    };
    v2["zero"] = glm::vec2(0.0f);
    v2["one"] = glm::vec2(1.0f);
    lua["Vector2"] = v2;

    // --- Color ---------------------------------------------------------------
    // Цвет — это vec4 (rgba), а не третий вид четвёрки: в инспекторе он
    // показывается палитрой, но в математике ведёт себя как обычный вектор.
    sol::table color = MakeTypeTable(lua, [&lua](sol::variadic_args args) {
        sol::variadic_args a = args;
        return sol::make_object(lua, glm::vec4(Arg(a, 0), Arg(a, 1), Arg(a, 2), Arg(a, 3, 1.0f)));
    });
    color["Lerp"] = [](const glm::vec4& a, const glm::vec4& b, float t) {
        return a + (b - a) * std::clamp(t, 0.0f, 1.0f);
    };
    color["white"] = glm::vec4(1.0f);
    color["black"] = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    color["red"] = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
    color["green"] = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f);
    color["blue"] = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
    lua["Color"] = color;

    // --- Quaternion ----------------------------------------------------------
    sol::table quat = MakeTypeTable(lua, [&lua](sol::variadic_args) {
        return sol::make_object(lua, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    });
    quat["Identity"] = []() { return glm::quat(1.0f, 0.0f, 0.0f, 0.0f); };
    // Углы — В ГРАДУСАХ, как Transform.rotation: вторая единица измерения рядом
    // с первой означает, что однажды их перепутают.
    quat["Euler"] = [](float x, float y, float z) {
        return glm::quat(glm::radians(glm::vec3(x, y, z)));
    };
    quat["ToEuler"] = [](const glm::quat& q) { return glm::degrees(glm::eulerAngles(q)); };
    quat["Slerp"] = [](const glm::quat& a, const glm::quat& b, float t) {
        return glm::slerp(a, b, std::clamp(t, 0.0f, 1.0f));
    };
    quat["LookRotation"] = [](const glm::vec3& forward, sol::optional<glm::vec3> up) {
        const glm::vec3 f = Normalize(forward);
        if (glm::length(f) < 1e-6f) return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        return glm::quatLookAt(f, up ? *up : glm::vec3(0.0f, 1.0f, 0.0f));
    };
    lua["Quaternion"] = quat;

    // --- Math ----------------------------------------------------------------
    // Дописываем в стандартную таблицу math, а не заводим свою: две таблицы с
    // математикой — это вопрос «а в какой из них Clamp» на каждой строке.
    sol::table m = lua["math"];
    if (!m.valid()) {
        m = lua.create_table();
        lua["math"] = m;
    }
    auto add = [&m](const char* name, auto fn) {
        if (!m[name].valid()) m.set_function(name, fn);
    };
    add("Clamp", [](float v, float lo, float hi) { return std::clamp(v, lo, hi); });
    add("Lerp", [](float a, float b, float t) { return a + (b - a) * std::clamp(t, 0.0f, 1.0f); });
    add("Sign", [](float v) { return v > 0.0f ? 1.0f : (v < 0.0f ? -1.0f : 0.0f); });
    add("Radians", [](float deg) { return glm::radians(deg); });
    add("Degrees", [](float rad) { return glm::degrees(rad); });
    lua["Math"] = m; // то же самое под именем раздела (см. §21 «организация API»)
}

} // namespace sage::scripting::lua
