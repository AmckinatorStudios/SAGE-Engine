// Тени не дрожат при движении камеры: каскады привязаны к сетке текселей.
//
// Карта едет за камерой. Если её край попадает между текселями, одна и та же
// кромка предмета каждый кадр ложится в карту по-разному — тень «кипит». С
// привязкой содержимое карты сдвигается ровно на целый тексель, и мировая
// точка попадает в ТО ЖЕ место внутри своего текселя. Здесь стояла привязка в
// базисе, построенном от самого центра, — она не делала ничего, и проверка
// ниже на ней падает.
#include "TestFramework.h"

#include <cmath>
#include <memory>

#include "sage/render/ShadowMap.h"
#include "sage/rhi/GraphicsDevice.h"

namespace {

struct NullDeviceScope {
    std::unique_ptr<sage::rhi::GraphicsDevice> Device =
        sage::rhi::GraphicsDevice::Create(sage::rhi::Backend::Null);
    NullDeviceScope() {
        Device->Init(nullptr);
        sage::rhi::GraphicsDevice::SetCurrent(Device.get());
    }
    ~NullDeviceScope() { sage::rhi::GraphicsDevice::SetCurrent(nullptr); }
};

// Дробное положение мировой точки внутри текселя каскада.
glm::vec2 TexelPhase(const ShadowMap& sm, int cascade, glm::vec3 world) {
    const glm::vec4 lp = sm.LightMatrix(cascade) * glm::vec4(world, 1.0f);
    const glm::vec2 uv = glm::vec2(lp) / lp.w * 0.5f + 0.5f;
    const glm::vec2 t = uv * (float)sm.Resolution();
    return t - glm::floor(t);
}

} // namespace

TEST(shadow_cascades_are_snapped_to_texels_while_camera_moves) {
    NullDeviceScope scope;
    ShadowMap sm(2048, 3);
    const glm::vec3 sun = glm::normalize(glm::vec3(0.45f, -0.55f, 0.70f));
    ShadowMap::CameraView v;
    v.Forward = glm::normalize(glm::vec3(0.2f, -0.3f, -1.0f));
    v.Up = {0.0f, 1.0f, 0.0f};
    v.FovY = glm::radians(60.0f);
    v.Aspect = 16.0f / 9.0f;
    v.Near = 0.1f;
    v.ShadowDistance = 120.0f;

    const glm::vec3 point(1.3f, 0.2f, -2.7f);
    v.Position = {0.0f, 1.7f, 6.0f};
    sm.SetCascades(sun, v);
    glm::vec2 phase0[3];
    float radius0[3];
    for (int c = 0; c < 3; ++c) {
        phase0[c] = TexelPhase(sm, c, point);
        radius0[c] = sm.CascadeRadius(c);
    }
    float worst = 0.0f;
    bool sameRadius = true;
    // Шаги по 3 мм — меньше текселя ближнего каскада: именно такие сдвиги и
    // давали дрожание кромок при ходьбе.
    for (int step = 1; step <= 40; ++step) {
        v.Position = glm::vec3(0.0f, 1.7f, 6.0f) + glm::vec3(0.003f, 0.001f, -0.002f) * (float)step;
        sm.SetCascades(sun, v);
        for (int c = 0; c < 3; ++c) {
            sameRadius = sameRadius && sm.CascadeRadius(c) == radius0[c];
            const glm::vec2 d = glm::abs(TexelPhase(sm, c, point) - phase0[c]);
            // Фаза сравнивается по модулю 1: 0.999 и 0.001 — одно и то же место.
            worst = std::max(worst, std::max(std::min(d.x, 1.0f - d.x), std::min(d.y, 1.0f - d.y)));
        }
    }
    CHECK_TRUE(sameRadius);
    CHECK_TRUE(worst < 0.02f);
}

TEST(shadow_penumbra_grows_with_sun_size) {
    NullDeviceScope scope;
    ShadowMap sm(2048, 1);
    sm.SetLightMatrix(glm::vec3(0.0f, -1.0f, 0.2f), glm::vec3(0.0f), 20.0f);
    sm.SetSunAngle(0.0f);
    CHECK_TRUE(sm.CascadePenumbraTexels(0) == 0.0f);
    sm.SetSunAngle(0.5f);
    const float small = sm.CascadePenumbraTexels(0);
    sm.SetSunAngle(2.0f);
    CHECK_TRUE(sm.CascadePenumbraTexels(0) > small * 3.5f);
    // Перевод честный: метр от заслона при солнце 2° — полутень tg(1°) м,
    // то есть столько текселей, сколько их в этих сантиметрах.
    const float metresPerDepth = sm.CascadeDepthRange(0);
    const float expected = metresPerDepth * std::tan(glm::radians(1.0f)) / sm.CascadeTexelSize(0);
    CHECK_NEAR(sm.CascadePenumbraTexels(0), expected, expected * 1e-3f);
}
