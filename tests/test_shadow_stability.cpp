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

namespace {

ShadowMap::CameraView DefaultView() {
    ShadowMap::CameraView v;
    v.Position = {0.0f, 1.7f, 6.0f};
    v.Forward = glm::normalize(glm::vec3(0.2f, -0.3f, -1.0f));
    v.Up = {0.0f, 1.0f, 0.0f};
    v.FovY = glm::radians(60.0f);
    v.Aspect = 16.0f / 9.0f;
    v.Near = 0.1f;
    v.ShadowDistance = 120.0f;
    return v;
}

const glm::vec3 kSun = glm::normalize(glm::vec3(0.45f, -0.55f, 0.70f));

// Координаты мировой точки в пространстве каскада: uv и глубина в [0,1].
glm::vec3 CascadeUVZ(const ShadowMap& sm, int cascade, glm::vec3 world) {
    const glm::vec4 lp = sm.LightMatrix(cascade) * glm::vec4(world, 1.0f);
    return glm::vec3(lp) / lp.w * 0.5f + 0.5f;
}

// Поворот вектора вокруг оси на угол (радианы).
glm::vec3 Rotate(glm::vec3 v, glm::vec3 axis, float angle) {
    return glm::vec3(glm::rotate(glm::mat4(1.0f), angle, axis) * glm::vec4(v, 0.0f));
}

} // namespace

// Поворот камеры не должен менять МАСШТАБ карт, а сдвиг содержимого — только на
// целые тексели. Иначе тени «кипят» от одного движения мыши: размер текселя
// скачет, и все кромки переезжают на его долю.
TEST(shadow_cascades_are_stable_while_camera_rotates) {
    NullDeviceScope scope;
    ShadowMap sm(2048, 3);
    ShadowMap::CameraView v = DefaultView();
    sm.SetCascades(kSun, v);
    const glm::vec3 point(1.3f, 0.2f, -2.7f);
    float radius0[3];
    glm::vec2 phase0[3];
    for (int c = 0; c < 3; ++c) {
        radius0[c] = sm.CascadeRadius(c);
        phase0[c] = TexelPhase(sm, c, point);
    }
    const glm::vec3 forward0 = v.Forward;
    bool sameRadius = true;
    float worst = 0.0f;
    // Рыскание и тангаж по 0.05° — доли пикселя на экране.
    for (int step = 1; step <= 30; ++step) {
        const float a = glm::radians(0.05f) * (float)step;
        glm::vec3 f = Rotate(forward0, glm::vec3(0.0f, 1.0f, 0.0f), a);
        f = Rotate(f, glm::normalize(glm::cross(f, glm::vec3(0.0f, 1.0f, 0.0f))), a * 0.5f);
        v.Forward = f;
        sm.SetCascades(kSun, v);
        for (int c = 0; c < 3; ++c) {
            sameRadius = sameRadius && sm.CascadeRadius(c) == radius0[c];
            const glm::vec2 d = glm::abs(TexelPhase(sm, c, point) - phase0[c]);
            worst = std::max(worst, std::max(std::min(d.x, 1.0f - d.x), std::min(d.y, 1.0f - d.y)));
        }
    }
    CHECK_TRUE(sameRadius);
    CHECK_TRUE(worst < 0.02f);
}

// Глубина тоже привязана: записанная глубина мировой точки меняется при ходьбе
// только целыми шагами в тексель, а не на произвольные доли. Иначе там, где
// запаса сравнения впритык, самозатенение мерцает, хотя карта стоит на месте.
TEST(shadow_cascade_depth_is_snapped_while_camera_moves) {
    NullDeviceScope scope;
    ShadowMap sm(2048, 3);
    ShadowMap::CameraView v = DefaultView();
    sm.SetCascades(kSun, v);
    const glm::vec3 point(1.3f, 0.2f, -2.7f);
    float z0[3];
    for (int c = 0; c < 3; ++c) z0[c] = CascadeUVZ(sm, c, point).z;
    float worstFraction = 0.0f;
    for (int step = 1; step <= 60; ++step) {
        v.Position = glm::vec3(0.0f, 1.7f, 6.0f) + glm::vec3(0.004f, 0.002f, -0.003f) * (float)step;
        sm.SetCascades(kSun, v);
        for (int c = 0; c < 3; ++c) {
            const float steps = std::abs(CascadeUVZ(sm, c, point).z - z0[c]) / sm.CascadeDepthPerTexel(c);
            const float frac = steps - std::floor(steps);
            worstFraction = std::max(worstFraction, std::min(frac, 1.0f - frac));
        }
    }
    CHECK_TRUE(worstFraction < 0.02f);
}

// Размер текселя, который уходит в шейдер, — ровно тот, с которым построена
// матрица. Раньше наружу шёл неквантованный радиус, а матрица строилась по
// квантованному: отступ и фильтр в шейдере работали с чужим текселем.
TEST(shadow_cascade_radius_matches_its_matrix) {
    NullDeviceScope scope;
    ShadowMap sm(2048, 3);
    ShadowMap::CameraView v = DefaultView();
    v.FovY = glm::radians(57.3f); // радиус сферы не кратен шагу квантования
    sm.SetCascades(kSun, v);
    for (int c = 0; c < 3; ++c) {
        const glm::mat4& m = sm.LightMatrix(c);
        const float matrixRadius = 1.0f / glm::length(glm::vec3(m[0][0], m[1][0], m[2][0]));
        CHECK_NEAR(sm.CascadeRadius(c), matrixRadius, matrixRadius * 1e-5f);
        CHECK_NEAR(sm.CascadeDepthPerTexel(c) * sm.CascadeDepthRange(c), sm.CascadeTexelSize(c),
                   sm.CascadeTexelSize(c) * 1e-4f);
    }
}

// Каскады накрывают ВСЁ, что видно до ShadowDistance, — при любом угле обзора
// и соотношении сторон: каждая точка видимого фрустума до этой дальности лежит
// хотя бы в одном каскаде (в его квадрате и в диапазоне глубин). Иначе на краю
// кадра или у дальней границы тени пропадали бы полосой.
TEST(shadow_cascades_cover_the_view_up_to_shadow_distance) {
    NullDeviceScope scope;
    ShadowMap sm(2048, 3);
    const float fovs[] = {40.0f, 60.0f, 90.0f};
    const float aspects[] = {1.0f, 16.0f / 9.0f, 2.4f};
    bool covered = true;
    for (float fov : fovs) {
        for (float aspect : aspects) {
            ShadowMap::CameraView v = DefaultView();
            v.FovY = glm::radians(fov);
            v.Aspect = aspect;
            sm.SetCascades(kSun, v);
            const glm::vec3 f = glm::normalize(v.Forward);
            const glm::vec3 r = glm::normalize(glm::cross(f, v.Up));
            const glm::vec3 u = glm::cross(r, f);
            const float th = std::tan(v.FovY * 0.5f), tw = th * v.Aspect;
            for (float dist : {0.2f, 3.0f, 9.0f, 25.0f, 60.0f, 119.0f}) {
                for (float sx : {-1.0f, 0.0f, 1.0f}) {
                    for (float sy : {-1.0f, 0.0f, 1.0f}) {
                        const glm::vec3 p = v.Position + f * dist + r * (sx * tw * dist) + u * (sy * th * dist);
                        bool inside = false;
                        for (int c = 0; c < 3 && !inside; ++c) {
                            const glm::vec3 q = CascadeUVZ(sm, c, p);
                            inside = q.x >= 0.0f && q.x <= 1.0f && q.y >= 0.0f && q.y <= 1.0f &&
                                     q.z >= 0.0f && q.z <= 1.0f;
                        }
                        covered = covered && inside;
                    }
                }
            }
        }
    }
    CHECK_TRUE(covered);
}

// Деление дальности: ближний каскад подробный, каскады растут, дальний
// дотягивается до ShadowDistance. Числа — для настроек по умолчанию (2048,
// 120 м, 60°, 16:9): при прежнем делении (lambda 0.6) ближний тексель был
// 1.9 см, средний 4.5 см.
TEST(shadow_cascade_split_gives_detail_near_camera) {
    NullDeviceScope scope;
    ShadowMap sm(2048, 3);
    sm.SetCascades(kSun, DefaultView());
    CHECK_TRUE(sm.CascadeTexelSize(0) < 0.012f);
    CHECK_TRUE(sm.CascadeTexelSize(1) < 0.032f);
    CHECK_TRUE(sm.CascadeTexelSize(0) < sm.CascadeTexelSize(1));
    CHECK_TRUE(sm.CascadeTexelSize(1) < sm.CascadeTexelSize(2));
}

// Поиск полутени: нулевое солнце — искать нечего (шейдер обходится одним
// ядром), у ближнего каскада радиус поиска больше ядра, но не больше предела;
// у дальнего — тексель крупнее любой полутени, и поиска нет.
TEST(shadow_penumbra_search_depends_on_cascade_texel) {
    NullDeviceScope scope;
    ShadowMap sm(2048, 3);
    sm.SetCascades(kSun, DefaultView());
    sm.SetSunAngle(0.0f);
    for (int c = 0; c < 3; ++c) CHECK_TRUE(sm.CascadeSearchTexels(c) == 0.0f);
    sm.SetSunAngle(0.8f);
    CHECK_TRUE(sm.CascadeSearchTexels(0) > 2.0f);
    CHECK_TRUE(sm.CascadeSearchTexels(0) <= ShadowMap::kMaxPenumbraTexels);
    CHECK_TRUE(sm.CascadeSearchTexels(2) < 2.0f);
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
