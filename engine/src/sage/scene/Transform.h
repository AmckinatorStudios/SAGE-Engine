#pragma once
#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

struct Transform {
    glm::vec3 Position{0.0f};
    glm::vec3 Rotation{0.0f}; // в градусах, углы Эйлера
    glm::vec3 Scale{1.0f};

    // Матрица объекта: T * Rx * Ry * Rz * S.
    //
    // СОБРАНА ВРУЧНУЮ, и это не микрооптимизация ради красоты. Это самая
    // горячая функция движка: её зовут на каждую сущность в каждом проходе
    // кадра (тени по каскадам, шесть граней куба отражений, зеркальный проход,
    // сам кадр), плюс физика и скрипты. На сцене в двадцать тысяч объектов
    // разница видна в миллисекундах кадра, а не в наносекундах замера.
    //
    // Что было: четыре вызова glm (translate + три rotate) + scale. Каждый
    // rotate — это НОРМАЛИЗАЦИЯ оси (которая здесь всегда единичный орт), пара
    // sin/cos и полное перемножение матриц 4x4, то есть шестьдесят четыре
    // умножения ради поворота вокруг одной оси. Итого около двух сотен операций
    // и три нормализации там, где хватает трёх пар sin/cos и десятка умножений.
    //
    // Формула развёрнута из того же произведения, что стояло здесь раньше, и
    // порядок множителей сохранён в точности — иначе сменилось бы соглашение об
    // углах Эйлера, а на нём стоят и сцены, и гизмо редактора, и скрипты.
    // Совпадение со старым путём проверяется тестом (Math_transform_matrix_*).
    glm::mat4 GetMatrix() const {
        constexpr float kDeg2Rad = 0.01745329251994329577f;
        const float rx = Rotation.x * kDeg2Rad;
        const float ry = Rotation.y * kDeg2Rad;
        const float rz = Rotation.z * kDeg2Rad;
        const float cx = std::cos(rx), sx = std::sin(rx);
        const float cy = std::cos(ry), sy = std::sin(ry);
        const float cz = std::cos(rz), sz = std::sin(rz);

        // Строки поворота R = Rx*Ry*Rz. glm хранит по столбцам (m[col][row]),
        // поэтому ниже элементы разложены транспонированно.
        glm::mat4 m;
        m[0][0] = (cy * cz) * Scale.x;
        m[0][1] = (cx * sz + sx * sy * cz) * Scale.x;
        m[0][2] = (sx * sz - cx * sy * cz) * Scale.x;
        m[0][3] = 0.0f;

        m[1][0] = (-cy * sz) * Scale.y;
        m[1][1] = (cx * cz - sx * sy * sz) * Scale.y;
        m[1][2] = (sx * cz + cx * sy * sz) * Scale.y;
        m[1][3] = 0.0f;

        m[2][0] = (sy) * Scale.z;
        m[2][1] = (-sx * cy) * Scale.z;
        m[2][2] = (cx * cy) * Scale.z;
        m[2][3] = 0.0f;

        m[3][0] = Position.x;
        m[3][1] = Position.y;
        m[3][2] = Position.z;
        m[3][3] = 1.0f;
        return m;
    }
};
