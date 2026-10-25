#include "TestFramework.h"

#include <cmath>
#include <string>

#include "sage/core/Noise.h"
#include "sage/render/MeshBuilder.h"
#include "sage/render/ResourceManager.h"
#include "sage/scene/Scene.h"
#include "sage/scripting/ScriptEngine.h"

// ---------------------------------------------------------------------------
// ПРОЦЕДУРНАЯ ГЕНЕРАЦИЯ: шумы (sage/core/Noise.h), построитель меша
// (sage/render/MeshBuilder.h) и то, как их видит скрипт (sage.noise,
// sage.math, sage.mesh, sage.material, sage.texture).
// ---------------------------------------------------------------------------

using namespace sage::noise;

TEST(noise_is_deterministic_by_seed) {
    // Одно зерно — один мир на любой машине: без этого «зерно мира» в
    // сохранении ничего не значит.
    Source a(42), b(42), c(43);
    float diff = 0.0f;
    for (int i = 0; i < 64; ++i) {
        const float x = i * 0.37f, y = i * 0.11f, z = i * 0.73f;
        CHECK_EQ(a.Perlin(x, y, z), b.Perlin(x, y, z));
        CHECK_EQ(a.Simplex(x, y, z), b.Simplex(x, y, z));
        diff += std::fabs(a.Perlin(x, y, z) - c.Perlin(x, y, z));
    }
    CHECK_TRUE(diff > 0.5f);   // другое зерно — другой мир
}

TEST(noise_ranges_are_what_the_docs_promise) {
    Source s(7);
    float pMin = 9, pMax = -9, sMin = 9, sMax = -9, vMin = 9, vMax = -9;
    for (int i = 0; i < 20000; ++i) {
        const float x = i * 0.0731f, y = i * 0.0517f, z = i * 0.0123f;
        const float p = s.Perlin(x, y, z), q = s.Simplex(x, y, z), v = s.Value(x, y, z);
        pMin = std::min(pMin, p); pMax = std::max(pMax, p);
        sMin = std::min(sMin, q); sMax = std::max(sMax, q);
        vMin = std::min(vMin, v); vMax = std::max(vMax, v);
        CHECK_TRUE(std::fabs(s.Perlin(x, y)) <= 1.05f);
        CHECK_TRUE(std::fabs(s.Simplex(x, y)) <= 1.05f);
    }
    CHECK_TRUE(pMin >= -1.05f && pMax <= 1.05f && pMax - pMin > 1.0f);
    CHECK_TRUE(sMin >= -1.05f && sMax <= 1.05f && sMax - sMin > 1.0f);
    CHECK_TRUE(vMin >= -1.0f && vMax <= 1.0f);
    // Градиентный шум равен нулю в узлах решётки.
    CHECK_NEAR(s.Perlin(3.0f, -2.0f, 5.0f), 0.0f, 1e-6);
}

TEST(noise_worley_and_fractals_behave) {
    Source s(1);
    for (int i = 0; i < 500; ++i) {
        float f1, f2, cell;
        s.Worley(i * 0.31f, i * 0.17f, i * 0.07f, f1, f2, cell);
        CHECK_TRUE(f1 <= f2);
        CHECK_TRUE(f1 >= 0.0f && f1 < 1.8f);
        CHECK_TRUE(cell >= 0.0f && cell < 1.0f);
    }
    Fractal f;
    f.Octaves = 6;
    for (int i = 0; i < 2000; ++i) {
        const glm::vec3 p(i * 0.05f, i * 0.031f, 0.5f);
        CHECK_TRUE(std::fabs(Fbm(s, Kind::Perlin, p, f)) <= 1.05f);
        const float r = Ridged(s, Kind::Simplex, p, f), t = Turbulence(s, Kind::Perlin, p, f);
        CHECK_TRUE(r >= 0.0f && r <= 1.0f);
        CHECK_TRUE(t >= 0.0f && t <= 1.05f);
    }
}

using sage::render::MeshBuilder;

TEST(mesh_builder_grid_counts_and_faces_up) {
    MeshBuilder g = MeshBuilder::Grid(10, 10, 4, 3);
    CHECK_EQ(g.VertexCount(), (size_t)(5 * 4));
    CHECK_EQ(g.TriangleCount(), (size_t)(4 * 3 * 2));
    const sage::render::MeshData d = g.ToMeshData();
    // Обход против часовой сверху: нормаль треугольника смотрит вверх.
    const glm::vec3 a = d.Vertices[d.Indices[0]].Position, b = d.Vertices[d.Indices[1]].Position,
                    c = d.Vertices[d.Indices[2]].Position;
    CHECK_TRUE(glm::cross(b - a, c - a).y > 0.0f);
    // Рельеф прямо при построении — и нормали досчитаны.
    MeshBuilder hill = MeshBuilder::Grid(10, 10, 8, 8, [](float x, float z) { return 5.0f - 0.1f * (x * x + z * z); });
    glm::vec3 lo, hi;
    hill.Bounds(lo, hi);
    CHECK_NEAR(hi.y, 5.0f, 1e-4);
    for (size_t i = 0; i < hill.VertexCount(); ++i) CHECK_NEAR(glm::length(hill.At((unsigned)i).Normal), 1.0f, 1e-3);
}

TEST(mesh_builder_shapes_face_outward_and_split_by_material) {
    for (const MeshBuilder& m : {MeshBuilder::Box(glm::vec3(2.0f)), MeshBuilder::Sphere(1.0f, 12, 8),
                                 MeshBuilder::Cylinder(1.0f, 2.0f, 12)}) {
        const sage::render::MeshData d = m.ToMeshData();
        int outward = 0, total = 0;
        for (size_t i = 0; i + 2 < d.Indices.size(); i += 3) {
            const glm::vec3 a = d.Vertices[d.Indices[i]].Position, b = d.Vertices[d.Indices[i + 1]].Position,
                            c = d.Vertices[d.Indices[i + 2]].Position;
            const glm::vec3 n = glm::cross(b - a, c - a);
            if (glm::length(n) < 1e-8f) continue;   // вырожденные у полюсов сферы
            ++total;
            if (glm::dot(n, (a + b + c) / 3.0f) > 0.0f) ++outward;
        }
        CHECK_TRUE(total > 0);
        CHECK_EQ(outward, total);
    }
    MeshBuilder b;
    const unsigned v0 = b.AddVertex({0, 0, 0}), v1 = b.AddVertex({1, 0, 0}), v2 = b.AddVertex({0, 1, 0});
    b.AddTriangle(v0, v1, v2);
    b.UseMaterial(2);
    b.AddTriangle(v0, v2, v1);
    CHECK_FALSE(b.AddTriangle(v0, v1, 99));
    const sage::render::MeshData d = b.ToMeshData();
    CHECK_EQ(d.Submeshes.size(), (size_t)2);
    CHECK_EQ(d.Submeshes[1].Material, 2);
}

TEST(script_builds_noise_formulas_meshes_materials_and_textures) {
    ScriptEngine se;
    Scene scene("P");
    se.BindScene(scene);
    se.Lua().script(R"LUA(
        local n = sage.noise.New{ type = "simplex", seed = 9, octaves = 3, frequency = 0.5 }
        local m = sage.noise.New{ type = "simplex", seed = 9, octaves = 3, frequency = 0.5 }
        same = n:Fbm(1.5, 2.5, 3.5) == m:Fbm(1.5, 2.5, 3.5)
        f1, f2 = sage.noise.Worley(0.3, 0.7, 0.1)

        local wave = sage.math.Formula("sin(x) * y + clamp(z, 0, 1)", "x, y, z")
        formula = wave(math.pi / 2, 3, 5)
        remap = sage.math.Remap(5, 0, 10, 100, 200)
        local r1, r2 = sage.math.Rng(5), sage.math.Rng(5)
        rng = r1:Int(1, 1000) == r2:Int(1, 1000)

        local g = sage.mesh.Grid(4, 4, 2, 2, function(x, z) return n:Fbm(x, 0, z) end)
        local b = sage.mesh.New()
        local a = b:Vertex(Vec3(0,0,0)); local c = b:Vertex(Vec3(1,0,0)); local d = b:Vertex(Vec3(0,0,1))
        b:Triangle(a, d, c)
        b:Append(g)
        verts, tris = b:VertexCount(), b:TriangleCount()

        sage.material.Create("proc_stone", { albedo = Vec3(0.5, 0.4, 0.3), roughness = 0.9, nearest = true })
        sage.material.Set("proc_stone", "metallic", 0.25)

        local t = sage.texture.New("proc_tex", 4, 2)
        t:Fill(function(u, v) return Vec3(u, v, 0) end)
        px = t:GetPixel(3, 1)
    )LUA");
    CHECK_TRUE(se.Lua()["same"].get<bool>());
    CHECK_TRUE(se.Lua()["f1"].get<float>() <= se.Lua()["f2"].get<float>());
    CHECK_NEAR(se.Lua()["formula"].get<float>(), 4.0f, 1e-4);
    CHECK_NEAR(se.Lua()["remap"].get<float>(), 150.0f, 1e-4);
    CHECK_TRUE(se.Lua()["rng"].get<bool>());
    CHECK_EQ(se.Lua()["verts"].get<int>(), 3 + 9);
    CHECK_EQ(se.Lua()["tris"].get<int>(), 1 + 8);
    std::shared_ptr<Material> m = ResourceManager::Instance().GetMaterial("proc_stone");
    CHECK_TRUE(m != nullptr);
    if (m) {
        CHECK_NEAR(m->Roughness, 0.9f, 1e-6);
        CHECK_NEAR(m->Metallic, 0.25f, 1e-6);
        CHECK_TRUE(m->Render.Filter == MaterialRender::Filtering::Nearest);
    }
    const glm::vec4 px = se.Lua()["px"].get<glm::vec4>();
    CHECK_NEAR(px.x, 0.875f, 0.01);   // центр последнего пикселя: (3 + 0.5) / 4
    CHECK_NEAR(px.y, 0.75f, 0.01);
}
