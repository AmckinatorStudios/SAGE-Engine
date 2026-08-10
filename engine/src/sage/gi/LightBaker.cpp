#include "sage/gi/GI.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <thread>

#include <glm/gtc/constants.hpp>

#include "sage/core/Log.h"
#include "sage/gi/BVH.h"
#include "sage/gi/LightmapUV.h"
#include "sage/scene/Scene.h"

// Реализации stb живут в Texture.cpp (image) и Screenshot.cpp (image_write) —
// здесь только заголовки: чтение/запись HDR-страниц атласа лайтмап.
#include <stb_image.h>
#include <stb_image_write.h>

// ---------------------------------------------------------------------------
// LightBaker — CPU path tracer запечённого GI (см. обзор в GI.h).
//
// Конвенция значений: и лайтмапа, и пробы хранят «непрямую освещённость / π» —
// величину, которой в шейдере напрямую умножается albedo (та же роль, что у
// полусферического ambient в PbrShader). При косинус-взвешенном сэмплинге она
// равна простому среднему радианса по полусфере: E/π = avg(L). Прямой свет
// первой поверхности НЕ запекается — он остаётся realtime (динамические тени
// и спекуляр), поэтому двойного счёта нет: запекается только то, что заменяет
// прежний однотонный ambient.
//
// Параллелизм — собственные потоки (std::thread), НЕ движковый JobSystem:
// бейк запускается редактором в фоне одновременно с рендером, а два
// конкурентных ParallelFor на общем пуле — гонка за воркеров. Каждый поток
// пишет только свои тексели/пробы — гонок нет; сэмплинг детерминирован
// (счётчик Уэлча по позиции текселя + Seed), результат не зависит от числа
// потоков.
// ---------------------------------------------------------------------------
namespace sage::gi {

namespace {

constexpr float kRayEps = 2e-3f;  // отступ от поверхности против self-hit

// --- Детерминированный RNG: PCG-хэш + xorshift ---
inline uint32_t Hash(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

struct Rng {
    uint32_t State;
    explicit Rng(uint32_t seed) : State(Hash(seed | 1u)) {}
    uint32_t Next() {
        State ^= State << 13; State ^= State >> 17; State ^= State << 5;
        return State;
    }
    float Next01() { return (Next() >> 8) * (1.0f / 16777216.0f); } // [0,1)
};

// Ортонормальный базис вокруг n (метод Duff et al.).
inline void BuildBasis(const glm::vec3& n, glm::vec3& t, glm::vec3& b) {
    float sign = n.z >= 0.0f ? 1.0f : -1.0f;
    float a = -1.0f / (sign + n.z);
    t = glm::vec3(1.0f + sign * n.x * n.x * a, sign * n.x * n.y * a, -sign * n.x);
    b = glm::vec3(n.x * n.y * a, sign + n.y * n.y * a, -n.y);
}

// Косинус-взвешенное направление в полусфере вокруг n (pdf = cosθ/π).
inline glm::vec3 CosineSample(const glm::vec3& n, Rng& rng) {
    float u1 = rng.Next01(), u2 = rng.Next01();
    float r = std::sqrt(u1);
    float phi = 2.0f * glm::pi<float>() * u2;
    glm::vec3 t, b;
    BuildBasis(n, t, b);
    float z = std::sqrt(std::max(0.0f, 1.0f - u1));
    return glm::normalize(t * (r * std::cos(phi)) + b * (r * std::sin(phi)) + n * z);
}

// Равномерное направление на сфере.
inline glm::vec3 UniformSphere(Rng& rng) {
    float z = rng.Next01() * 2.0f - 1.0f;
    float phi = 2.0f * glm::pi<float>() * rng.Next01();
    float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
    return glm::vec3(r * std::cos(phi), z, r * std::sin(phi));
}

// Радианс неба по направлению — согласован с полусферическим ambient рендера
// (ResolveAmbient), чтобы запечённая и realtime картинки не расходились.
inline glm::vec3 SkyRadiance(const LightingEnvironment& env, const glm::vec3& dir) {
    glm::vec3 sky, ground;
    env.ResolveAmbient(sky, ground);
    float w = dir.y * 0.5f + 0.5f;
    return glm::mix(ground, sky, w) * env.AmbientStrength;
}

// Прямой свет в точке поверхности (Ламберт, с теневыми лучами): Σ по
// источникам radiance·cosθ·затухание·видимость. Формулы затухания/конуса — те
// же, что в PbrShader (единый источник правды о модели света).
glm::vec3 DirectLight(const BVH& bvh, const LightingEnvironment& env,
                      const glm::vec3& p, const glm::vec3& n) {
    glm::vec3 sum(0.0f);
    glm::vec3 orig = p + n * kRayEps;

    // Солнце (направленный, без затухания).
    glm::vec3 sunL = -glm::normalize(env.Sun.Direction);
    float ndl = glm::dot(n, sunL);
    if (ndl > 0.0f && env.Sun.Intensity > 0.0f) {
        if (!bvh.Occluded(orig, sunL, 1e9f))
            sum += env.Sun.Color * env.Sun.Intensity * ndl;
    }

    for (const PointLight& l : env.PointLights) {
        glm::vec3 tl = l.Position - p;
        float dist = glm::length(tl);
        if (dist < 1e-4f) continue;
        glm::vec3 L = tl / dist;
        float cosT = glm::dot(n, L);
        if (cosT <= 0.0f) continue;
        float att = 1.0f / (l.Constant() + l.Linear() * dist + l.Quadratic() * dist * dist);
        if (bvh.Occluded(orig, L, dist - kRayEps)) continue;
        sum += l.Color * l.Intensity * att * cosT;
    }

    for (const SpotLight& l : env.SpotLights) {
        glm::vec3 tl = l.Position - p;
        float dist = glm::length(tl);
        if (dist < 1e-4f) continue;
        glm::vec3 L = tl / dist;
        float cosT = glm::dot(n, L);
        if (cosT <= 0.0f) continue;
        float theta = glm::dot(L, -glm::normalize(l.Direction));
        float eps = std::max(l.CosInner() - l.CosOuter(), 1e-4f);
        float cone = glm::clamp((theta - l.CosOuter()) / eps, 0.0f, 1.0f);
        if (cone <= 0.0f) continue;
        float att = 1.0f / (l.Constant() + l.Linear() * dist + l.Quadratic() * dist * dist);
        if (bvh.Occluded(orig, L, dist - kRayEps)) continue;
        sum += l.Color * l.Intensity * att * cone * cosT;
    }
    return sum;
}

// Радианс, ПРИХОДЯЩИЙ вдоль луча (не считая прямого света в точке старта):
// у каждой встреченной поверхности учитывается её прямой свет (это и есть
// 1-й отскок GI), дальше — косинус-продолжение до maxBounces. Промах — небо.
glm::vec3 TraceRadiance(const BVH& bvh, const LightingEnvironment& env,
                        glm::vec3 orig, glm::vec3 dir, int maxBounces, Rng& rng) {
    glm::vec3 L(0.0f);
    glm::vec3 throughput(1.0f);
    for (int bounce = 1; bounce <= maxBounces; ++bounce) {
        RayHit hit;
        if (!bvh.Raycast(orig, dir, 1e9f, hit)) {
            L += throughput * SkyRadiance(env, dir);
            break;
        }
        if (hit.BackFace) break; // изнанка (внутри геометрии) — темно

        // Исходящий диффузный радианс поверхности от прямого света.
        glm::vec3 direct = DirectLight(bvh, env, hit.Pos, hit.N);
        L += throughput * hit.Albedo * direct * (1.0f / glm::pi<float>());

        if (bounce == maxBounces) break;
        // Косинус-сэмплинг: BRDF·cos/pdf = albedo — throughput умножается ровно на него.
        throughput *= hit.Albedo;
        orig = hit.Pos + hit.N * kRayEps;
        dir = CosineSample(hit.N, rng);
    }
    return L;
}

// «Непрямая освещённость / π» в точке поверхности: среднее TraceRadiance по
// косинус-взвешенной полусфере (E/π = avg(L) при pdf = cosθ/π).
glm::vec3 GatherSurface(const BVH& bvh, const LightingEnvironment& env,
                        const glm::vec3& p, const glm::vec3& n,
                        int samples, int bounces, uint32_t seed) {
    Rng rng(seed);
    glm::vec3 acc(0.0f);
    glm::vec3 orig = p + n * kRayEps;
    for (int s = 0; s < samples; ++s)
        acc += TraceRadiance(bvh, env, orig, CosineSample(n, rng), bounces, rng);
    return acc / (float)samples;
}

// Локальный parallel-for на собственных потоках (см. комментарий модуля).
void ParallelChunks(size_t count, const std::function<void(size_t, size_t)>& body) {
    unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    size_t chunk = (count + hw - 1) / hw;
    if (count < 64 || hw == 1) { body(0, count); return; }
    std::vector<std::thread> threads;
    for (size_t begin = 0; begin < count; begin += chunk) {
        size_t end = std::min(begin + chunk, count);
        threads.emplace_back(body, begin, end);
    }
    for (auto& t : threads) t.join();
}

// Компоновка лайтмап: чарты каждой сущности -> мини-атлас сущности -> страницы.
// ДЕТЕРМИНИРОВАНА и вынесена отдельно: тот же код гоняет и Bake, и
// RebuildAfterLoad (восстановление UV под сохранённые страницы).
struct ItemBake {
    const StaticItem* Item;
    sage::render::MeshData Mesh;   // копия с дублированными вершинами и UV2
    std::vector<Chart> Charts;
};

int BuildLayout(const BakeInput& input, int pageSize, std::vector<ItemBake>& bakes) {
    const GISettings& s = input.Settings;
    std::vector<Chart> entityRects; // прямоугольник каждой сущности как псевдо-чарт
    for (const StaticItem& item : input.Items) {
        if (!item.Lightmapped || item.Mesh.Empty()) continue;
        ItemBake ib;
        ib.Item = &item;
        ib.Mesh = item.Mesh;
        float density = (float)s.TexelsPerUnit * item.TexelScale;
        ib.Charts = BuildCharts(ib.Mesh, item.World, density, pageSize / 2);
        if (ib.Charts.empty()) continue;

        // Мини-упаковка чартов сущности в собственный прямоугольник, чтобы вся
        // сущность жила на ОДНОЙ странице (у отрисовки один сэмплер лайтмапы).
        // Сторона подбирается от суммарной площади; при переполнении растёт.
        long area = 0;
        int widest = 1, tallest = 1;
        for (const Chart& c : ib.Charts) {
            area += (long)c.W * c.H;
            widest = std::max(widest, c.W);
            tallest = std::max(tallest, c.H);
        }
        int side = std::max({widest, tallest, (int)std::ceil(std::sqrt((double)area) * 1.15)});
        side = std::min(side, pageSize);
        std::vector<Chart*> ptrs;
        for (Chart& c : ib.Charts) ptrs.push_back(&c);
        while (PackCharts(ptrs, side) > 1 && side < pageSize)
            side = std::min(side * 3 / 2 + 1, pageSize);
        // Использованный bbox мини-атласа — прямоугольник сущности.
        int usedW = 0, usedH = 0;
        for (const Chart& c : ib.Charts) {
            usedW = std::max(usedW, c.X + c.W);
            usedH = std::max(usedH, c.Y + c.H);
        }
        Chart rect;
        rect.W = usedW; rect.H = usedH;
        rect.Tris = {(int)bakes.size()}; // ссылка на ItemBake по индексу
        entityRects.push_back(rect);
        bakes.push_back(std::move(ib));
    }

    std::vector<Chart*> rectPtrs;
    for (Chart& r : entityRects) rectPtrs.push_back(&r);
    int pageCount = PackCharts(rectPtrs, pageSize);

    // Финальные позиции чартов = позиция прямоугольника сущности + локальная.
    for (Chart& rect : entityRects) {
        ItemBake& ib = bakes[rect.Tris[0]];
        for (Chart& c : ib.Charts) {
            c.Page = rect.Page;
            c.X += rect.X;
            c.Y += rect.Y;
        }
        ApplyAtlasTransform(ib.Mesh, ib.Charts, pageSize);
    }
    return pageCount;
}

// Тексель лайтмапы, подлежащий запеканию.
struct TexelJob {
    int Page, X, Y;
    glm::vec3 Pos, N;
};

// Растеризация чарта в задания текселей: для каждого текселя, покрытого
// треугольником чарта (с небольшим запасом на краях — консервативно),
// барицентрически интерполируются мировая позиция и нормаль.
void RasterizeChart(const StaticItem& item, const sage::render::MeshData& mesh,
                    const Chart& chart, int pageSize,
                    std::vector<uint8_t>& covered, std::vector<TexelJob>& jobs) {
    const glm::mat3 nrm = glm::transpose(glm::inverse(glm::mat3(item.World)));
    const int pad = kChartPadding;
    const glm::vec2 inner((float)(chart.W - 2 * pad), (float)(chart.H - 2 * pad));

    for (int t : chart.Tris) {
        glm::vec3 wp[3], wn[3];
        glm::vec2 q[3]; // координаты в текселях страницы
        for (int k = 0; k < 3; ++k) {
            const Vertex& v = mesh.Vertices[mesh.Indices[t * 3 + k]];
            wp[k] = glm::vec3(item.World * glm::vec4(v.Position, 1.0f));
            wn[k] = nrm * v.Normal;
            glm::vec2 proj = ProjectAxis(wp[k], chart.Axis);
            glm::vec2 local = (proj - chart.WorldMin) / chart.WorldSize;
            q[k] = glm::vec2((float)chart.X + pad, (float)chart.Y + pad) + local * inner;
        }
        float area = (q[1].x - q[0].x) * (q[2].y - q[0].y) -
                     (q[2].x - q[0].x) * (q[1].y - q[0].y);
        if (std::abs(area) < 1e-6f) continue; // треугольник ребром к проекции
        float invArea = 1.0f / area;

        int x0 = std::max(chart.X, (int)std::floor(std::min({q[0].x, q[1].x, q[2].x})) - 1);
        int x1 = std::min(chart.X + chart.W - 1, (int)std::ceil(std::max({q[0].x, q[1].x, q[2].x})) + 1);
        int y0 = std::max(chart.Y, (int)std::floor(std::min({q[0].y, q[1].y, q[2].y})) - 1);
        int y1 = std::min(chart.Y + chart.H - 1, (int)std::ceil(std::max({q[0].y, q[1].y, q[2].y})) + 1);

        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                size_t ci = (size_t)y * pageSize + x;
                if (covered[ci]) continue;
                glm::vec2 pc((float)x + 0.5f, (float)y + 0.5f);
                float b0 = ((q[1].x - pc.x) * (q[2].y - pc.y) - (q[2].x - pc.x) * (q[1].y - pc.y)) * invArea;
                float b1 = ((q[2].x - pc.x) * (q[0].y - pc.y) - (q[0].x - pc.x) * (q[2].y - pc.y)) * invArea;
                float b2 = 1.0f - b0 - b1;
                // Запас 0.35 текселя за краем: крайние тексели получают валидную
                // позицию (клампится в треугольник ниже) — без чёрной каймы.
                const float margin = -0.35f;
                if (b0 < margin || b1 < margin || b2 < margin) continue;
                float c0 = std::max(b0, 0.0f), c1 = std::max(b1, 0.0f), c2 = std::max(b2, 0.0f);
                float norm = c0 + c1 + c2;
                if (norm < 1e-6f) continue;
                c0 /= norm; c1 /= norm; c2 /= norm;
                TexelJob job;
                job.Page = chart.Page; job.X = x; job.Y = y;
                job.Pos = wp[0] * c0 + wp[1] * c1 + wp[2] * c2;
                glm::vec3 n = wn[0] * c0 + wn[1] * c1 + wn[2] * c2;
                job.N = glm::dot(n, n) > 1e-12f ? glm::normalize(n) : glm::vec3(0, 1, 0);
                covered[ci] = 1;
                jobs.push_back(job);
            }
        }
    }
}

// Дилатация страницы: непокрытые тексели заполняются средним покрытых соседей
// (несколько итераций) — билинейная выборка у краёв чартов не тянет чёрное.
void DilatePage(LightmapPage& page, int iterations) {
    const int size = page.Size;
    std::vector<uint8_t> cov = page.Coverage;
    std::vector<uint8_t> covNext = cov;
    for (int it = 0; it < iterations; ++it) {
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                size_t i = (size_t)y * size + x;
                if (cov[i]) continue;
                glm::vec3 acc(0.0f);
                int cnt = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= size || ny >= size) continue;
                        size_t ni = (size_t)ny * size + nx;
                        if (!cov[ni]) continue;
                        acc += page.Texels[ni];
                        ++cnt;
                    }
                }
                if (cnt > 0) {
                    page.Texels[i] = acc / (float)cnt;
                    covNext[i] = 1;
                }
            }
        }
        cov = covNext;
    }
}

} // namespace

// --- SH (L1): базис Y00=0.282095, Y1*=0.488603·(x|y|z) --------------------
void SHAddSample(SH1& sh, const glm::vec3& dir, const glm::vec3& radiance, float weight) {
    const float y0 = 0.282095f;
    const float y1 = 0.488603f;
    glm::vec4 basis(y0, y1 * dir.x, y1 * dir.y, y1 * dir.z);
    sh.R += basis * (radiance.r * weight);
    sh.G += basis * (radiance.g * weight);
    sh.B += basis * (radiance.b * weight);
}

glm::vec3 SHEvalIrradianceOverPi(const SH1& sh, const glm::vec3& n) {
    // E(n) = Â0·c0·Y00 + Â1·Y1·(c·n), Â0 = π, Â1 = 2π/3; здесь сразу E/π.
    const float w0 = 0.282095f;               // Y00 (множитель π сокращён)
    const float w1 = (2.0f / 3.0f) * 0.488603f;
    glm::vec4 basis(w0, w1 * n.x, w1 * n.y, w1 * n.z);
    glm::vec3 v(glm::dot(sh.R, basis), glm::dot(sh.G, basis), glm::dot(sh.B, basis));
    return glm::max(v, glm::vec3(0.0f));
}

std::shared_ptr<GIState> Bake(const BakeInput& input, const ProgressFn& progress) {
    auto report = [&](float f, const char* phase) { if (progress) progress(f, phase); };
    auto state = std::make_shared<GIState>();
    state->Settings = input.Settings;
    state->GeometryHash = input.GeometryHash;

    const GISettings& s = input.Settings;
    const int pageSize = std::max(64, s.AtlasSize);

    // --- 1. Треугольный суп мира + BVH (окклюдеры — ВСЯ статика) -----------
    report(0.0f, "Сбор геометрии");
    std::vector<Tri> tris;
    for (const StaticItem& item : input.Items) {
        for (size_t i = 0; i + 2 < item.Mesh.Indices.size(); i += 3) {
            Tri t;
            t.A = glm::vec3(item.World * glm::vec4(item.Mesh.Vertices[item.Mesh.Indices[i]].Position, 1.0f));
            t.B = glm::vec3(item.World * glm::vec4(item.Mesh.Vertices[item.Mesh.Indices[i + 1]].Position, 1.0f));
            t.C = glm::vec3(item.World * glm::vec4(item.Mesh.Vertices[item.Mesh.Indices[i + 2]].Position, 1.0f));
            glm::vec3 n = glm::cross(t.B - t.A, t.C - t.A);
            float len = glm::length(n);
            if (len < 1e-12f) continue; // вырожденный
            // Геометрическая нормаль согласуется с вершинными (FixWinding рендера
            // делает то же самое): изнанка/лицо определяются одинаково.
            glm::vec3 vn = item.Mesh.Vertices[item.Mesh.Indices[i]].Normal +
                           item.Mesh.Vertices[item.Mesh.Indices[i + 1]].Normal +
                           item.Mesh.Vertices[item.Mesh.Indices[i + 2]].Normal;
            t.N = n / len;
            if (glm::dot(t.N, vn) < 0.0f) t.N = -t.N;
            t.Albedo = item.Albedo;
            tris.push_back(t);
        }
    }
    BVH bvh;
    bvh.Build(std::move(tris));
    LOG_INFO("GI") << "Бейк: " << bvh.TriangleCount() << " треугольников, "
                   << input.Items.size() << " статичных сущностей";

    // --- 2. Развёртка: чарты каждой сущности -> мини-атлас -> страницы ------
    report(0.05f, "Развёртка лайтмап");
    std::vector<ItemBake> bakes;
    int pageCount = BuildLayout(input, pageSize, bakes);

    state->Pages.resize(pageCount);
    for (LightmapPage& p : state->Pages) {
        p.Size = pageSize;
        p.Texels.assign((size_t)pageSize * pageSize, glm::vec3(0.0f));
        p.Coverage.assign((size_t)pageSize * pageSize, 0);
    }

    // --- 3. Растеризация чартов в задания текселей --------------------------
    report(0.1f, "Растеризация чартов");
    std::vector<TexelJob> jobs;
    for (ItemBake& ib : bakes) {
        for (const Chart& c : ib.Charts) {
            LightmapPage& page = state->Pages[c.Page];
            RasterizeChart(*ib.Item, ib.Mesh, c, pageSize, page.Coverage, jobs);
        }
    }
    LOG_INFO("GI") << "Бейк: " << jobs.size() << " текселей лайтмапы, "
                   << pageCount << " страниц(ы) " << pageSize << "x" << pageSize;

    // --- 4. Трассировка текселей (детерминированно, параллельно) ------------
    report(0.15f, "Трассировка лайтмап");
    std::atomic<size_t> done{0};
    ParallelChunks(jobs.size(), [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            const TexelJob& j = jobs[i];
            // Умножения — в БЕЗЗНАКОВОМ типе. В int они переполняются уже на
            // странице шириной в пару тысяч текселей, а знаковое переполнение
            // — неопределённое поведение: компилятор вправе считать, что его не
            // бывает, и посчитать хэш иначе при другом уровне оптимизации.
            // Для бейка, который обещан ДЕТЕРМИНИРОВАННЫМ, это означало бы
            // разный результат у двух сборок одного и того же кода.
            const uint32_t seed = Hash((uint32_t)j.Page * 73856093u ^
                                       (uint32_t)j.X * 19349663u ^
                                       (uint32_t)j.Y * 83492791u ^ s.Seed);
            glm::vec3 v = GatherSurface(bvh, input.Env, j.Pos, j.N,
                                        s.SampleCount, s.Bounces, seed);
            state->Pages[j.Page].Texels[(size_t)j.Y * pageSize + j.X] = v;
            size_t d = ++done;
            if (progress && (d & 0x3FF) == 0)
                progress(0.15f + 0.6f * (float)d / (float)jobs.size(), "Трассировка лайтмап");
        }
    });

    // --- 5. Дилатация страниц (кайма чартов) --------------------------------
    report(0.75f, "Дилатация");
    for (LightmapPage& p : state->Pages) DilatePage(p, 3);

    // --- 6. GI-объём: сетка L1 SH-проб --------------------------------------
    report(0.8f, "Световые пробы");
    {
        // AABB статики + запас в одну ячейку.
        glm::vec3 mn(std::numeric_limits<float>::max());
        glm::vec3 mx(-std::numeric_limits<float>::max());
        for (const StaticItem& item : input.Items) {
            for (const Vertex& v : item.Mesh.Vertices) {
                glm::vec3 w = glm::vec3(item.World * glm::vec4(v.Position, 1.0f));
                mn = glm::min(mn, w);
                mx = glm::max(mx, w);
            }
        }
        if (input.Items.empty()) { mn = glm::vec3(-5.0f); mx = glm::vec3(5.0f); }

        float cell = std::max(0.25f, s.ProbeCellSize);
        mn -= glm::vec3(cell * 0.5f);
        mx += glm::vec3(cell * 0.5f);
        glm::vec3 size = mx - mn;
        glm::ivec3 dims;
        for (int a = 0; a < 3; ++a)
            dims[a] = std::clamp((int)std::ceil(size[a] / cell) + 1, 2, std::max(2, s.MaxProbeAxis));
        glm::vec3 cellSize = size / glm::vec3(dims - glm::ivec3(1));

        ProbeVolume& vol = state->Probes;
        vol.Dims = dims;
        vol.Min = mn;
        vol.CellSize = cellSize;
        vol.Probes.assign((size_t)dims.x * dims.y * dims.z, SH1{});
        std::vector<uint8_t> valid((size_t)dims.x * dims.y * dims.z, 1);

        const int probeSamples = std::max(32, s.SampleCount);
        const float wQuad = 4.0f * glm::pi<float>() / (float)probeSamples;
        const float insideT = 0.5f * std::min({cellSize.x, cellSize.y, cellSize.z});

        ParallelChunks(vol.Probes.size(), [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                int x = (int)(i % dims.x);
                int y = (int)((i / dims.x) % dims.y);
                int z = (int)(i / ((size_t)dims.x * dims.y));
                glm::vec3 p = mn + glm::vec3(x, y, z) * cellSize;
                Rng rng(Hash((uint32_t)i * 2654435761u ^ s.Seed ^ 0x9E3779B9u));
                SH1 sh;
                int backHits = 0;
                for (int sIdx = 0; sIdx < probeSamples; ++sIdx) {
                    glm::vec3 dir = UniformSphere(rng);
                    RayHit hit;
                    if (bvh.Raycast(p, dir, 1e9f, hit) && hit.BackFace && hit.T < insideT)
                        ++backHits;
                    glm::vec3 L = TraceRadiance(bvh, input.Env, p, dir, s.Bounces, rng);
                    SHAddSample(sh, dir, L, wQuad);
                }
                // Проба внутри геометрии (много близких изнанок) — невалидна,
                // заполнится соседями ниже.
                if (backHits > probeSamples / 4) valid[i] = 0;
                else vol.Probes[i] = sh;
            }
        });

        // Невалидные пробы — среднее валидных 6-соседей (несколько итераций).
        for (int it = 0; it < 3; ++it) {
            std::vector<uint8_t> validNext = valid;
            for (int z = 0; z < dims.z; ++z)
                for (int y = 0; y < dims.y; ++y)
                    for (int x = 0; x < dims.x; ++x) {
                        size_t i = (size_t)vol.Index(x, y, z);
                        if (valid[i]) continue;
                        SH1 acc;
                        int cnt = 0;
                        const int off[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
                        for (auto& o : off) {
                            int nx = x + o[0], ny = y + o[1], nz = z + o[2];
                            if (nx < 0 || ny < 0 || nz < 0 || nx >= dims.x || ny >= dims.y || nz >= dims.z) continue;
                            size_t ni = (size_t)vol.Index(nx, ny, nz);
                            if (!valid[ni]) continue;
                            acc.R += vol.Probes[ni].R; acc.G += vol.Probes[ni].G; acc.B += vol.Probes[ni].B;
                            ++cnt;
                        }
                        if (cnt > 0) {
                            float inv = 1.0f / (float)cnt;
                            vol.Probes[i].R = acc.R * inv;
                            vol.Probes[i].G = acc.G * inv;
                            vol.Probes[i].B = acc.B * inv;
                            validNext[i] = 1;
                        }
                    }
            valid = validNext;
        }
    }

    // --- 7. Готовые данные по сущностям -------------------------------------
    for (ItemBake& ib : bakes) {
        EntityBake eb;
        eb.Page = ib.Charts.empty() ? 0 : ib.Charts.front().Page;
        eb.Mesh = std::move(ib.Mesh);
        state->Entities.emplace(ib.Item->EntityId, std::move(eb));
    }
    state->Baked = !state->Entities.empty();

    report(1.0f, "Готово");
    LOG_INFO("GI") << "Бейк завершён: " << state->Entities.size() << " сущностей с лайтмапами, объём проб "
                   << state->Probes.Dims.x << "x" << state->Probes.Dims.y << "x" << state->Probes.Dims.z;
    return state;
}

namespace {

// Путь страницы атласа: «<сцена без расширения>_gi_page<N>.hdr» рядом со сценой.
std::string PagePath(const std::string& scenePath, int page) {
    std::string stem = scenePath;
    size_t dot = stem.find_last_of('.');
    size_t slash = stem.find_last_of("/\\");
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        stem.resize(dot);
    return stem + "_gi_page" + std::to_string(page) + ".hdr";
}

} // namespace

void SavePages(const GIState& state, const std::string& scenePath) {
    for (size_t i = 0; i < state.Pages.size(); ++i) {
        const LightmapPage& p = state.Pages[i];
        if (p.Size <= 0) continue;
        std::string path = PagePath(scenePath, (int)i);
        // glm::vec3 плотно упакован — Texels.data() это Size*Size*3 float'ов.
        if (!stbi_write_hdr(path.c_str(), p.Size, p.Size, 3, (const float*)p.Texels.data()))
            LOG_ERROR("GI") << "Не удалось сохранить страницу лайтмапы: " << path;
        else
            LOG_INFO("GI") << "Лайтмапа сохранена: " << path;
    }
}

void RebuildAfterLoad(Scene& scene, const std::string& scenePath) {
    if (!scene.GI) return;
    GIState& st = *scene.GI;
    if (!st.Baked) return; // из файла пришли только настройки/пробы — нечего восстанавливать

    // Отпечаток текущей сцены обязан совпасть с сохранённым: иначе сцена
    // правилась после бейка и сохранённые страницы не соответствуют развёртке.
    uint64_t hash = ComputeGeometryHash(scene, st.Settings);
    if (hash != st.GeometryHash) {
        LOG_WARN("GI") << "Лайтмапы устарели (геометрия изменилась после бейка) — нужен повторный бейк";
        st.Baked = false;
        st.Pages.clear();
        st.Entities.clear();
        return;
    }

    // Развёртка детерминирована — пересчитываем UV под сохранённые страницы.
    BakeInput input = CollectBakeInput(scene, st.Settings);
    const int pageSize = std::max(64, st.Settings.AtlasSize);
    std::vector<ItemBake> bakes;
    int pageCount = BuildLayout(input, pageSize, bakes);

    st.Entities.clear();
    for (ItemBake& ib : bakes) {
        EntityBake eb;
        eb.Page = ib.Charts.empty() ? 0 : ib.Charts.front().Page;
        eb.Mesh = std::move(ib.Mesh);
        st.Entities.emplace(ib.Item->EntityId, std::move(eb));
    }

    // Страницы — из .hdr рядом с файлом сцены. ВАЖНО: глобальный флип stb
    // (включается загрузкой обычных текстур, см. Texture.cpp) здесь недопустим —
    // строки страницы соответствуют лайтмап-UV напрямую, без переворота.
    stbi_set_flip_vertically_on_load(false);
    st.Pages.clear();
    st.Pages.resize(pageCount);
    for (int i = 0; i < pageCount; ++i) {
        std::string path = PagePath(scenePath, i);
        int w = 0, h = 0, n = 0;
        float* data = stbi_loadf(path.c_str(), &w, &h, &n, 3);
        if (!data || w != pageSize || h != pageSize) {
            LOG_WARN("GI") << "Страница лайтмапы не найдена/не подходит (" << path
                           << ") — нужен повторный бейк";
            if (data) stbi_image_free(data);
            st.Baked = false;
            st.Pages.clear();
            st.Entities.clear();
            return;
        }
        LightmapPage& p = st.Pages[i];
        p.Size = pageSize;
        p.Texels.resize((size_t)pageSize * pageSize);
        std::memcpy(p.Texels.data(), data, sizeof(float) * 3 * p.Texels.size());
        p.Coverage.assign(p.Texels.size(), 1);
        stbi_image_free(data);
    }
    stbi_set_flip_vertically_on_load(true); // вернуть конвенцию обычных текстур
    LOG_INFO("GI") << "GI восстановлено: " << st.Entities.size() << " сущностей, "
                   << pageCount << " страниц(ы) лайтмап";
}

} // namespace sage::gi
