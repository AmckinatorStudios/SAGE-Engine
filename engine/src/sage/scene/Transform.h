#pragma once
#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace sage {

// ---------------------------------------------------------------------------
// СОГЛАШЕНИЕ ОБ УГЛАХ ЭЙЛЕРА: R = Ry * Rx * Rz (градусы).
//
// Рыскание (Y) — вокруг МИРОВОЙ вертикали, наклон (X) — вокруг СОБСТВЕННОЙ оси
// объекта уже после рыскания, крен (Z) — вокруг его взгляда. Так устроены
// Unity и любой контроллер от первого лица: «повернуть голову и посмотреть
// вниз» — это ровно (наклон, рыскание, 0), и наклон не заваливает горизонт.
//
// Раньше было R = Rx*Ry*Rz: наклон применялся ПОСЛЕДНИМ, то есть вокруг
// ГЛОБАЛЬНОЙ X. Камера, повёрнутая на 90° по Y и наклонённая по X, вместо
// «посмотреть вниз» заваливалась набок, а тело персонажа с камерой на себе
// клонилось вместе со взглядом. Старые сцены переводятся миграцией формата
// (v16 -> v17, EulerDegreesFromLegacyXYZ): тот же поворот, новые числа.
//
// ВСЕ преобразования «углы <-> матрица/кватернион» движка — здесь. Своя копия
// где-то ещё однажды разошлась бы по порядку осей, и объект прыгал бы при
// переходе из одной системы в другую (физика, гизмо, скрипты).
// ---------------------------------------------------------------------------
inline glm::mat4 RotationMatrixFromEulerDegrees(const glm::vec3& e) {
    constexpr float kDeg2Rad = 0.01745329251994329577f;
    const float cx = std::cos(e.x * kDeg2Rad), sx = std::sin(e.x * kDeg2Rad);
    const float cy = std::cos(e.y * kDeg2Rad), sy = std::sin(e.y * kDeg2Rad);
    const float cz = std::cos(e.z * kDeg2Rad), sz = std::sin(e.z * kDeg2Rad);
    // Строки R = Ry*Rx*Rz; glm хранит по столбцам (m[col][row]).
    glm::mat4 m(1.0f);
    m[0][0] = cy * cz + sy * sx * sz;
    m[0][1] = cx * sz;
    m[0][2] = -sy * cz + cy * sx * sz;
    m[1][0] = -cy * sz + sy * sx * cz;
    m[1][1] = cx * cz;
    m[1][2] = sy * sz + cy * sx * cz;
    m[2][0] = sy * cx;
    m[2][1] = -sx;
    m[2][2] = cy * cx;
    return m;
}

// Углы (градусы) из ЧИСТОЙ матрицы поворота — столбцы единичные, масштаб уже
// снят. Обратное RotationMatrixFromEulerDegrees.
inline glm::vec3 EulerDegreesFromRotationMatrix(const glm::mat4& rot) {
    constexpr float kRad2Deg = 57.29577951308232087680f;
    const float sx = std::min(std::max(-rot[2][1], -1.0f), 1.0f);
    const float cx = std::sqrt(std::max(0.0f, 1.0f - sx * sx));
    float rx = std::asin(sx), ry, rz;
    if (cx > 1e-6f) {
        ry = std::atan2(rot[2][0], rot[2][2]);
        rz = std::atan2(rot[0][1], rot[1][1]);
    } else {
        // Взгляд строго вверх или вниз: рыскание и крен становятся одним и тем
        // же движением (gimbal lock). Отдаём всё рысканию — горизонт не валится.
        ry = std::atan2(-rot[0][2], rot[0][0]);
        rz = 0.0f;
    }
    return glm::vec3(rx, ry, rz) * kRad2Deg;
}

inline glm::quat QuatFromEulerDegrees(const glm::vec3& e) {
    return glm::normalize(glm::quat_cast(RotationMatrixFromEulerDegrees(e)));
}
inline glm::vec3 EulerDegreesFromQuat(const glm::quat& q) {
    return EulerDegreesFromRotationMatrix(glm::mat4_cast(glm::normalize(q)));
}

// Углы прежнего соглашения (R = Rx*Ry*Rz) -> углы нынешнего, ТОТ ЖЕ поворот.
// Только для миграции старых файлов.
inline glm::vec3 EulerDegreesFromLegacyXYZ(const glm::vec3& legacy) {
    constexpr float kDeg2Rad = 0.01745329251994329577f;
    const float cx = std::cos(legacy.x * kDeg2Rad), sx = std::sin(legacy.x * kDeg2Rad);
    const float cy = std::cos(legacy.y * kDeg2Rad), sy = std::sin(legacy.y * kDeg2Rad);
    const float cz = std::cos(legacy.z * kDeg2Rad), sz = std::sin(legacy.z * kDeg2Rad);
    glm::mat4 m(1.0f);
    m[0][0] = cy * cz;
    m[0][1] = cx * sz + sx * sy * cz;
    m[0][2] = sx * sz - cx * sy * cz;
    m[1][0] = -cy * sz;
    m[1][1] = cx * cz - sx * sy * sz;
    m[1][2] = sx * cz + cx * sy * sz;
    m[2][0] = sy;
    m[2][1] = -sx * cy;
    m[2][2] = cx * cy;
    return EulerDegreesFromRotationMatrix(m);
}
// Из равнозначных наборов углов того же поворота — ближайший к ref.
//
// Один поворот записывается многими тройками: (x, y, z) и (180−x, y+180, z+180)
// дают одну матрицу, а любой угол можно сдвинуть на 360°. Анимация между двумя
// ключами интерполирует ЧИСЛА, поэтому ключи, переведённые независимо, могли
// бы оказаться «по разные стороны» — и объект провернулся бы через полный
// оборот там, где в исходнике было движение на пару градусов. Миграции
// анимаций ставят каждый ключ ближе всего к предыдущему.
inline glm::vec3 ClosestEquivalentEuler(const glm::vec3& e, const glm::vec3& ref) {
    auto unwrap = [](float a, float r) {
        while (a - r > 180.0f) a -= 360.0f;
        while (a - r < -180.0f) a += 360.0f;
        return a;
    };
    auto closest = [&](const glm::vec3& c) {
        return glm::vec3(unwrap(c.x, ref.x), unwrap(c.y, ref.y), unwrap(c.z, ref.z));
    };
    const glm::vec3 a = closest(e);
    const glm::vec3 b = closest(glm::vec3(180.0f - e.x, e.y + 180.0f, e.z + 180.0f));
    const glm::vec3 da = glm::abs(a - ref), db = glm::abs(b - ref);
    return (da.x + da.y + da.z) <= (db.x + db.y + db.z) ? a : b;
}
} // namespace sage

struct Transform {
    glm::vec3 Position{0.0f};
    glm::vec3 Rotation{0.0f}; // в градусах, углы Эйлера
    glm::vec3 Scale{1.0f};

    // Матрица объекта: T * Ry * Rx * Rz * S (соглашение об углах — выше).
    //
    // Самая горячая функция движка: её зовут на каждую сущность в каждом
    // проходе кадра, плюс физика и скрипты, поэтому поворот собирается
    // формулой (RotationMatrixFromEulerDegrees), а не тремя glm::rotate с
    // нормализацией осей и перемножением матриц 4x4.
    glm::mat4 GetMatrix() const {
        glm::mat4 m = sage::RotationMatrixFromEulerDegrees(Rotation);
        m[0] *= Scale.x;
        m[1] *= Scale.y;
        m[2] *= Scale.z;
        m[3] = glm::vec4(Position, 1.0f);
        return m;
    }

    // Обратное GetMatrix: матрица -> Position/Rotation/Scale.
    //
    // ЗАЧЕМ ЭТО В ДВИЖКЕ, А НЕ В РЕДАКТОРЕ. Разложить матрицу обратно нужно
    // всюду, где положение приходит НЕ из полей: гизмо тянет мировую матрицу,
    // управление камерой от её лица ставит объект по базису вида, физика
    // возвращает тело после симуляции. Порядок углов обязан совпадать с
    // GetMatrix (T*Ry*Rx*Rz*S) — разложение «вообще» (например, декомпозиция
    // ImGuizmo, у неё другой порядок) даёт ДРУГИЕ углы, и объект прыгает при
    // первом же касании. Один раз здесь, рядом со сборкой матрицы, — и спорить
    // двум реализациям не о чем.
    void SetFromMatrix(const glm::mat4& m) {
        Position = glm::vec3(m[3]);

        glm::vec3 scale(glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])),
                        glm::length(glm::vec3(m[2])));
        scale = glm::max(scale, glm::vec3(1e-6f));   // защита от вырожденного масштаба
        Scale = scale;

        glm::mat4 rot(1.0f);
        rot[0] = glm::vec4(glm::vec3(m[0]) / scale.x, 0.0f);
        rot[1] = glm::vec4(glm::vec3(m[1]) / scale.y, 0.0f);
        rot[2] = glm::vec4(glm::vec3(m[2]) / scale.z, 0.0f);

        // Ровно то разложение, из которого GetMatrix соберёт ту же матрицу обратно.
        Rotation = sage::EulerDegreesFromRotationMatrix(rot);
    }
};
