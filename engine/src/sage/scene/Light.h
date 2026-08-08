#pragma once
#include <glm/glm.hpp>
#include <glm/trigonometric.hpp>
#include <string>
#include <vector>

// Направленный свет — "солнце". На сцену обычно один: светит из бесконечности
// в одном направлении, не затухает с расстоянием. Используется и для
// вокселей, и для обычных объектов, и для воды — единый источник правды
// о том, "куда светит день".
struct DirectionalLight {
    glm::vec3 Direction{-0.4f, -1.0f, -0.3f}; // направление, КУДА летит свет (не откуда)
    glm::vec3 Color{1.0f, 0.95f, 0.85f};
    float Intensity = 1.0f;
    bool CastShadows = true; // каскадные карты (см. ShadowMap.h)
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
    // Просит ли источник тень. ПРОСИТ, а не получает: мест в атласе конечное
    // число (см. render/ShadowAtlas.h), и кто их занял, решает распределитель.
    bool CastShadows = true;
    // Занятое место в атласе теней, -1 — тени нет. Заполняется покадрово при
    // раздаче мест и НЕ сериализуется: это состояние кадра, а не свойство
    // источника. Держится здесь, потому что шейдеру нужно знать номер места
    // ровно для того источника, чей вклад он считает, — а связывает их индекс
    // в этом же массиве.
    int ShadowSlot = -1;

    float Constant() const { return 1.0f; }
    float Linear() const { return 4.5f / Range; }
    float Quadratic() const { return 75.0f / (Range * Range); }
};

// Прожектор (spot) — точечный источник, светящий КОНУСОМ вдоль Direction:
// фонарик, лампа-спот, сценический прожектор. Внутри внутреннего угла
// (InnerAngle) светит на полную, между внутренним и внешним (OuterAngle)
// плавно гаснет, за внешним — темно. Затухание с расстоянием — как у точечного
// (из Range). Направление обычно берётся из поворота сущности (см. LightSystem).
struct SpotLight {
    glm::vec3 Position{0.0f};
    glm::vec3 Direction{0.0f, -1.0f, 0.0f}; // КУДА светит конус
    glm::vec3 Color{1.0f};
    float Intensity = 1.0f;
    float Range = 12.0f;
    float InnerAngleDeg = 20.0f; // полная яркость внутри этого полуугла
    float OuterAngleDeg = 30.0f; // край конуса (яркость -> 0)
    bool CastShadows = true;     // см. PointLight::CastShadows
    int ShadowSlot = -1;         // место в атласе теней кадра, -1 — нет

    float Constant() const { return 1.0f; }
    float Linear() const { return 4.5f / Range; }
    float Quadratic() const { return 75.0f / (Range * Range); }
    // Косинусы углов — шейдер сравнивает через dot(), а не через arccos.
    float CosInner() const { return glm::cos(glm::radians(InnerAngleDeg)); }
    float CosOuter() const { return glm::cos(glm::radians(OuterAngleDeg)); }
};

// Полное описание освещения сцены: фоновая засветка (ambient) + солнце +
// произвольное число точечных источников (до MaxPointLights одновременно
// видимых шейдеру — остальные игнорируются, этого достаточно для одного
// уровня среднего размера).
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
// Линейный туман: фрагменты дальше Start плавно уходят в Color к End. Часть
// атмосферы сцены (сериализуется вместе с окружением).
struct FogSettings {
    bool Enabled = false;
    glm::vec3 Color{0.55f, 0.62f, 0.72f};
    float Start = 12.0f;
    float End = 60.0f;
};

// Небо сцены. Два режима, выбор — по наличию CubemapDir:
//
//   • процедурный градиент — переход от TopColor (зенит) к HorizonColor
//     (горизонт) по вертикали луча взгляда, без единого ассета;
//   • кубическая текстура — набор из шести граней px/nx/py/ny/pz/nz в одном
//     каталоге (см. Skybox::LoadFromDirectory).
//
// Enabled == false — фон просто очищается сплошным цветом.
struct SkyboxSettings {
    bool Enabled = false;
    glm::vec3 TopColor{0.30f, 0.45f, 0.75f};
    glm::vec3 HorizonColor{0.70f, 0.80f, 0.92f};

    // Каталог с гранями кубической текстуры. Пусто — процедурный градиент.
    std::string CubemapDir;
    float Intensity = 1.0f;    // яркость неба (экспозиция окружения)
    float RotationDeg = 0.0f;  // поворот вокруг вертикали: развернуть солнце набора под сцену

    // --- Светила на процедурном небе ----------------------------------------
    //
    // Небо было просто градиентом: солнце в сцене светило, но НА НЕБЕ его не
    // было — источник света без источника. С Celestials на градиенте появляются
    // диск солнца по направлению солнца сцены, луна противоходом и звёзды,
    // проступающие по мере его захода. Направление не дублируется: небо берёт
    // его у того же DirectionalLight, который освещает мир, поэтому тени и
    // положение солнца не могут разойтись.
    //
    // Выключено по умолчанию: сцена, собранная до этого, не должна вдруг
    // обзавестись солнцем там, где автор его не ставил.
    bool Celestials = false;
    glm::vec3 SunColor{1.00f, 0.95f, 0.85f};
    float SunSize = 0.045f;      // угловой радиус диска, радианы
    bool Moon = true;
    glm::vec3 MoonColor{0.78f, 0.82f, 0.95f};
    float MoonSize = 0.030f;
    float StarIntensity = 1.0f;  // 0 — без звёзд

    bool HasCubemap() const { return !CubemapDir.empty(); }
};

struct LightingEnvironment {
    static constexpr int MaxPointLights = 8;
    static constexpr int MaxSpotLights = 8;

    glm::vec3 SkyColor{0.55f, 0.65f, 0.85f};    // засветка верхних граней (свет неба)
    glm::vec3 GroundColor{0.20f, 0.18f, 0.16f}; // засветка нижних граней (отражённый свет)
    float AmbientStrength = 0.3f;

    DirectionalLight Sun;

    std::vector<PointLight> PointLights;
    std::vector<SpotLight> SpotLights;

    // Атмосфера сцены — туман и скайбокс. Живут здесь же, потому что это часть
    // окружения (сериализуются одним блоком с освещением, см. SceneSerializer).
    FogSettings Fog;
    SkyboxSettings Skybox;

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

    // Итоговые цвета полусферического ambient. Если включён скайбокс —
    // засветка берётся ИЗ НЕГО (верх объектов тянется к цвету зенита/неба,
    // низ — к приглушённому цвету горизонта), чтобы освещение согласовывалось
    // с видимым небом. Без скайбокса — заданные Sky/GroundColor.
    void ResolveAmbient(glm::vec3& outSky, glm::vec3& outGround) const {
        if (Skybox.Enabled) {
            // Верхняя полусфера видит и зенит, и горизонт — смещаемся к зениту;
            // нижняя (отражённый свет земли) — приглушённый горизонт.
            outSky = glm::mix(Skybox.HorizonColor, Skybox.TopColor, 0.6f);
            outGround = Skybox.HorizonColor * 0.5f;
        } else {
            outSky = SkyColor;
            outGround = GroundColor;
        }
    }
};
