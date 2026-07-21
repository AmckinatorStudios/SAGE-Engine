#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "sage/render/MeshData.h"
#include "sage/rhi/Resources.h"
#include "sage/scene/Light.h"

class Scene;
class Mesh;

// ---------------------------------------------------------------------------
// sage/gi — запечённое глобальное освещение (GI) движка.
//
// Двухуровневая схема, как в современных движках:
//   • ЛАЙТМАПЫ для статичной геометрии (сущности с GIStaticComponent):
//     CPU path tracer (BVH + многоотскоковый непрямой свет + небо) запекает
//     НЕПРЯМУЮ освещённость в HDR-атласы; развёртка — чарты по доминантной
//     оси нормали, упакованные shelf-упаковщиком. Прямой свет (солнце/точечные/
//     прожекторы) остаётся realtime — динамические тени и спекуляр живут.
//   • GI-ОБЪЁМ (сетка световых проб, L1 сферические гармоники) для
//     динамических объектов: те же лучи запекаются в пробы, упакованные в
//     3D-текстуры; шейдер семплит объём per-pixel с аппаратной трилинейной
//     интерполяцией.
//
// Запекание — ЧИСТЫЙ CPU (JobSystem), без GL: BakeInput собирается на главном
// потоке из сцены, Bake() можно гнать в фоновом потоке и в headless-тестах.
// GPU-ресурсы (текстуры страниц/объёма, меши с лайтмап-UV) создаются лениво
// на первом кадре рендера (см. RenderBatch).
// ---------------------------------------------------------------------------
namespace sage::gi {

// Настройки запекания. Сериализуются со сценой; детерминируют развёртку UV,
// поэтому при загрузке сцены UV восстанавливаются пересчётом, а не хранением.
struct GISettings {
    int   TexelsPerUnit = 8;    // плотность лайтмапы (текселей на единицу мира)
    int   AtlasSize    = 1024;  // сторона страницы атласа лайтмап
    int   SampleCount  = 64;    // лучей на тексель/пробу
    int   Bounces      = 2;     // непрямых отскоков света
    float ProbeCellSize = 2.0f; // шаг сетки GI-проб, единицы мира
    int   MaxProbeAxis = 24;    // максимум проб вдоль одной оси
    uint32_t Seed      = 1337;  // база детерминированного сэмплинга

    bool operator==(const GISettings& o) const {
        return TexelsPerUnit == o.TexelsPerUnit && AtlasSize == o.AtlasSize &&
               SampleCount == o.SampleCount && Bounces == o.Bounces &&
               ProbeCellSize == o.ProbeCellSize && MaxProbeAxis == o.MaxProbeAxis &&
               Seed == o.Seed;
    }
};

// Страница атласа лайтмап: линейный HDR (значение — «непрямая освещённость,
// делённая на π», т.е. прямой множитель albedo — та же конвенция, что у
// полусферического ambient в PbrShader).
struct LightmapPage {
    int Size = 0;
    std::vector<glm::vec3> Texels;                 // Size*Size, row-major
    std::vector<uint8_t> Coverage;                 // 1 — тексель покрыт чартом
    std::unique_ptr<sage::rhi::Texture2D> Gpu;     // лениво (нужен GL-контекст)
};

// Запечённая сущность: копия её геометрии с заполненным UV2 (в ЛОКАЛЬНЫХ
// координатах — рендер как обычно умножает на мировую матрицу).
struct EntityBake {
    int Page = 0;
    sage::render::MeshData Mesh;      // UV2 → атлас страницы Page
    std::shared_ptr<::Mesh> GpuMesh;  // лениво (нужен GL-контекст)
};

// L1-гармоники на канал: vec4(c0, cx, cy, cz) — базис Y00, Y1x, Y1y, Y1z.
struct SH1 {
    glm::vec4 R{0.0f}, G{0.0f}, B{0.0f};
};

// Сетка световых проб (irradiance volume). Пробы лежат в узлах равномерной
// сетки: точка пробы (i,j,k) = Min + (i,j,k)*CellSize.
struct ProbeVolume {
    glm::ivec3 Dims{0};
    glm::vec3 Min{0.0f};
    glm::vec3 CellSize{1.0f};
    std::vector<SH1> Probes; // Dims.x*Dims.y*Dims.z, x-major (x + y*W + z*W*H)

    // 3D-текстуры каналов R/G/B (в каждой — vec4 гармоник канала). Лениво.
    std::unique_ptr<sage::rhi::Texture3D> GpuR, GpuG, GpuB;

    bool Valid() const { return Dims.x > 0 && Dims.y > 0 && Dims.z > 0; }
    int Index(int x, int y, int z) const { return x + y * Dims.x + z * Dims.x * Dims.y; }
};

// Полное состояние GI сцены. Живёт в Scene::GI (shared_ptr — переезжает между
// снапшотами сцены редактора без повторного запекания, см. Transplant).
struct GIState {
    GISettings Settings;
    uint64_t GeometryHash = 0; // отпечаток статичной геометрии на момент бейка
    bool Baked = false;        // есть валидные лайтмапы (страницы + UV)

    std::vector<LightmapPage> Pages;
    std::unordered_map<int, EntityBake> Entities; // ключ — id сущности сцены
    ProbeVolume Probes;
};

// --- Вход бейкера (собирается на главном потоке, дальше — чистая математика) --
struct StaticItem {
    int EntityId = 0;
    glm::mat4 World{1.0f};
    glm::vec3 Albedo{1.0f};
    float TexelScale = 1.0f;
    bool Lightmapped = true;          // false — только окклюдер/отражатель
    sage::render::MeshData Mesh;      // локальная геометрия
};

struct BakeInput {
    GISettings Settings;
    LightingEnvironment Env;
    std::vector<StaticItem> Items;
    uint64_t GeometryHash = 0;
};

// Прогресс: доля 0..1 + текущая фаза (для UI редактора). Зовётся из потока бейка.
using ProgressFn = std::function<void(float, const char*)>;

// Собирает вход бейкера из сцены: сущности с GIStaticComponent (+ Transform +
// MeshRenderer). Геометрия примитивов — те же Build*-генераторы, что у рендера;
// модели — .obj через ModelLoader (CPU-стадия). Без GL.
BakeInput CollectBakeInput(Scene& scene, const GISettings& settings);

// Запекает лайтмапы + GI-объём. Чистый CPU (параллелится JobSystem'ом),
// безопасно звать из фонового потока. progress может быть пустым.
std::shared_ptr<GIState> Bake(const BakeInput& input, const ProgressFn& progress = {});

// Восстановление после загрузки сцены: пересчитывает развёртку/UV по текущей
// геометрии и, если отпечаток совпал с сохранённым, читает страницы атласа с
// диска (<сцена без расширения>_gi_page<N>.hdr). При несовпадении лайтмапы
// помечаются устаревшими (Baked=false), объём проб из файла сцены остаётся.
void RebuildAfterLoad(Scene& scene, const std::string& scenePath);

// Сохранение страниц атласа рядом с файлом сцены (зовёт SceneSerializer::Save).
void SavePages(const GIState& state, const std::string& scenePath);

// Переносит запечённый GI со старой сцены на новую (снапшоты undo/Play
// редактора: сериализация в строку не тащит страницы атласа). Переносит только
// если у новой сцены совпадает отпечаток статичной геометрии.
void Transplant(Scene& from, Scene& to);

// Отпечаток статичной геометрии сцены (id/трансформы/меши/настройки) — решает,
// актуальна ли запечённая развёртка для текущего состояния сцены.
uint64_t ComputeGeometryHash(Scene& scene, const GISettings& settings);

// --- Утилиты SH (используются бейкером, рантаймом и тестами) ---------------
// Добавляет вклад направленного сэмпла радианса в L1-гармоники (weight —
// вес квадратуры, для равномерной сферы 4π/N).
void SHAddSample(SH1& sh, const glm::vec3& dir, const glm::vec3& radiance, float weight);

// «Непрямая освещённость / π» из гармоник по нормали (конвенция лайтмап/ambient).
glm::vec3 SHEvalIrradianceOverPi(const SH1& sh, const glm::vec3& n);

} // namespace sage::gi
