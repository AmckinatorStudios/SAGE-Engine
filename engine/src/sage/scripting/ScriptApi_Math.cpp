#include "ScriptEngine.h"

#include "sage/core/Log.h"
#include "sage/scene/Transform.h"
#include <algorithm>
#include <random>
#include <glm/gtc/quaternion.hpp>

// ---------------------------------------------------------------------------
// Математические типы и хелперы: Vec2/Vec3/Vec4/Quat, Transform, sage.math.*
//
// Часть Lua-API движка. Раньше ВСЕ привязки жили в одном ScriptEngine.cpp на
// 1800 строк: 126 функций, восемнадцать областей, и чтобы дописать одну
// строчку про анимацию, приходилось листать интерфейс, физику и таймеры.
// Определения разъехались по файлам ScriptApi_*.cpp — по файлу на область;
// объявления методов остались в ScriptEngine.h, поэтому порядок регистрации
// по-прежнему записан в одном месте (RegisterEngineApi) и не зависит от того,
// в каком файле лежит тело.
// ---------------------------------------------------------------------------

namespace {
// Генератор для случайных геометрических хелперов (RandomRange, точки/
// направления на сфере и т.п.) — СВОЙ, отдельный от math.randomseed/
// math.random: тот поток живёт внутри Lua 5.4 (xoshiro256), и дотянуться до
// него из C++ нечем. Кому нужна воспроизводимая случайность здесь — придётся
// завести свой ГПСЧ на Lua; этот даёт только «хорошее случайное», не
// детерминированное между запусками.
std::mt19937& MathRng() {
    static std::mt19937 rng{std::random_device{}()};
    return rng;
}
float RandomFloat(float lo, float hi) {
    return std::uniform_real_distribution<float>(lo, hi)(MathRng());
}
} // namespace

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
        "Cross", [](const glm::vec3& a, const glm::vec3& b) { return glm::cross(a, b); },
        // Отражённое направление: свет от лампы, отскок снаряда от стены,
        // блик по нормали. normal — ЕДИНИЧНЫЙ (как и везде в движке, никто не
        // нормализует его за вызывающего — цена лишней normalize() на каждый
        // отскок сотни частиц реальна).
        "Reflect", [](const glm::vec3& dir, const glm::vec3& normal) {
            return glm::reflect(dir, normal);
        },
        // Угол между направлениями, В ГРАДУСАХ (как Rotation и math.Degrees) —
        // не радианы, чтобы не заводить молчаливую вторую единицу измерения
        // рядом с Rotation.
        "Angle", [](const glm::vec3& a, const glm::vec3& b) {
            const float la = glm::length(a), lb = glm::length(b);
            if (la < 1e-6f || lb < 1e-6f) return 0.0f;
            const float c = std::clamp(glm::dot(a, b) / (la * lb), -1.0f, 1.0f);
            return glm::degrees(std::acos(c));
        },
        // Шаг ОТ current К target не дальше maxDelta — движение с постоянной
        // скоростью к точке, которое САМО останавливается, дойдя до неё (а не
        // проскакивает и не топчется на месте дрожью в один кадр туда-сюда,
        // как топорный `current + dir*speed*dt` без проверки дистанции).
        "MoveTowards", [](const glm::vec3& current, const glm::vec3& target, float maxDelta) {
            const glm::vec3 diff = target - current;
            const float dist = glm::length(diff);
            if (dist <= maxDelta || dist < 1e-6f) return target;
            return current + diff / dist * maxDelta;
        },
        // Обрезать длину вектора сверху, направление сохранить — предел
        // скорости персонажа, руки камеры, силы отталкивания.
        "ClampLength", [](const glm::vec3& v, float maxLen) {
            const float len = glm::length(v);
            return (len > maxLen && len > 1e-6f) ? v / len * maxLen : v;
        }
    );

    // Vec2 — размеры билбордов (Size) и прочая 2D-математика (экранные
    // координаты, UV). Тот же набор, что у Vec3, минус Cross (в 2D это
    // скаляр, не вектор, — другая операция, а не недостающая перегрузка).
    m_lua.new_usertype<glm::vec2>("Vec2",
        sol::constructors<glm::vec2(), glm::vec2(float, float)>(),
        sol::call_constructor, sol::constructors<glm::vec2(), glm::vec2(float, float)>(),
        "x", &glm::vec2::x,
        "y", &glm::vec2::y,
        sol::meta_function::addition, [](const glm::vec2& a, const glm::vec2& b) { return a + b; },
        sol::meta_function::subtraction, [](const glm::vec2& a, const glm::vec2& b) { return a - b; },
        sol::meta_function::unary_minus, [](const glm::vec2& a) { return -a; },
        sol::meta_function::multiplication, [](const glm::vec2& a, float s) { return a * s; },
        sol::meta_function::division, [](const glm::vec2& a, float s) { return a / s; },
        sol::meta_function::to_string, [](const glm::vec2& a) {
            return "(" + std::to_string(a.x) + ", " + std::to_string(a.y) + ")";
        },
        "Length", [](const glm::vec2& a) { return glm::length(a); },
        "Normalized", [](const glm::vec2& a) {
            float len = glm::length(a);
            return len > 0.0001f ? a / len : a;
        },
        "Distance", [](const glm::vec2& a, const glm::vec2& b) { return glm::length(b - a); },
        "Dot", [](const glm::vec2& a, const glm::vec2& b) { return glm::dot(a, b); }
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

    // Quat — поворот БЕЗ gimbal lock и без порядка осей: там, где Rotation
    // (углы Эйлера в Transform) может дать скачок на полюсе или неочевидный
    // результат смешивания X/Y/Z, кватернион просто вращает и гладко
    // интерполируется (Slerp). Через Euler()/FromEuler() он читается в ТЕ ЖЕ
    // градусы XYZ, что и Transform.Rotation — общая формула с движком (см.
    // sage::EulerXYZDegreesFromRotationMatrix в Transform.h), поэтому
    // `entity.Transform.Rotation = q:Euler()` — законный, предсказуемый мост
    // между «посчитать поворот кватернионом» и «применить его к объекту».
    //
    // ПОЧЕМУ FromEuler СОБИРАЕТ ИМЕННО qx*qy*qz. Кватернионы, как и матрицы,
    // не коммутируют: порядок обязан совпадать с Transform::GetMatrix
    // (R = Rx*Ry*Rz), иначе один и тот же Vec3(pitch,yaw,roll) даст РАЗНЫЙ
    // фактический поворот в зависимости от того, собрала его матрица или
    // кватернион — расхождение, которое не отловить взглядом, пока кто-то не
    // сравнит Transform.Rotation и Quat:Euler() на одном и том же объекте.
    //
    // БЕЗ Quat.new(x,y,z,w) — НАМЕРЕННО. У glm::quat конструктор из четырёх
    // чисел берёт их как (w,x,y,z), а поля читаются как x,y,z,w: позиционный
    // конструктор в Lua выглядел бы как Vec4.new (x,y,z,w по порядку), но
    // тихо клал бы w первым — скрытая ловушка, которую снаружи не увидеть,
    // пока кто-то не сравнит записанное и прочитанное. Собирать кватернион
    // законными способами (Identity/FromAxisAngle/FromEuler/Slerp/умножение)
    // с тем и незачем.
    m_lua.new_usertype<glm::quat>("Quat",
        "x", &glm::quat::x,
        "y", &glm::quat::y,
        "z", &glm::quat::z,
        "w", &glm::quat::w,
        sol::meta_function::multiplication, sol::overload(
            [](const glm::quat& a, const glm::quat& b) { return a * b; },
            // Применить поворот к направлению/точке относительно начала координат.
            [](const glm::quat& q, const glm::vec3& v) { return q * v; }
        ),
        sol::meta_function::to_string, [](const glm::quat& q) {
            return "(" + std::to_string(q.x) + ", " + std::to_string(q.y) + ", "
                 + std::to_string(q.z) + ", " + std::to_string(q.w) + ")";
        },
        "Identity", []() { return glm::quat(1.0f, 0.0f, 0.0f, 0.0f); },
        // axis не обязана быть единичной — glm::angleAxis сам нормализует.
        "FromAxisAngle", [](const glm::vec3& axis, float deg) {
            const float len = glm::length(axis);
            return len > 1e-6f ? glm::angleAxis(glm::radians(deg), axis / len) : glm::quat(1, 0, 0, 0);
        },
        "FromEuler", [](const glm::vec3& degrees) {
            const glm::quat qx = glm::angleAxis(glm::radians(degrees.x), glm::vec3(1, 0, 0));
            const glm::quat qy = glm::angleAxis(glm::radians(degrees.y), glm::vec3(0, 1, 0));
            const glm::quat qz = glm::angleAxis(glm::radians(degrees.z), glm::vec3(0, 0, 1));
            return qx * qy * qz;
        },
        "Euler", [](const glm::quat& q) {
            return sage::EulerXYZDegreesFromRotationMatrix(glm::mat4_cast(q));
        },
        "Rotate", [](const glm::quat& q, const glm::vec3& v) { return q * v; },
        "Inverse", [](const glm::quat& q) { return glm::inverse(q); },
        "Normalized", [](const glm::quat& q) { return glm::normalize(q); },
        "Dot", [](const glm::quat& a, const glm::quat& b) { return glm::dot(a, b); },
        // Кратчайшая дуга между поворотами, с постоянной угловой скоростью:
        // качели камеры, сглаженный прицел, смена ориентации без рывка.
        // НЕ Lerp по компонентам — тот даёт неравномерную скорость и без
        // повторной нормализации вообще перестаёт быть поворотом.
        "Slerp", [](const glm::quat& a, const glm::quat& b, float t) { return glm::slerp(a, b, t); }
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
    // Перегружен на все три вектора и число — раньше Vec2/Vec4 просто не
    // интерполировались НИКАК (ни оператора вычитания у Vec4 нет, ни этой
    // перегрузки), и цвет частицы или экранную позицию UI приходилось сводить
    // вручную по x/y/z/w. Реализация тут не через Lua-операторы Vec4 (их
    // по-прежнему нет и не будет, см. Vec4 выше) — прямая арифметика glm.
    Bind("math", "Lerp", "Lerp", sol::overload(
        [](float a, float b, float t) { return a + (b - a) * t; },
        [](const glm::vec2& a, const glm::vec2& b, float t) { return a + (b - a) * t; },
        [](const glm::vec3& a, const glm::vec3& b, float t) { return a + (b - a) * t; },
        [](const glm::vec4& a, const glm::vec4& b, float t) { return a + (b - a) * t; }
    ));
    // 0..1 -> плавная S-образная кривая (0 в edge0, 1 в edge1, нулевая
    // производная на обоих концах) — анимация значения без рывка старта/
    // остановки там, где линейного Lerp не хватает (затухание звука по
    // дистанции, прозрачность у края тумана).
    Bind("math", "SmoothStep", "SmoothStep", [](float edge0, float edge1, float x) {
        return glm::smoothstep(edge0, edge1, x);
    });
    // Сферическая интерполяция МЕЖДУ НАПРАВЛЕНИЯМИ (не поворотами — для тех
    // Quat:Slerp выше): камера, огибающая цель по дуге, а не по прямой,
    // стрелка компаса, доворачивающая по кратчайшей дуге. Длина ведётся
    // линейно, направление — по дуге между нормализованными a и b.
    Bind("math", "Slerp", "Slerp", [](const glm::vec3& a, const glm::vec3& b, float t) -> glm::vec3 {
        const float la = glm::length(a), lb = glm::length(b);
        if (la < 1e-6f || lb < 1e-6f) return a + (b - a) * t; // вырожденный случай — линейно
        const glm::vec3 na = a / la, nb = b / lb;
        const float len = la + (lb - la) * t;
        const float cosAngle = std::clamp(glm::dot(na, nb), -1.0f, 1.0f);
        const float angle = std::acos(cosAngle);
        if (angle < 1e-5f) return na * len; // направления и так совпадают
        const float sinAngle = std::sin(angle);
        const float wa = std::sin((1.0f - t) * angle) / sinAngle;
        const float wb = std::sin(t * angle) / sinAngle;
        return (na * wa + nb * wb) * len;
    });

    // --- Случайность сверх стандартного math.random. Свой генератор (см.
    // MathRng выше) — НЕ то же самое, что math.randomseed сеет: тот управляет
    // потоком Lua 5.4 (xoshiro256), этот — своим std::mt19937. ---
    // Непрерывный диапазон: math.random(lo,hi) в стандартной Lua ждёт ЦЕЛЫЕ
    // границы и переполняется, если это не так, — здесь честный float.
    Bind("math", "RandomRange", "RandomRange", [](float lo, float hi) { return RandomFloat(lo, hi); });
    // Точка ВНУТРИ единичной сферы — равномерно по объёму (отбраковка, а не
    // наивный перевод случайных сферических углов, который сгущает точки к
    // полюсам). Самое частое применение — точка спавна частицы/предмета в
    // объёме, домноженная на нужный радиус.
    Bind("math", "RandomInsideUnitSphere", "RandomInsideUnitSphere", []() -> glm::vec3 {
        for (int i = 0; i < 32; ++i) {
            const glm::vec3 p(RandomFloat(-1.0f, 1.0f), RandomFloat(-1.0f, 1.0f), RandomFloat(-1.0f, 1.0f));
            if (glm::dot(p, p) <= 1.0f) return p;
        }
        return glm::vec3(0.0f); // 32 промахов подряд статистически не бывает — только вырожденный край
    });
    // Точка НА поверхности единичной сферы — равномерно по площади: случайное
    // направление разлёта (частицы взрыва, случайный вектор атаки ИИ).
    Bind("math", "RandomOnUnitSphere", "RandomOnUnitSphere", []() -> glm::vec3 {
        for (int i = 0; i < 32; ++i) {
            const glm::vec3 p(RandomFloat(-1.0f, 1.0f), RandomFloat(-1.0f, 1.0f), RandomFloat(-1.0f, 1.0f));
            const float lenSq = glm::dot(p, p);
            if (lenSq > 1e-6f && lenSq <= 1.0f) return p / std::sqrt(lenSq);
        }
        return glm::vec3(0.0f, 0.0f, 1.0f);
    });
    // 2D-вариант RandomInsideUnitSphere: разброс на плоскости (спавн вокруг
    // точки на земле, дрожание экрана).
    Bind("math", "RandomInsideUnitCircle", "RandomInsideUnitCircle", []() -> glm::vec2 {
        for (int i = 0; i < 32; ++i) {
            const glm::vec2 p(RandomFloat(-1.0f, 1.0f), RandomFloat(-1.0f, 1.0f));
            if (glm::dot(p, p) <= 1.0f) return p;
        }
        return glm::vec2(0.0f);
    });

    // --- Геометрия без физического мира: ray-plane/ray-sphere ЧИСТОЙ
    // математикой, а не через physics.Raycast (см. ScriptApi_Physics.cpp) —
    // тому нужны настоящие коллайдеры на сцене. Клик мышью в плоскость земли,
    // прицел по невидимой сфере-триггеру — geometry без единого физ. тела.
    // dir ОБЯЗАН быть единичным, как и во всех лучах движка — normal тоже. ---
    Bind("math", "IntersectRayPlane", "IntersectRayPlane",
         [](const glm::vec3& origin, const glm::vec3& dir, const glm::vec3& planePoint,
            const glm::vec3& planeNormal) -> sol::optional<glm::vec3> {
             const float denom = glm::dot(dir, planeNormal);
             if (std::abs(denom) < 1e-6f) return sol::nullopt; // луч вдоль плоскости — точки нет
             const float t = glm::dot(planePoint - origin, planeNormal) / denom;
             if (t < 0.0f) return sol::nullopt; // плоскость позади начала луча
             return origin + dir * t;
         });
    Bind("math", "IntersectRaySphere", "IntersectRaySphere",
         [](const glm::vec3& origin, const glm::vec3& dir, const glm::vec3& center,
            float radius) -> sol::optional<glm::vec3> {
             const glm::vec3 oc = origin - center;
             const float b = glm::dot(oc, dir);
             const float c = glm::dot(oc, oc) - radius * radius;
             const float disc = b * b - c;
             if (disc < 0.0f) return sol::nullopt; // луч проходит мимо
             const float sq = std::sqrt(disc);
             float t = -b - sq;                    // ближняя точка пересечения
             if (t < 0.0f) t = -b + sq;             // начало луча внутри сферы — берём дальнюю
             if (t < 0.0f) return sol::nullopt;     // сфера целиком позади начала луча
             return origin + dir * t;
         });
}

