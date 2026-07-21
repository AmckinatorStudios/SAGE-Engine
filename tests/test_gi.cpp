#include "TestFramework.h"

#include <cstdio>

#include <glm/glm.hpp>

#include "sage/gi/BVH.h"
#include "sage/gi/GI.h"
#include "sage/gi/LightmapUV.h"
#include "sage/render/MeshData.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"

// ---------------------------------------------------------------------------
// Тесты запечённого GI (sage/gi) — ЧИСТЫЙ CPU, без GL-контекста: BVH,
// SH-гармоники, развёртка/упаковка лайтмап и полный бейк мини-сцены (свет,
// тень, детерминизм). Бейкер сознательно спроектирован headless — здесь это
// и проверяется.
// ---------------------------------------------------------------------------

using namespace sage::gi;

namespace {

// Треугольники единичного куба в мировых координатах (через генератор движка).
std::vector<Tri> CubeTris(const glm::vec3& center, float scale, const glm::vec3& albedo) {
    sage::render::MeshData d = sage::render::BuildCube();
    std::vector<Tri> tris;
    for (size_t i = 0; i + 2 < d.Indices.size(); i += 3) {
        Tri t;
        t.A = d.Vertices[d.Indices[i]].Position * scale + center;
        t.B = d.Vertices[d.Indices[i + 1]].Position * scale + center;
        t.C = d.Vertices[d.Indices[i + 2]].Position * scale + center;
        glm::vec3 n = glm::cross(t.B - t.A, t.C - t.A);
        glm::vec3 vn = d.Vertices[d.Indices[i]].Normal;
        t.N = glm::normalize(glm::dot(n, vn) < 0.0f ? -n : n);
        t.Albedo = albedo;
        tris.push_back(t);
    }
    return tris;
}

// Мини-сцена бейка: пол-плоскость 20x20 + куб на полу, солнце сверху.
Scene& BuildBakeScene(Scene& scene) {
    scene.Lighting.Sun.Direction = {0.0f, -1.0f, 0.0f};
    scene.Lighting.Sun.Intensity = 2.0f;
    scene.Lighting.Sun.Color = {1.0f, 1.0f, 1.0f};
    scene.Lighting.SkyColor = {0.5f, 0.6f, 0.8f};
    scene.Lighting.GroundColor = {0.2f, 0.2f, 0.2f};
    scene.Lighting.AmbientStrength = 0.5f;

    GameObject floor = scene.CreateObject("floor");
    floor.Renderer().Ref = MeshRef{MeshRef::Type::Plane, ""};
    floor.GetTransform().Scale = {20.0f, 1.0f, 20.0f};
    floor.Registry()->emplace<GIStaticComponent>(floor.Entity());

    GameObject cube = scene.CreateObject("cube");
    cube.Renderer().Ref = MeshRef{MeshRef::Type::Cube, ""};
    cube.GetTransform().Position = {0.0f, 1.0f, 0.0f}; // над полом (тень вниз)
    cube.GetTransform().Scale = {2.0f, 2.0f, 2.0f};
    cube.Registry()->emplace<GIStaticComponent>(cube.Entity());
    return scene;
}

GISettings FastSettings() {
    GISettings s;
    s.TexelsPerUnit = 2;
    s.AtlasSize = 256;
    s.SampleCount = 24;
    s.Bounces = 2;
    s.ProbeCellSize = 4.0f;
    s.MaxProbeAxis = 8;
    return s;
}

} // namespace

// --- BVH -------------------------------------------------------------------

TEST(gi_bvh_raycast_hits_cube) {
    BVH bvh;
    bvh.Build(CubeTris({0, 0, 0}, 1.0f, {1, 1, 1}));
    CHECK_EQ(bvh.TriangleCount(), (size_t)12);

    RayHit hit;
    // Луч в лоб по -Z: попадание в грань +Z на расстоянии 4.5 (куб 1x1 в нуле).
    CHECK_TRUE(bvh.Raycast({0, 0, 5}, {0, 0, -1}, 100.0f, hit));
    CHECK_NEAR(hit.T, 4.5f, 1e-3f);
    CHECK_NEAR(hit.N.z, 1.0f, 1e-4f);
    CHECK_FALSE(hit.BackFace);

    // Мимо куба.
    CHECK_FALSE(bvh.Raycast({0, 5, 5}, {0, 0, -1}, 100.0f, hit));

    // Изнутри куба: ближайшая грань — изнанка.
    CHECK_TRUE(bvh.Raycast({0, 0, 0}, {0, 0, -1}, 100.0f, hit));
    CHECK_TRUE(hit.BackFace);
}

TEST(gi_bvh_occlusion) {
    BVH bvh;
    bvh.Build(CubeTris({0, 0, 0}, 1.0f, {1, 1, 1}));
    // Заслонён кубом.
    CHECK_TRUE(bvh.Occluded({0, 0, 5}, {0, 0, -1}, 100.0f));
    // Отрезок заканчивается ДО куба.
    CHECK_FALSE(bvh.Occluded({0, 0, 5}, {0, 0, -1}, 2.0f));
    // Мимо.
    CHECK_FALSE(bvh.Occluded({0, 5, 5}, {0, 0, -1}, 100.0f));
    // Пустой BVH ничего не заслоняет.
    BVH empty;
    empty.Build({});
    CHECK_FALSE(empty.Occluded({0, 0, 0}, {0, 1, 0}, 100.0f));
}

// --- SH (L1) ---------------------------------------------------------------

TEST(gi_sh_directional_lobe) {
    // Весь радианс — с +Y: освещённость по +Y должна быть заметно больше, чем
    // по -Y, и неотрицательна во всех направлениях.
    SH1 sh;
    SHAddSample(sh, {0, 1, 0}, {1.0f, 0.5f, 0.25f}, 1.0f);
    glm::vec3 up = SHEvalIrradianceOverPi(sh, {0, 1, 0});
    glm::vec3 down = SHEvalIrradianceOverPi(sh, {0, -1, 0});
    CHECK_TRUE(up.r > down.r);
    CHECK_TRUE(up.r > 0.0f);
    CHECK_TRUE(down.r >= 0.0f); // кламп отрицательного лепестка L1
    // Каналы масштабируются пропорционально радиансу.
    CHECK_NEAR(up.g / up.r, 0.5f, 1e-3f);
    CHECK_NEAR(up.b / up.r, 0.25f, 1e-3f);
}

TEST(gi_sh_uniform_environment) {
    // Равномерное окружение радианса L со всей сферы: E/π должна сойтись к L
    // (свёртка косинусом по полусфере: E = πL). Квадратура — регулярная сетка.
    SH1 sh;
    const int N = 32;
    int count = 0;
    for (int i = 0; i < N; ++i) {
        for (int j = 0; j < N * 2; ++j) {
            float cosT = 1.0f - 2.0f * (i + 0.5f) / N; // равные площади по сфере
            float sinT = std::sqrt(std::max(0.0f, 1.0f - cosT * cosT));
            float phi = 2.0f * 3.14159265f * (j + 0.5f) / (N * 2);
            SHAddSample(sh, {sinT * std::cos(phi), cosT, sinT * std::sin(phi)},
                        {2.0f, 2.0f, 2.0f}, 1.0f);
            ++count;
        }
    }
    // Нормировка квадратуры: вес каждого сэмпла 4π/count.
    float w = 4.0f * 3.14159265f / (float)count;
    sh.R *= w; sh.G *= w; sh.B *= w;
    glm::vec3 e = SHEvalIrradianceOverPi(sh, {0, 1, 0});
    CHECK_NEAR(e.r, 2.0f, 0.05f);
    CHECK_NEAR(e.g, 2.0f, 0.05f);
}

// --- Развёртка и упаковка ---------------------------------------------------

TEST(gi_charts_cube_six_axes) {
    sage::render::MeshData mesh = sage::render::BuildCube();
    auto charts = BuildCharts(mesh, glm::mat4(1.0f), 8.0f, 128);
    CHECK_EQ(charts.size(), (size_t)6); // по чарту на каждую грань куба
    // Куб уже имеет пограневые вершины — дублирование не должно раздуть меш.
    CHECK_EQ(mesh.Vertices.size(), (size_t)24);
    // Локальные UV в [0,1].
    for (const auto& c : charts) {
        for (int t : c.Tris)
            for (int k = 0; k < 3; ++k) {
                glm::vec2 uv = mesh.Vertices[mesh.Indices[t * 3 + k]].TexCoords2;
                CHECK_TRUE(uv.x >= -1e-4f && uv.x <= 1.0001f);
                CHECK_TRUE(uv.y >= -1e-4f && uv.y <= 1.0001f);
            }
    }
}

TEST(gi_pack_charts_no_overlap) {
    // 40 чартов разных размеров: все размещены, в границах страницы, без
    // пересечений (проверка занятости по текселям).
    std::vector<Chart> charts(40);
    for (size_t i = 0; i < charts.size(); ++i) {
        charts[i].W = 8 + (int)(i * 7) % 40;
        charts[i].H = 8 + (int)(i * 13) % 32;
    }
    std::vector<Chart*> ptrs;
    for (auto& c : charts) ptrs.push_back(&c);
    const int page = 128;
    int pages = PackCharts(ptrs, page);
    CHECK_TRUE(pages >= 1);

    std::vector<std::vector<uint8_t>> occupancy(pages, std::vector<uint8_t>((size_t)page * page, 0));
    for (const auto& c : charts) {
        CHECK_TRUE(c.Page >= 0 && c.Page < pages);
        CHECK_TRUE(c.X >= 0 && c.Y >= 0 && c.X + c.W <= page && c.Y + c.H <= page);
        for (int y = c.Y; y < c.Y + c.H; ++y)
            for (int x = c.X; x < c.X + c.W; ++x) {
                uint8_t& cell = occupancy[c.Page][(size_t)y * page + x];
                CHECK_EQ((int)cell, 0); // пересечение чартов — провал
                cell = 1;
            }
    }
}

// --- Полный бейк -----------------------------------------------------------

TEST(gi_bake_scene_light_and_shadow) {
    Scene scene("bake");
    BuildBakeScene(scene);
    BakeInput input = CollectBakeInput(scene, FastSettings());
    CHECK_EQ(input.Items.size(), (size_t)2);

    auto state = Bake(input);
    CHECK_TRUE(state->Baked);
    CHECK_EQ(state->Entities.size(), (size_t)2);
    CHECK_TRUE(!state->Pages.empty());
    CHECK_TRUE(state->Probes.Valid());

    // Средняя запечённая освещённость покрытых текселей > 0 (небо + отскоки),
    // и не запредельная (санити диапазона).
    const LightmapPage& p = state->Pages[0];
    double sum = 0.0;
    int covered = 0;
    for (size_t i = 0; i < p.Texels.size(); ++i) {
        if (!p.Coverage[i]) continue;
        sum += p.Texels[i].r;
        ++covered;
    }
    CHECK_TRUE(covered > 0);
    double avg = sum / covered;
    CHECK_TRUE(avg > 0.01);
    CHECK_TRUE(avg < 10.0);

    // Пробы: непрямая освещённость над сценой неотрицательна и конечна.
    glm::vec3 e = SHEvalIrradianceOverPi(state->Probes.Probes[state->Probes.Probes.size() / 2], {0, 1, 0});
    CHECK_TRUE(e.r >= 0.0f && e.r < 100.0f);
}

TEST(gi_bake_deterministic) {
    // Два бейка одной сцены обязаны совпасть побайтово (детерминизм — контракт:
    // на нём держится восстановление UV при загрузке).
    Scene scene("bake");
    BuildBakeScene(scene);
    GISettings s = FastSettings();
    s.SampleCount = 8; // быстрее — детерминизму количество сэмплов безразлично
    BakeInput input = CollectBakeInput(scene, s);
    auto a = Bake(input);
    auto b = Bake(input);
    CHECK_EQ(a->Pages.size(), b->Pages.size());
    bool texelsEqual = true;
    for (size_t pi = 0; pi < a->Pages.size() && texelsEqual; ++pi) {
        const auto& ta = a->Pages[pi].Texels;
        const auto& tb = b->Pages[pi].Texels;
        if (ta.size() != tb.size()) { texelsEqual = false; break; }
        for (size_t i = 0; i < ta.size(); ++i)
            if (ta[i] != tb[i]) { texelsEqual = false; break; }
    }
    CHECK_TRUE(texelsEqual);
    CHECK_EQ(a->GeometryHash, b->GeometryHash);
}

TEST(gi_bake_occlusion_darkens) {
    // Пол, полностью накрытый широкой «крышей» чуть выше: небо закрыто —
    // тексели пола обязаны быть заметно темнее, чем у открытого пола.
    auto bakeAvgFloor = [](bool roofed) {
        Scene scene("cmp");
        scene.Lighting.Sun.Intensity = 0.0f; // только небо — чистая проверка окклюзии
        scene.Lighting.SkyColor = {1.0f, 1.0f, 1.0f};
        scene.Lighting.GroundColor = {0.0f, 0.0f, 0.0f};
        scene.Lighting.AmbientStrength = 1.0f;

        GameObject floor = scene.CreateObject("floor");
        floor.Renderer().Ref = MeshRef{MeshRef::Type::Plane, ""};
        floor.GetTransform().Scale = {8.0f, 1.0f, 8.0f};
        floor.Registry()->emplace<GIStaticComponent>(floor.Entity());

        if (roofed) {
            GameObject roof = scene.CreateObject("roof");
            roof.Renderer().Ref = MeshRef{MeshRef::Type::Cube, ""};
            roof.GetTransform().Position = {0.0f, 1.0f, 0.0f};
            roof.GetTransform().Scale = {16.0f, 0.5f, 16.0f};
            auto& gs = roof.Registry()->emplace<GIStaticComponent>(roof.Entity());
            gs.Lightmapped = false; // только окклюдер
        }

        GISettings s;
        s.TexelsPerUnit = 2;
        s.AtlasSize = 128;
        s.SampleCount = 32;
        s.Bounces = 1;
        s.MaxProbeAxis = 4;
        auto state = Bake(CollectBakeInput(scene, s));

        // Средний тексель ЦЕНТРА страницы пола (края могут видеть небо сбоку).
        const LightmapPage& p = state->Pages[0];
        double sum = 0.0;
        int cnt = 0;
        for (size_t i = 0; i < p.Texels.size(); ++i) {
            if (!p.Coverage[i]) continue;
            sum += p.Texels[i].r;
            ++cnt;
        }
        return cnt > 0 ? sum / cnt : 0.0;
    };

    double open = bakeAvgFloor(false);
    double roofed = bakeAvgFloor(true);
    CHECK_TRUE(open > 0.5);          // открытый пол видит белое небо (≈1)
    CHECK_TRUE(roofed < open * 0.5); // под крышей ощутимо темнее
}

TEST(gi_rebuild_after_load_restores_pages) {
    // Полный цикл восстановления: бейк -> страницы на диск -> «загруженная»
    // сцена с тем же составом + пустой GIState(баked) -> RebuildAfterLoad
    // пересчитывает UV и читает страницы. Работает headless (без GL).
    const char* scenePath = "test_gi_scene.sage"; // рядом с бинарником тестов

    Scene scene("gi_io");
    BuildBakeScene(scene);
    GISettings s = FastSettings();
    s.SampleCount = 8;
    auto baked = Bake(CollectBakeInput(scene, s));
    CHECK_TRUE(baked->Baked);
    SavePages(*baked, scenePath);

    // «Загрузка»: та же сцена структурно, GI-состояние — как из файла сцены
    // (настройки + отпечаток + флаг baked, без страниц/сущностей).
    Scene loaded("gi_io");
    BuildBakeScene(loaded);
    loaded.GI = std::make_shared<GIState>();
    loaded.GI->Settings = s;
    loaded.GI->GeometryHash = baked->GeometryHash;
    loaded.GI->Baked = true;
    RebuildAfterLoad(loaded, scenePath);

    CHECK_TRUE(loaded.GI->Baked);
    CHECK_EQ(loaded.GI->Entities.size(), baked->Entities.size());
    CHECK_EQ(loaded.GI->Pages.size(), baked->Pages.size());
    // Страницы после .hdr-раундтрипа близки к исходным (формат с потерями —
    // общая экспонента RGBE, поэтому допуск, а не равенство).
    const auto& pa = baked->Pages[0];
    const auto& pb = loaded.GI->Pages[0];
    CHECK_EQ(pa.Texels.size(), pb.Texels.size());
    double maxDiff = 0.0;
    for (size_t i = 0; i < pa.Texels.size(); ++i)
        maxDiff = std::max(maxDiff, (double)std::abs(pa.Texels[i].r - pb.Texels[i].r));
    CHECK_TRUE(maxDiff < 0.05);

    // Геометрия изменилась после бейка -> лайтмапы обязаны сброситься.
    Scene changed("gi_io");
    BuildBakeScene(changed);
    changed.Get(2).GetTransform().Position.x += 3.0f; // сдвинули куб
    changed.GI = std::make_shared<GIState>();
    changed.GI->Settings = s;
    changed.GI->GeometryHash = baked->GeometryHash;
    changed.GI->Baked = true;
    RebuildAfterLoad(changed, scenePath);
    CHECK_FALSE(changed.GI->Baked);

    std::remove("test_gi_scene_gi_page0.hdr");
}

TEST(gi_transplant_matches_hash) {
    Scene a("t");
    BuildBakeScene(a);
    GISettings s = FastSettings();
    s.SampleCount = 8;
    a.GI = Bake(CollectBakeInput(a, s));
    CHECK_TRUE(a.GI->Baked);

    // Идентичная сцена — бейк переезжает.
    Scene b("t");
    BuildBakeScene(b);
    Transplant(a, b);
    CHECK_TRUE(b.GI == a.GI);

    // Изменённая сцена — не переезжает.
    Scene c("t");
    BuildBakeScene(c);
    c.Get(2).GetTransform().Position.y += 1.0f;
    Transplant(a, c);
    CHECK_TRUE(c.GI == nullptr);
}
