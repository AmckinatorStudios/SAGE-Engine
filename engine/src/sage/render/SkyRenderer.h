#pragma once
#include <memory>
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
    // Звёзды проступают по мере ухода солнца — 0 днём, 1 ночью. Считается
    // из высоты солнца; отдельного времени суток небу знать не нужно.
    float StarIntensity = 1.0f;
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
