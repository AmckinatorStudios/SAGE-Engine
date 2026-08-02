#include "ScriptEngine.h"

#include "sage/core/Log.h"
#include <algorithm>

// ---------------------------------------------------------------------------
// Математические типы и хелперы: Vec2/Vec3/Vec4, Transform, sage.math.*
//
// Часть Lua-API движка. Раньше ВСЕ привязки жили в одном ScriptEngine.cpp на
// 1800 строк: 126 функций, восемнадцать областей, и чтобы дописать одну
// строчку про анимацию, приходилось листать интерфейс, физику и таймеры.
// Определения разъехались по файлам ScriptApi_*.cpp — по файлу на область;
// объявления методов остались в ScriptEngine.h, поэтому порядок регистрации
// по-прежнему записан в одном месте (RegisterEngineApi) и не зависит от того,
// в каком файле лежит тело.
// ---------------------------------------------------------------------------

void ScriptEngine::RegisterMathTypes() {
    // glm::vec3 — доступен из Lua как обычная таблица с полями x/y/z, плюс
    // арифметика (+, -, унарный минус, умножение/деление на число) и пара
    // геометрических хелперов — без этого любая игровая математика (движение,
    // направления, дистанции) была бы мучением через отдельные x/y/z-поля.
    // ВАЖНО: sol::constructors регистрирует только Vec3.new(...) — вызов
    // Vec3(...) как функции требует ОТДЕЛЬНОЙ регистрации call_constructor
    // (без него Lua падает с «attempt to call a table value»; найдено боевым
    // тестом games/testgame — прежние скрипты векторы не конструировали).
    m_lua.new_usertype<glm::vec3>("Vec3",
        sol::constructors<glm::vec3(), glm::vec3(float, float, float)>(),
        sol::call_constructor, sol::constructors<glm::vec3(), glm::vec3(float, float, float)>(),
        "x", &glm::vec3::x,
        "y", &glm::vec3::y,
        "z", &glm::vec3::z,
        sol::meta_function::addition, [](const glm::vec3& a, const glm::vec3& b) { return a + b; },
        sol::meta_function::subtraction, [](const glm::vec3& a, const glm::vec3& b) { return a - b; },
        sol::meta_function::unary_minus, [](const glm::vec3& a) { return -a; },
        sol::meta_function::multiplication, [](const glm::vec3& a, float s) { return a * s; },
        sol::meta_function::division, [](const glm::vec3& a, float s) { return a / s; },
        sol::meta_function::to_string, [](const glm::vec3& a) {
            return "(" + std::to_string(a.x) + ", " + std::to_string(a.y) + ", " + std::to_string(a.z) + ")";
        },
        "Length", [](const glm::vec3& a) { return glm::length(a); },
        "Normalized", [](const glm::vec3& a) {
            float len = glm::length(a);
            return len > 0.0001f ? a / len : a;
        },
        "Distance", [](const glm::vec3& a, const glm::vec3& b) { return glm::length(b - a); },
        "Dot", [](const glm::vec3& a, const glm::vec3& b) { return glm::dot(a, b); },
        "Cross", [](const glm::vec3& a, const glm::vec3& b) { return glm::cross(a, b); }
    );

    // Vec2 — размеры билбордов (Size) и прочая 2D-математика (экранные
    // координаты, UV). Минимальный набор — арифметика та же, что у Vec3.
    m_lua.new_usertype<glm::vec2>("Vec2",
        sol::constructors<glm::vec2(), glm::vec2(float, float)>(),
        sol::call_constructor, sol::constructors<glm::vec2(), glm::vec2(float, float)>(),
        "x", &glm::vec2::x,
        "y", &glm::vec2::y,
        sol::meta_function::addition, [](const glm::vec2& a, const glm::vec2& b) { return a + b; },
        sol::meta_function::subtraction, [](const glm::vec2& a, const glm::vec2& b) { return a - b; },
        sol::meta_function::multiplication, [](const glm::vec2& a, float s) { return a * s; },
        sol::meta_function::to_string, [](const glm::vec2& a) {
            return "(" + std::to_string(a.x) + ", " + std::to_string(a.y) + ")";
        }
    );

    // Vec4 — цвета с альфа-каналом (частицы, тонирование билбордов). Здесь
    // намеренно нет геометрических операций (Length/Normalized/Dot) — Vec4
    // в этом движке используется только как rgba, не как направление/точка.
    m_lua.new_usertype<glm::vec4>("Vec4",
        sol::constructors<glm::vec4(), glm::vec4(float, float, float, float)>(),
        sol::call_constructor, sol::constructors<glm::vec4(), glm::vec4(float, float, float, float)>(),
        "x", &glm::vec4::x,
        "y", &glm::vec4::y,
        "z", &glm::vec4::z,
        "w", &glm::vec4::w,
        sol::meta_function::to_string, [](const glm::vec4& a) {
            return "(" + std::to_string(a.x) + ", " + std::to_string(a.y) + ", "
                 + std::to_string(a.z) + ", " + std::to_string(a.w) + ")";
        }
    );

    // Transform — позиция/поворот/масштаб объекта, доступны на чтение и запись
    m_lua.new_usertype<::Transform>("Transform",
        "Position", &::Transform::Position,
        "Rotation", &::Transform::Rotation,
        "Scale", &::Transform::Scale
    );
}

void ScriptEngine::RegisterMathHelpers() {
    // --- Математические хелперы сверх Vec-арифметики: то, чего не выразить
    // операторами. Lerp работает и для чисел, и для Vec3 (перегрузка). ---
    Bind("math", "Cross", "Cross", [](const glm::vec3& a, const glm::vec3& b) { return glm::cross(a, b); });
    Bind("math", "Radians", "Radians", [](float deg) { return glm::radians(deg); });
    Bind("math", "Degrees", "Degrees", [](float rad) { return glm::degrees(rad); });
    Bind("math", "Clamp", "Clamp", [](float x, float lo, float hi) { return glm::clamp(x, lo, hi); });
    Bind("math", "Lerp", "Lerp", sol::overload(
        [](float a, float b, float t) { return a + (b - a) * t; },
        [](const glm::vec3& a, const glm::vec3& b, float t) { return a + (b - a) * t; }
    ));
}

