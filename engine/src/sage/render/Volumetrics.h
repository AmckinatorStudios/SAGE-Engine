#pragma once
#include <memory>

#include <glm/glm.hpp>

#include "sage/core/Config.h"
#include "sage/render/ShadowMap.h"
#include "sage/render/Framebuffer.h"
#include "sage/rhi/Resources.h"

struct LightingEnvironment;

// ---------------------------------------------------------------------------
// Объёмный свет и объёмные облака (бета).
//
// Даёт две вещи, которых нельзя получить ни тенями, ни туманом по глубине:
//   • ЛУЧИ. Свет, пробившийся между досками надстройки, виден в воздухе, а не
//     только пятном на палубе. Считается маршем по лучу взгляда с выборкой
//     карты теней солнца: там, где отрезок луча освещён, воздух светится.
//   • ОБЛАКА. Настоящий объём, а не картинка на куполе: у них есть низ и верх,
//     они самозатеняются, светятся по краю на просвет и плывут по ветру.
//
// Цена держится низкой тремя приёмами, и все три обязательны:
//   1. Проход идёт в УМЕНЬШЕННОМ буфере (по умолчанию половина стороны, то
//      есть четверть пикселей) и поднимается обратно с учётом глубины.
//   2. Начало марша СМЕЩАЕТСЯ по пикселю упорядоченным шумом. Без этого
//      малое число шагов даёт видимые кольца-«ступени»; с ним они
//      рассыпаются в шум, который снимает подъём.
//   3. Марш обрывается, когда пропускание падает ниже порога: дальше вклад
//      уже не виден, а шаги стоят столько же.
//
// БЕТА: облака процедурные (шум считается в шейдере), без временного
// накопления между кадрами и без теней от облаков на землю. Этого достаточно,
// чтобы увидеть систему в деле и понять, чего от неё ждать дальше.
// ---------------------------------------------------------------------------
namespace sage::render {

struct VolumetricSettings {
    bool Enabled = false;

    // --- Объёмный свет в воздухе -------------------------------------------
    bool LightShafts = true;
    float Density = 0.02f;        // плотность среды у земли, 1/м
    float Anisotropy = 0.76f;     // вытянутость рассеяния вперёд (0 — равномерно)
    float MaxDistance = 160.0f;   // дальше не маршируем
    int Steps = 28;
    float Intensity = 1.0f;
    // Убывание плотности с высотой: без него дымка одинакова и у воды, и в
    // небе, отчего сцена выглядит затянутой молоком.
    float HeightFalloff = 0.06f;
    float BaseHeight = 0.0f;

    // --- Облака --------------------------------------------------------------
    bool Clouds = true;
    float CloudBottom = 180.0f;
    float CloudTop = 520.0f;
    float Coverage = 0.56f;       // 0 — ясно, 1 — сплошная облачность
    float CloudDensity = 1.3f;
    float CloudScale = 0.0022f;   // размер клубов (меньше — крупнее)
    glm::vec2 Wind{7.0f, 2.5f};
    glm::vec3 Tint{1.0f, 1.0f, 1.0f};
    int CloudSteps = 40;
    int CloudLightSteps = 5;

    // Доля разрешения для прохода: 1 — полное, 0.5 — половина стороны.
    float Scale = 0.5f;

    // Показать поле видимости солнца вместо результата (отладка теней в марше).
    bool Debug = false;
};

VolumetricSettings VolumetricsFromConfig(const sage::EngineConfig& cfg);

class Volumetrics {
public:
    Volumetrics() = default;
    Volumetrics(const Volumetrics&) = delete;
    Volumetrics& operator=(const Volumetrics&) = delete;

    // Считает объём в своём уменьшенном буфере и КОМПОЗИТИТ его в текущий
    // привязанный буфер (буфер сцены). Вызывать после отрисовки геометрии и до
    // пост-обработки: свечение лучей должно попасть в bloom, иначе солнце
    // светит, а его лучи — нет.
    // target — буфер сцены, в который композитится результат. Передаётся явно,
    // а не берётся «текущим»: марш идёт в свой буфер, и вернуться после него
    // надо туда, откуда пришли, а не в экран.
    void Render(Framebuffer& target, sage::rhi::TextureHandle sceneDepth, int w, int h,
                const glm::mat4& proj, const glm::mat4& view, const glm::vec3& camPos,
                const LightingEnvironment& env, const ShadowBinding& shadows,
                const VolumetricSettings& s, float time);

private:
    void EnsureTargets(int w, int h);

    std::unique_ptr<sage::rhi::Geometry> m_fsTri;
    std::unique_ptr<sage::rhi::RenderTarget> m_march;
    int m_w = 0, m_h = 0;   // размер уменьшенного буфера
};

} // namespace sage::render
