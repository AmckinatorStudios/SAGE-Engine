// Модульные тесты математики движка: отсечение по фрустуму, матрица трансформа,
// вычисления конфигурации (соотношение сторон / масштаб рендера / letterbox).
// Всё чистая математика — без GL-контекста.
#include "TestFramework.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "sage/render/Frustum.h"
#include "sage/scene/Transform.h"
#include "sage/core/Config.h"

using sage::render::Frustum;

// --- Frustum: извлечение плоскостей и тест сферы ----------------------------

TEST(Frustum_plane_normals_are_unit_length) {
    glm::mat4 proj = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
    glm::mat4 view = glm::lookAt(glm::vec3(0, 0, 5), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
    Frustum f = Frustum::FromViewProj(proj * view);
    for (int i = 0; i < 6; ++i) {
        float len = glm::length(glm::vec3(f.Planes[i]));
        CHECK_NEAR(len, 1.0f, 1e-4); // после нормализации все нормали единичны
    }
}

TEST(Frustum_sphere_inside_and_outside) {
    glm::mat4 proj = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
    glm::mat4 view = glm::lookAt(glm::vec3(0, 0, 5), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
    Frustum f = Frustum::FromViewProj(proj * view);

    // В центре взгляда — виден.
    CHECK_TRUE(f.IntersectsSphere(glm::vec3(0, 0, 0), 1.0f));
    // Позади камеры (камера в z=5 смотрит к -Z, значит +Z — сзади) — отсекается.
    CHECK_FALSE(f.IntersectsSphere(glm::vec3(0, 0, 50), 1.0f));
    // Далеко в стороне — отсекается.
    CHECK_FALSE(f.IntersectsSphere(glm::vec3(1000, 0, 0), 1.0f));
    // За дальней плоскостью (far=100, камера в z=5 → дальняя ~ z=-95) — отсекается.
    CHECK_FALSE(f.IntersectsSphere(glm::vec3(0, 0, -300), 1.0f));
}

TEST(Frustum_huge_radius_sphere_always_intersects) {
    glm::mat4 proj = glm::perspective(glm::radians(45.0f), 1.0f, 0.1f, 50.0f);
    glm::mat4 view = glm::lookAt(glm::vec3(0, 0, 3), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
    Frustum f = Frustum::FromViewProj(proj * view);
    // Сфера радиусом 1000 накрывает фрустум откуда угодно.
    CHECK_TRUE(f.IntersectsSphere(glm::vec3(500, 500, 500), 1000.0f));
}

// --- Transform::GetMatrix --------------------------------------------------

TEST(Transform_identity_is_identity) {
    Transform t; // дефолт: позиция 0, поворот 0, масштаб 1
    glm::mat4 m = t.GetMatrix();
    glm::mat4 id(1.0f);
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            CHECK_NEAR(m[c][r], id[c][r], 1e-6);
}

TEST(Transform_translation_places_origin) {
    Transform t;
    t.Position = {2.0f, -3.0f, 4.0f};
    glm::vec4 p = t.GetMatrix() * glm::vec4(0, 0, 0, 1);
    CHECK_NEAR(p.x, 2.0f, 1e-5);
    CHECK_NEAR(p.y, -3.0f, 1e-5);
    CHECK_NEAR(p.z, 4.0f, 1e-5);
}

TEST(Transform_scale_and_rotation_compose) {
    Transform t;
    t.Scale = {2.0f, 2.0f, 2.0f};
    t.Rotation = {0.0f, 90.0f, 0.0f}; // поворот вокруг Y на 90°
    // Точка (1,0,0) масштабируется до (2,0,0), затем поворот Y на 90° → (0,0,-2).
    glm::vec4 p = t.GetMatrix() * glm::vec4(1, 0, 0, 1);
    CHECK_NEAR(p.x, 0.0f, 1e-4);
    CHECK_NEAR(p.y, 0.0f, 1e-4);
    CHECK_NEAR(p.z, -2.0f, 1e-4);
}

// --- EngineConfig: математика дисплея --------------------------------------

TEST(Config_aspect_ratio_values) {
    sage::EngineConfig cfg;
    cfg.Aspect = sage::AspectMode::Free;
    CHECK_NEAR(cfg.AspectRatio(), 0.0f, 1e-6); // Free — «нет фикс. соотношения»
    cfg.Aspect = sage::AspectMode::R16x9;
    CHECK_NEAR(cfg.AspectRatio(), 16.0f / 9.0f, 1e-6);
    cfg.Aspect = sage::AspectMode::R4x3;
    CHECK_NEAR(cfg.AspectRatio(), 4.0f / 3.0f, 1e-6);
}

TEST(Config_scaled_resolution_clamps) {
    sage::EngineConfig cfg;
    int w, h;

    cfg.RenderScale = 0.5f;
    cfg.ScaledResolution(1280, 720, w, h);
    CHECK_EQ(w, 640);
    CHECK_EQ(h, 360);

    cfg.RenderScale = 5.0f; // кламп до 2.0
    cfg.ScaledResolution(1280, 720, w, h);
    CHECK_EQ(w, 2560);
    CHECK_EQ(h, 1440);

    cfg.RenderScale = 0.01f; // кламп до 0.25
    cfg.ScaledResolution(1280, 720, w, h);
    CHECK_EQ(w, 320);
    CHECK_EQ(h, 180);

    cfg.RenderScale = 1.0f; // минимум 1x1 даже для крошечного окна
    cfg.ScaledResolution(0, 0, w, h);
    CHECK_EQ(w, 1);
    CHECK_EQ(h, 1);
}

TEST(Config_letterbox_free_is_full_window) {
    sage::EngineConfig cfg;
    cfg.Aspect = sage::AspectMode::Free;
    int x, y, w, h;
    cfg.LetterboxViewport(1600, 900, x, y, w, h);
    CHECK_EQ(x, 0); CHECK_EQ(y, 0);
    CHECK_EQ(w, 1600); CHECK_EQ(h, 900);
}

TEST(Config_letterbox_pillarbox_when_window_too_wide) {
    sage::EngineConfig cfg;
    cfg.Aspect = sage::AspectMode::R16x9; // 1.777…
    int x, y, w, h;
    // Окно 2000x900 (2.22) шире 16:9 → вертикальные полосы.
    cfg.LetterboxViewport(2000, 900, x, y, w, h);
    CHECK_EQ(h, 900);            // высота во всё окно
    CHECK_EQ(w, 1600);           // 900 * 16/9
    CHECK_EQ(x, 200);            // центрировано: (2000-1600)/2
    CHECK_EQ(y, 0);
}

TEST(Config_letterbox_letterbox_when_window_too_tall) {
    sage::EngineConfig cfg;
    cfg.Aspect = sage::AspectMode::R16x9;
    int x, y, w, h;
    // Окно 1600x1200 (1.33) выше 16:9 → горизонтальные полосы.
    cfg.LetterboxViewport(1600, 1200, x, y, w, h);
    CHECK_EQ(w, 1600);           // ширина во всё окно
    CHECK_EQ(h, 900);            // 1600 / (16/9)
    CHECK_EQ(x, 0);
    CHECK_EQ(y, 150);            // (1200-900)/2
}
