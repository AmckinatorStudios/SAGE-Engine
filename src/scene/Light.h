#pragma once
#include <glm/glm.hpp>
#include <vector>

// Направленный свет — "солнце". На сцену обычно один: светит из бесконечности
// в одном направлении, не затухает с расстоянием. Используется и для
// вокселей, и для обычных объектов, и для воды — единый источник правды
// о том, "куда светит день".
struct DirectionalLight {
    glm::vec3 Direction{-0.4f, -1.0f, -0.3f}; // направление, КУДА летит свет (не откуда)
    glm::vec3 Color{1.0f, 0.95f, 0.85f};
    float Intensity = 1.0f;
};

// Точечный источник света — фонарь, факел, лампа. Затухает с расстоянием
// по классической формуле 1/(constant + linear*d + quadratic*d^2).
// Constant/Linear/Quadratic выводятся из Range, чтобы дизайнеру уровня
// не приходилось подбирать три магических числа — только "как далеко светит".
struct PointLight {
    glm::vec3 Position{0.0f};
    glm::vec3 Color{1.0f};
    float Intensity = 1.0f;
    float Range = 12.0f;

    float Constant() const { return 1.0f; }
    float Linear() const { return 4.5f / Range; }
    float Quadratic() const { return 75.0f / (Range * Range); }
};

// Полное описание освещения сцены: фоновая засветка (ambient) + солнце +
// произвольное число точечных источников (до MaxPointLights одновременно
// видимых шейдеру — остальные игнорируются, этого достаточно для одного
// уровня вроде "The Boat").
//
// Ambient — полусферический (hemisphere ambient), а не плоский цвет:
// верхние грани (нормаль смотрит вверх) освещаются SkyColor, нижние —
// GroundColor, между ними — плавный переход по вертикальной компоненте
// нормали. Так небо реально подсвечивает верх объектов холодным цветом,
// а низ — теплее (отражённый свет воды/палубы), вместо одинаковой засветки
// со всех сторон, которая делает объекты плоскими на вид.
//
// AmbientColor/AmbientStrength оставлены для обратной совместимости со
// старыми сохранёнными сценами (.sage) и просто задают Sky/Ground разом,
// если кто-то предпочитает старый плоский ambient — см. SetFlatAmbient().
struct LightingEnvironment {
    static constexpr int MaxPointLights = 8;

    glm::vec3 SkyColor{0.55f, 0.65f, 0.85f};    // засветка верхних граней (свет неба)
    glm::vec3 GroundColor{0.20f, 0.18f, 0.16f}; // засветка нижних граней (отражённый свет)
    float AmbientStrength = 0.3f;

    DirectionalLight Sun;

    std::vector<PointLight> PointLights;

    // Обратная совместимость / удобный шорткат: выставляет Sky и Ground
    // в один и тот же цвет — эквивалент старого плоского ambient.
    void SetFlatAmbient(const glm::vec3& color, float strength) {
        SkyColor = color;
        GroundColor = color;
        AmbientStrength = strength;
    }

    // Старые геттеры для кода/сериализации, который ещё думает в терминах
    // единого AmbientColor (например, .sage файлы, сохранённые до этого апдейта)
    glm::vec3 AmbientColorApprox() const { return (SkyColor + GroundColor) * 0.5f; }
};
