#pragma once
#include <memory>
#include <string>
#include <glm/glm.hpp>
#include "sage/rhi/GraphicsDevice.h"

// ---------------------------------------------------------------------------
// SkyRenderer — процедурный градиентный скайбокс без ассетов. Рисует
// полноэкранный треугольник, восстанавливает мировое направление луча из
// обратной матрицы вид-проекции и заливает фон градиентом от HorizonColor
// (низ) к TopColor (зенит) по вертикали луча. Часть ЯДРА рендера, доступна
// и редактору, и играм.
//
// Вызывать ПЕРВЫМ в кадре (до отрисовки сцены): пишет только цвет, без
// глубины (depth-mask off, depth-test off) — сцена рисуется поверх.
//
//   sky.Draw(view, proj, env.Skybox.TopColor, env.Skybox.HorizonColor);
//   ... затем обычный проход сцены ...
//
// Шейдер встроен строкой — внешних файлов не требует.
// ---------------------------------------------------------------------------
// Светила на небе. Отдельной структурой, а не пятью аргументами Draw:
// параметров у неба будет только больше, и очередной «ещё один vec3» в
// сигнатуре быстро превращает вызов в набор безымянных чисел.
struct SkyCelestials {
    bool Enabled = false;
    // Направление НА светило (единичное). Совпадает с направлением солнца
    // сцены, взятым со знаком минус: у направленного света хранится, КУДА
    // он светит, а нарисовать надо там, ОТКУДА.
    glm::vec3 SunDir{0.0f, 1.0f, 0.0f};
    glm::vec3 SunColor{1.0f, 0.95f, 0.85f};
    float SunSize = 0.045f;      // угловой радиус диска в радианах
    // Луна ходит противоходом солнцу: когда одно садится, другое встаёт.
    // Считается тут же из SunDir, чтобы игре не приходилось вести два
    // направления и следить за их согласованностью.
    bool Moon = true;
    glm::vec3 MoonColor{0.78f, 0.82f, 0.95f};
    float MoonSize = 0.030f;
    // Настройка автора: сколько звёзд на ночном небе (0 — без звёзд).
    float StarIntensity = 1.0f;
    // Время суток: 1 — день, 0 — ночь. Приходит из общей модели
    // (sage/render/SkyModel.h), а не считается небом заново: два независимых
    // представления о времени суток однажды разойдутся, и звёзды загорятся не
    // тогда, когда стемнеет освещение.
    float DayFactor = 1.0f;

    // Высотный туман на НЕБЕ: без него даль тонула в дымке, а небо над ней
    // оставалось чистым — по горизонту шла резкая черта «конец тумана».
    // Луч неба считается длиной kSkyFogDistance.
    bool HeightFog = false;
    glm::vec3 FogColor{0.55f, 0.62f, 0.72f};
    float FogDensity = 0.0f, FogFalloff = 0.2f, FogHeight = 0.0f, FogMaxOpacity = 1.0f;
    float FogSunScatter = 0.0f, FogSunExponent = 8.0f;
    glm::vec3 FogSunLight{0.0f};   // цвет * яркость солнца сцены

    // --- Форма неба (см. SkyboxSettings: там смысл каждого поля) ---------
    // Умолчания — прежняя картинка: небо, собранное кодом без этих полей,
    // выглядит как раньше.
    float GradientExponent = 0.5f;
    float HorizonSoftness = 0.25f;
    float HorizonOffset = 0.0f;
    bool Ground = false;
    glm::vec3 GroundColor{0.10f, 0.14f, 0.45f};  // уже с учётом времени суток
    float GroundBlend = 0.05f;
    int SunShape = 0, MoonShape = 0;             // SkyboxSettings::DiscShape
    float SunBrightness = 6.0f;
    float SunGlow = 1.0f;
    bool MoonPhase = true;
    std::string SunTexture, MoonTexture;         // пусто — диск формой
    bool PixelArt = false;                       // картинки светил без сглаживания
    float StarDensity = 1.0f, StarSize = 1.0f;
    bool Clouds = false;
    int CloudStyle = 0;                          // SkyboxSettings::CloudStyle
    glm::vec3 CloudColor{1.0f};                  // уже с учётом времени суток
    float CloudHeight = 120.0f, CloudScale = 12.0f, CloudCoverage = 0.45f;
    float CloudOpacity = 0.85f, CloudFade = 1500.0f;
    glm::vec2 CloudWind{1.0f, 0.0f};
    // Время, секунды: по нему плывут облака. Ноль — облака стоят.
    float Time = 0.0f;
};

// Светила из окружения сцены. Отдельной функцией, потому что потребителей у
// неба трое (редакторский вьюпорт, панель Game, рантайм игры), и собирать эти
// поля в каждом значило бы гарантированно получить три слегка разных неба.
struct LightingEnvironment;
SkyCelestials CelestialsFromEnvironment(const LightingEnvironment& env);

class SkyRenderer {
public:
    SkyRenderer();

    SkyRenderer(const SkyRenderer&) = delete;
    SkyRenderer& operator=(const SkyRenderer&) = delete;

    void Draw(const glm::mat4& view, const glm::mat4& proj, glm::vec3 topColor,
              glm::vec3 horizonColor, const SkyCelestials& sky = SkyCelestials{});

private:
    std::unique_ptr<sage::rhi::ShaderProgram> m_shader;
    std::unique_ptr<sage::rhi::Geometry> m_geometry;
};
