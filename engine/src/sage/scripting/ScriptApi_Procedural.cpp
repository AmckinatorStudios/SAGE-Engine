#include "sage/scripting/ScriptEngine.h"
#include "sage/scripting/ScriptApiCommon.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>

#include "sage/assets/format/MeshFormat.h"
#include "sage/core/Noise.h"
#include "sage/render/Material.h"
#include "sage/render/Mesh.h"
#include "sage/render/MeshBuilder.h"
#include "sage/render/ResourceManager.h"
#include "sage/render/Texture.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "sage/scripting/lua/LuaInternal.h"

// ---------------------------------------------------------------------------
// ПРОЦЕДУРНАЯ ГЕНЕРАЦИЯ ИЗ СКРИПТА: шумы, формулы, меши, материалы, текстуры.
//
// Всё, что игра раньше могла только ВЗЯТЬ из файла (меш, материал, картинку),
// теперь можно СОБРАТЬ кодом — рельеф по шуму, дорогу по точкам, планету,
// камень, раскраску по высоте. Строительные блоки маленькие и
// комбинируются как угодно; готовых «генераторов уровня» здесь нет
// намеренно: генератор — это логика игры, а не движка.
//
//   local n = sage.noise.New{ type = "simplex", seed = 7, octaves = 5 }
//   local ground = sage.mesh.Grid(100, 100, 128, 128, function(x, z)
//       return n:Fbm(x * 0.02, 0, z * 0.02) * 12
//   end)
//   sage.material.Create("grass", { albedo = Vec3(0.3, 0.6, 0.25), roughness = 0.9 })
//   ground:Apply(self.gameObject)
//   sage.render.SetMaterial(self.gameObject, "grass")
//
// Индексы вершин и номера слотов — с ЕДИНИЦЫ, как всё в Lua.
// ---------------------------------------------------------------------------

namespace {

using sage::noise::Fractal;
using sage::noise::Kind;
using sage::render::MeshBuilder;

Kind ParseKind(const std::string& s) {
    if (s == "perlin") return Kind::Perlin;
    if (s == "simplex") return Kind::Simplex;
    if (s == "value") return Kind::Value;
    if (s == "worley" || s == "cellular_f1") return Kind::Worley;
    if (s == "worley2" || s == "cellular_f2") return Kind::WorleyF2;
    if (s == "cell" || s == "cellular") return Kind::Cellular;
    throw std::runtime_error("noise: неизвестный тип шума '" + s +
                             "' (perlin/simplex/value/worley/worley2/cell)");
}

// Генератор шума со своими настройками — то, что удобно держать в поле скрипта.
struct NoiseGen {
    sage::noise::Source Src;
    Kind Type = Kind::Perlin;
    Fractal Frac;
    float WarpStrength = 0.0f;

    glm::vec3 P(float x, float y, float z) const {
        const glm::vec3 p(x, y, z);
        return WarpStrength != 0.0f ? sage::noise::Warp(Src, Type, p, WarpStrength, Frac) : p;
    }
};

void ApplyOptions(NoiseGen& g, const sol::optional<sol::table>& opts) {
    if (!opts) return;
    const sol::table& t = *opts;
    if (sol::optional<std::string> k = t["type"]) g.Type = ParseKind(*k);
    if (sol::optional<int> s = t["seed"]) g.Src.Reseed((uint32_t)*s);
    g.Frac.Octaves = t.get_or("octaves", g.Frac.Octaves);
    g.Frac.Lacunarity = t.get_or("lacunarity", g.Frac.Lacunarity);
    g.Frac.Gain = t.get_or("gain", g.Frac.Gain);
    g.Frac.Frequency = t.get_or("frequency", g.Frac.Frequency);
    g.WarpStrength = t.get_or("warp", g.WarpStrength);
}

// Общий источник для коротких вызовов sage.noise.Perlin(x, y) — зерно меняет
// sage.noise.Seed. Свой у каждого ScriptEngine не нужен: шум без состояния.
sage::noise::Source& Global() {
    static sage::noise::Source s(0);
    return s;
}

// Детерминированный генератор случайных чисел (xorshift128+): одно зерно —
// одна последовательность на любой машине, в отличие от math.random.
struct Rng {
    uint64_t A = 0x9E3779B97F4A7C15ull, B = 0xBF58476D1CE4E5B9ull;
    explicit Rng(uint64_t seed = 0) { Seed(seed); }
    void Seed(uint64_t seed) {
        uint64_t z = seed + 0x9E3779B97F4A7C15ull;
        auto mix = [&z]() {
            z += 0x9E3779B97F4A7C15ull;
            uint64_t x = z;
            x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
            x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
            return x ^ (x >> 31);
        };
        A = mix();
        B = mix();
        if (!A && !B) B = 1;
    }
    uint64_t Next() {
        uint64_t s1 = A;
        const uint64_t s0 = B;
        A = s0;
        s1 ^= s1 << 23;
        B = s1 ^ s0 ^ (s1 >> 17) ^ (s0 >> 26);
        return B + s0;
    }
    double Float() { return (double)(Next() >> 11) * (1.0 / 9007199254740992.0); }
};

// Пиксельный холст для процедурной текстуры.
struct TextureCanvas {
    std::string Name;
    int W = 0, H = 0;
    std::vector<unsigned char> Px;   // RGBA8
    bool Nearest = false;

    void Set(int x, int y, const glm::vec4& c) {
        if (x < 0 || y < 0 || x >= W || y >= H) return;
        unsigned char* p = &Px[((size_t)y * W + x) * 4];
        for (int k = 0; k < 4; ++k) p[k] = (unsigned char)std::clamp(c[k] * 255.0f + 0.5f, 0.0f, 255.0f);
    }
    glm::vec4 Get(int x, int y) const {
        if (x < 0 || y < 0 || x >= W || y >= H) return glm::vec4(0.0f);
        const unsigned char* p = &Px[((size_t)y * W + x) * 4];
        return glm::vec4(p[0], p[1], p[2], p[3]) / 255.0f;
    }
};

glm::vec4 ToColor(const sol::object& o) {
    if (o.is<glm::vec4>()) return o.as<glm::vec4>();
    if (o.is<glm::vec3>()) return glm::vec4(o.as<glm::vec3>(), 1.0f);
    if (o.is<double>()) {
        const float v = (float)o.as<double>();
        return glm::vec4(v, v, v, 1.0f);
    }
    throw std::runtime_error("цвет — это Vec3, Vec4 или число");
}

std::shared_ptr<Material> MaterialByName(const std::string& name) {
    std::shared_ptr<Material> m = ResourceManager::Instance().GetMaterial(name);
    if (!m) throw std::runtime_error("material: нет материала '" + name + "'");
    return m;
}

// Свойство материала по имени — одно место для Create и Set: имена те же, что
// в окне материала (albedo, roughness, …), а не поля C++.
void SetMaterialProperty(Material& m, const std::string& key, const sol::object& v) {
    auto f = [&]() { return (float)v.as<double>(); };
    auto vec3 = [&]() { return v.is<glm::vec3>() ? v.as<glm::vec3>() : glm::vec3(f()); };
    auto str = [&]() { return v.as<std::string>(); };
    if (key == "albedo" || key == "color") m.Albedo = vec3();
    else if (key == "emissive") m.Emissive = vec3();
    else if (key == "emissiveStrength") m.EmissiveStrength = f();
    else if (key == "metallic") m.Metallic = f();
    else if (key == "roughness") m.Roughness = f();
    else if (key == "opacity") m.Opacity = f();
    else if (key == "texture" || key == "albedoMap") m.TexturePath = str();
    else if (key == "normalMap") m.NormalMapPath = str();
    else if (key == "metallicMap") m.MetallicMapPath = str();
    else if (key == "roughnessMap") m.RoughnessMapPath = str();
    else if (key == "aoMap") m.AOMapPath = str();
    else if (key == "emissiveMap") m.EmissiveMap = str();
    else if (key == "tilingX") m.Render.UVScaleX = f();
    else if (key == "tilingY") m.Render.UVScaleY = f();
    else if (key == "unlit") m.Render.Unlit = v.as<bool>();
    else if (key == "doubleSided") m.Render.Cull = v.as<bool>() ? CullFaces::None : CullFaces::Back;
    else if (key == "cutoff") m.Render.AlphaCutoff = f();
    else if (key == "nearest")
        m.Render.Filter = v.as<bool>() ? MaterialRender::Filtering::Nearest : MaterialRender::Filtering::Smooth;
    else throw std::runtime_error("material: неизвестное свойство '" + key + "'");
}

} // namespace

void ScriptEngine::RegisterProceduralApi() {
    // --- Шумы ------------------------------------------------------------------
    Bind("noise", "Seed", nullptr, [](int seed) { Global().Reseed((uint32_t)seed); });
    Bind("noise", "Perlin", nullptr, [](float x, float y, sol::optional<float> z) {
        return z ? Global().Perlin(x, y, *z) : Global().Perlin(x, y);
    });
    Bind("noise", "Simplex", nullptr, [](float x, float y, sol::optional<float> z) {
        return z ? Global().Simplex(x, y, *z) : Global().Simplex(x, y);
    });
    Bind("noise", "Value", nullptr, [](float x, float y, sol::optional<float> z) {
        return Global().Value(x, y, z.value_or(0.0f));
    });
    // f1, f2, ячейка — три значения: Lua забирает сколько нужно.
    Bind("noise", "Worley", nullptr, [](float x, float y, sol::optional<float> z) {
        float f1, f2, cell;
        Global().Worley(x, y, z.value_or(0.0f), f1, f2, cell);
        return std::make_tuple(f1, f2, cell);
    });
    auto fractal = [](auto fn) {
        return [fn](float x, float y, float z, sol::optional<sol::table> opts) {
            NoiseGen g;
            g.Src = Global();
            ApplyOptions(g, opts);
            return fn(g.Src, g.Type, g.P(x, y, z), g.Frac);
        };
    };
    Bind("noise", "Fbm", nullptr, fractal(sage::noise::Fbm));
    Bind("noise", "Ridged", nullptr, fractal(sage::noise::Ridged));
    Bind("noise", "Turbulence", nullptr, fractal(sage::noise::Turbulence));
    Bind("noise", "Warp", nullptr, [](const glm::vec3& p, float strength, sol::optional<sol::table> opts) {
        NoiseGen g;
        g.Src = Global();
        ApplyOptions(g, opts);
        return sage::noise::Warp(g.Src, g.Type, p, strength, g.Frac);
    });

    m_lua.new_usertype<NoiseGen>(
        "NoiseGenerator", sol::no_constructor,
        "Sample", [](const NoiseGen& g, float x, sol::optional<float> y, sol::optional<float> z) {
            const glm::vec3 p = g.P(x, y.value_or(0.0f), z.value_or(0.0f));
            return g.Src.Sample(g.Type, p.x * g.Frac.Frequency, p.y * g.Frac.Frequency, p.z * g.Frac.Frequency);
        },
        "Fbm", [](const NoiseGen& g, float x, sol::optional<float> y, sol::optional<float> z) {
            return sage::noise::Fbm(g.Src, g.Type, g.P(x, y.value_or(0.0f), z.value_or(0.0f)), g.Frac);
        },
        "Ridged", [](const NoiseGen& g, float x, sol::optional<float> y, sol::optional<float> z) {
            return sage::noise::Ridged(g.Src, g.Type, g.P(x, y.value_or(0.0f), z.value_or(0.0f)), g.Frac);
        },
        "Turbulence", [](const NoiseGen& g, float x, sol::optional<float> y, sol::optional<float> z) {
            return sage::noise::Turbulence(g.Src, g.Type, g.P(x, y.value_or(0.0f), z.value_or(0.0f)), g.Frac);
        },
        "Hash", [](const NoiseGen& g, int x, sol::optional<int> y, sol::optional<int> z) {
            return g.Src.Hash(x, y.value_or(0), z.value_or(0));
        },
        "seed", sol::property([](const NoiseGen& g) { return (int)g.Src.Seed(); },
                              [](NoiseGen& g, int s) { g.Src.Reseed((uint32_t)s); }),
        "octaves", sol::property([](const NoiseGen& g) { return g.Frac.Octaves; },
                                 [](NoiseGen& g, int v) { g.Frac.Octaves = v; }),
        "frequency", sol::property([](const NoiseGen& g) { return g.Frac.Frequency; },
                                   [](NoiseGen& g, float v) { g.Frac.Frequency = v; }),
        "lacunarity", sol::property([](const NoiseGen& g) { return g.Frac.Lacunarity; },
                                    [](NoiseGen& g, float v) { g.Frac.Lacunarity = v; }),
        "gain", sol::property([](const NoiseGen& g) { return g.Frac.Gain; },
                              [](NoiseGen& g, float v) { g.Frac.Gain = v; }),
        "warp", &NoiseGen::WarpStrength);
    // Генератор со своими настройками: { type, seed, octaves, frequency,
    // lacunarity, gain, warp }.
    Bind("noise", "New", nullptr, [](sol::optional<sol::table> opts) {
        NoiseGen g;
        ApplyOptions(g, opts);
        return g;
    });

    // --- Математика сверх math --------------------------------------------------
    Bind("math", "Remap", nullptr, [](float v, float a0, float a1, float b0, float b1) {
        const float d = a1 - a0;
        return b0 + (std::fabs(d) > 1e-12f ? (v - a0) / d : 0.0f) * (b1 - b0);
    });
    Bind("math", "InverseLerp", nullptr, [](float a, float b, float v) {
        return std::fabs(b - a) > 1e-12f ? (v - a) / (b - a) : 0.0f;
    });
    Bind("math", "Fract", nullptr, [](float x) { return x - std::floor(x); });
    Bind("math", "Wrap", nullptr, [](float x, float lo, float hi) {
        const float r = hi - lo;
        return r > 0.0f ? lo + (x - lo) - r * std::floor((x - lo) / r) : lo;
    });
    Bind("math", "PingPong", nullptr, [](float t, float len) {
        if (len <= 0.0f) return 0.0f;
        const float m = std::fmod(std::fabs(t), len * 2.0f);
        return m > len ? len * 2.0f - m : m;
    });
    Bind("math", "Step", nullptr, [](float edge, float x) { return x < edge ? 0.0f : 1.0f; });
    Bind("math", "SmootherStep", nullptr, [](float e0, float e1, float x) {
        const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
        return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
    });
    Bind("math", "Hash", nullptr, [](int x, sol::optional<int> y, sol::optional<int> z, sol::optional<int> seed) {
        return sage::noise::Source((uint32_t)seed.value_or(0)).Hash(x, y.value_or(0), z.value_or(0));
    });

    m_lua.new_usertype<Rng>(
        "Rng", sol::no_constructor,
        "Float", [](Rng& r) { return r.Float(); },
        "Range", [](Rng& r, double lo, double hi) { return lo + (hi - lo) * r.Float(); },
        "Int", [](Rng& r, int lo, int hi) {
            if (hi < lo) std::swap(lo, hi);
            return lo + (int)(r.Next() % (uint64_t)(hi - lo + 1));
        },
        "Chance", [](Rng& r, double p) { return r.Float() < p; },
        "Seed", [](Rng& r, double seed) { r.Seed((uint64_t)seed); });
    // Свой генератор с зерном: sage.math.Rng(42):Range(0, 10).
    Bind("math", "Rng", nullptr, [](sol::optional<double> seed) { return Rng((uint64_t)seed.value_or(0)); });

    // ФОРМУЛА ИЗ СТРОКИ: sage.math.Formula("sin(x) * y + noise(x, y)", "x, y")
    // -> функция. Компилируется один раз (Lua load), а не разбирается на
    // каждом вызове, и видит все функции math и шумов без префикса. Так
    // формулы можно хранить в настройках и править без перекомпиляции игры.
    m_lua.script(R"LUA(
        -- Имена в формуле пишут как в учебнике — clamp, fbm, lerp, — а функции
        -- движка названы с заглавной (sage.math.Clamp): ищем обе формы.
        local function lookup(k)
            local v = math[k]
            if v ~= nil or type(k) ~= "string" then return v end
            local up = k:sub(1, 1):upper() .. k:sub(2)
            for _, lib in ipairs({ sage.math, sage.noise }) do
                v = lib[k]; if v == nil then v = lib[up] end
                if v ~= nil then return v end
            end
            return _G[k]
        end
        sage.math.Formula = function(expr, args)
            args = args or "x, y, z, t"
            -- Своё окружение на формулу: присваивание внутри одной не меняет
            -- соседние и глобалы игры.
            local env = setmetatable({ pi = math.pi,
                noise = function(...) return sage.noise.Perlin(...) end },
                { __index = function(_, k) return lookup(k) end })
            local chunk, err = load("return function(" .. args .. ") return " .. expr .. " end",
                                    "formula", "t", env)
            if not chunk then error("Formula: " .. tostring(err), 2) end
            return chunk()
        end
    )LUA");

    // --- Меш, собранный кодом ---------------------------------------------------
    m_lua.new_usertype<MeshBuilder>(
        "MeshBuilder", sol::no_constructor,
        "Vertex", [](MeshBuilder& b, const glm::vec3& p, sol::optional<glm::vec3> n, sol::optional<glm::vec2> uv) {
            return (int)b.AddVertex(p, n.value_or(glm::vec3(0.0f)), uv.value_or(glm::vec2(0.0f))) + 1;
        },
        "Triangle", [](MeshBuilder& b, int a, int c, int d) {
            if (!b.AddTriangle((unsigned)(a - 1), (unsigned)(c - 1), (unsigned)(d - 1)))
                throw std::runtime_error("mesh:Triangle: номер вершины вне меша");
        },
        "Quad", [](MeshBuilder& b, int a, int c, int d, int e) {
            if (!b.AddQuad((unsigned)(a - 1), (unsigned)(c - 1), (unsigned)(d - 1), (unsigned)(e - 1)))
                throw std::runtime_error("mesh:Quad: номер вершины вне меша");
        },
        "Material", [](MeshBuilder& b, int slot) { b.UseMaterial(slot - 1); },
        "VertexCount", [](const MeshBuilder& b) { return (int)b.VertexCount(); },
        "TriangleCount", [](const MeshBuilder& b) { return (int)b.TriangleCount(); },
        "GetPosition", [](const MeshBuilder& b, int i) {
            if (i < 1 || (size_t)i > b.VertexCount()) throw std::runtime_error("mesh: нет вершины " + std::to_string(i));
            return b.At((unsigned)(i - 1)).Position;
        },
        "SetPosition", [](MeshBuilder& b, int i, const glm::vec3& p) {
            if (i >= 1 && (size_t)i <= b.VertexCount()) b.At((unsigned)(i - 1)).Position = p;
        },
        "SetNormal", [](MeshBuilder& b, int i, const glm::vec3& n) {
            if (i >= 1 && (size_t)i <= b.VertexCount()) b.At((unsigned)(i - 1)).Normal = n;
        },
        "SetUV", [](MeshBuilder& b, int i, const glm::vec2& uv) {
            if (i >= 1 && (size_t)i <= b.VertexCount()) b.At((unsigned)(i - 1)).TexCoords = uv;
        },
        "RecalculateNormals", [](MeshBuilder& b, sol::optional<bool> smooth) { b.RecalculateNormals(smooth.value_or(true)); },
        "Deform", [](MeshBuilder& b, sol::protected_function fn) {
            b.Deform([&fn](const glm::vec3& p, const glm::vec3& n, const glm::vec2& uv) {
                sol::protected_function_result r = fn(p, n, uv);
                if (!r.valid()) {
                    sol::error e = r;
                    throw std::runtime_error(std::string("mesh:Deform: ") + e.what());
                }
                return r.get_type() == sol::type::userdata ? r.get<glm::vec3>() : p;
            });
        },
        "Translate", [](MeshBuilder& b, const glm::vec3& d) { b.Translate(d); },
        "Scale", [](MeshBuilder& b, sol::object s) {
            b.Scale(s.is<glm::vec3>() ? s.as<glm::vec3>() : glm::vec3((float)s.as<double>()));
        },
        "Append", [](MeshBuilder& b, const MeshBuilder& other) { b.Append(other); },
        "Clear", [](MeshBuilder& b) { b.Clear(); },
        "Bounds", [](const MeshBuilder& b) {
            glm::vec3 lo, hi;
            b.Bounds(lo, hi);
            return std::make_tuple(lo, hi);
        },
        // Поставить меш объекту: то, что видно в кадре, и то, по чему щёлкают
        // мышью и строят коллайдер-меш. В файл сцены процедурный меш не
        // пишется — его строит скрипт при старте (или сохраните :Save).
        // Объект — любой из трёх видов, что видит скрипт: объект нового API,
        // self скрипта (его gameObject) или GameObject прежнего.
        "Apply", [](const MeshBuilder& b, const sol::object& target) {
            GameObject obj;
            if (target.is<sage::scripting::lua::ObjectRef>()) obj = target.as<sage::scripting::lua::ObjectRef>().Obj;
            else if (target.is<GameObject>()) obj = target.as<GameObject>();
            else if (target.get_type() == sol::type::table) {
                sol::object go = target.as<sol::table>()["gameObject"];
                if (go.is<sage::scripting::lua::ObjectRef>()) obj = go.as<sage::scripting::lua::ObjectRef>().Obj;
            }
            if (!obj.Valid()) throw std::runtime_error("mesh:Apply: нужен объект сцены");
            const sage::render::MeshData d = b.ToMeshData();
            if (d.Empty()) throw std::runtime_error("mesh:Apply: в меше нет треугольников");
            MeshRendererComponent& mr = obj.EnsureRenderer();
            mr.Ref = MeshRef{MeshRef::Type::None, ""};
            mr.MeshPtr = std::make_shared<Mesh>(d.Vertices, d.Indices, d.Submeshes, true);
        },
        // В .sagemesh — дальше это обычная модель проекта.
        "Save", [](const MeshBuilder& b, const std::string& path) {
            std::string err;
            if (!sage::assets::WriteMeshFile(path, b.ToMeshData(), {}, err))
                throw std::runtime_error("mesh:Save: " + err);
            return true;
        });
    Bind("mesh", "New", nullptr, []() { return MeshBuilder(); });
    Bind("mesh", "Grid", nullptr, [](float sx, float sz, int rx, int rz, sol::optional<sol::protected_function> h) {
        if (!h) return MeshBuilder::Grid(sx, sz, rx, rz);
        sol::protected_function fn = *h;
        return MeshBuilder::Grid(sx, sz, rx, rz, [&fn](float x, float z) {
            sol::protected_function_result r = fn(x, z);
            if (!r.valid()) {
                sol::error e = r;
                throw std::runtime_error(std::string("mesh.Grid: ") + e.what());
            }
            return r.get_type() == sol::type::number ? r.get<float>() : 0.0f;
        });
    });
    Bind("mesh", "Box", nullptr, [](sol::object size) {
        return MeshBuilder::Box(size.is<glm::vec3>() ? size.as<glm::vec3>() : glm::vec3((float)size.as<double>()));
    });
    Bind("mesh", "Sphere", nullptr, [](float r, sol::optional<int> seg, sol::optional<int> rings) {
        return MeshBuilder::Sphere(r, seg.value_or(32), rings.value_or(16));
    });
    Bind("mesh", "Cylinder", nullptr, [](float r, float h, sol::optional<int> seg, sol::optional<bool> caps) {
        return MeshBuilder::Cylinder(r, h, seg.value_or(32), caps.value_or(true));
    });

    // --- Материал, созданный кодом ----------------------------------------------
    //
    // Имя — как путь: им назначают материал (sage.render.SetMaterial(obj, имя))
    // и правят его (sage.material.Set). Повторный Create с тем же именем
    // ЗАМЕНЯЕТ свойства у того же материала — объекты, которым он назначен,
    // меняют вид сразу.
    Bind("material", "Create", nullptr, [](const std::string& name, sol::optional<sol::table> props) {
        if (name.empty()) throw std::runtime_error("material.Create: имя не может быть пустым");
        auto m = std::make_shared<Material>();
        if (props)
            for (const auto& [k, v] : *props) SetMaterialProperty(*m, k.as<std::string>(), v);
        ResourceManager::Instance().RegisterMaterial(name, m);
        return name;
    });
    Bind("material", "Set", nullptr, [](const std::string& name, sol::object propsOrKey, sol::optional<sol::object> value) {
        std::shared_ptr<Material> m = MaterialByName(name);
        if (propsOrKey.is<sol::table>()) {
            for (const auto& [k, v] : propsOrKey.as<sol::table>()) SetMaterialProperty(*m, k.as<std::string>(), v);
        } else if (value) {
            SetMaterialProperty(*m, propsOrKey.as<std::string>(), *value);
        }
        ResourceManager::Instance().RefreshMaterialTextures(*m);
    });
    Bind("material", "Save", nullptr, [](const std::string& name, const std::string& path) {
        MaterialByName(name)->SaveToFile(path);
        return true;
    });

    // --- Текстура, нарисованная кодом -------------------------------------------
    m_lua.new_usertype<TextureCanvas>(
        "TextureCanvas", sol::no_constructor,
        "width", sol::readonly(&TextureCanvas::W),
        "height", sol::readonly(&TextureCanvas::H),
        "nearest", &TextureCanvas::Nearest,
        // Пиксели — с нуля: это координаты картинки, а не номера в списке.
        "SetPixel", [](TextureCanvas& c, int x, int y, sol::object color) { c.Set(x, y, ToColor(color)); },
        "GetPixel", [](const TextureCanvas& c, int x, int y) { return c.Get(x, y); },
        // fn(u, v, x, y) -> цвет; u, v — в долях 0..1 по центрам пикселей.
        "Fill", [](TextureCanvas& c, sol::protected_function fn) {
            for (int y = 0; y < c.H; ++y)
                for (int x = 0; x < c.W; ++x) {
                    sol::protected_function_result r =
                        fn(((float)x + 0.5f) / (float)c.W, ((float)y + 0.5f) / (float)c.H, x, y);
                    if (!r.valid()) {
                        sol::error e = r;
                        throw std::runtime_error(std::string("texture:Fill: ") + e.what());
                    }
                    c.Set(x, y, ToColor(r.get<sol::object>()));
                }
        },
        // Загрузить на видеокарту под своим именем — теперь его можно ставить
        // в материал (texture = имя).
        "Apply", [](const TextureCanvas& c) {
            auto tex = std::make_shared<Texture>(c.Px.data(), c.W, c.H,
                                                 c.Nearest ? TextureFilter::Nearest : TextureFilter::Trilinear,
                                                 !c.Nearest, true);
            ResourceManager::Instance().RegisterTexture(c.Name, tex);
            return c.Name;
        });
    Bind("texture", "New", nullptr, [](const std::string& name, int w, int h) {
        if (name.empty()) throw std::runtime_error("texture.New: имя не может быть пустым");
        if (w < 1 || h < 1 || w > 8192 || h > 8192)
            throw std::runtime_error("texture.New: размер от 1 до 8192");
        TextureCanvas c;
        c.Name = name;
        c.W = w;
        c.H = h;
        c.Px.assign((size_t)w * h * 4, 255);
        return c;
    });
}
