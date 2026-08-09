#include "sage/render/MeshSimplify.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>

namespace sage::render {
namespace {

// Ключ ячейки сетки. Пакуем три индекса в 64 бита: хэш-таблица по такому ключу
// в разы дешевле, чем по структуре с оператором сравнения, а 21 бита на ось
// (2 млн ячеек) хватает с запасом — сетка выше двух сотен не поднимается.
inline std::uint64_t CellKey(int x, int y, int z) {
    const std::uint64_t ux = (std::uint64_t)(x & 0x1FFFFF);
    const std::uint64_t uy = (std::uint64_t)(y & 0x1FFFFF);
    const std::uint64_t uz = (std::uint64_t)(z & 0x1FFFFF);
    return (ux << 42) | (uy << 21) | uz;
}

struct Cluster {
    glm::vec3 Position{0.0f};
    glm::vec3 Normal{0.0f};
    glm::vec2 Uv{0.0f};
    glm::vec2 Uv2{0.0f};
    glm::vec4 Tangent{0.0f};
    int Count = 0;
    unsigned int Index = 0; // номер в выходном массиве вершин
};

// Годится ли разметка для переноса на упрощённую геометрию: отрезки кратны
// треугольнику, идут подряд и покрывают буфер без дыр и наложений.
//
// Требование строгое намеренно. Перенос держится на том, что треугольники
// выпускаются в исходном порядке; на разметке с дырами или перехлёстами это
// перестаёт работать, и границы поехали бы — то есть части модели покрасились
// бы чужими материалами. Отдать грубый уровень без разметки честнее.
bool MarkupIsSequential(const std::vector<Submesh>& submeshes, size_t indexCount) {
    if (submeshes.empty()) return false;
    size_t cursor = 0;
    for (const Submesh& s : submeshes) {
        if (s.FirstIndex != cursor) return false;
        if (s.IndexCount % 3 != 0) return false;
        cursor += s.IndexCount;
        if (cursor > indexCount) return false;
    }
    return cursor == indexCount;
}

} // namespace

SimplifyResult SimplifyByClustering(const std::vector<Vertex>& vertices,
                                    const std::vector<unsigned int>& indices, int gridSize,
                                    const std::vector<Submesh>& submeshes) {
    SimplifyResult out;
    out.SourceTriangles = (int)(indices.size() / 3);
    out.Triangles = out.SourceTriangles;

    const bool keepMarkup = MarkupIsSequential(submeshes, indices.size());

    if (vertices.empty() || indices.size() < 3 || gridSize < 1) {
        out.Vertices = vertices;
        out.Indices = indices;
        if (keepMarkup) out.Submeshes = submeshes;   // геометрия не менялась
        return out;
    }

    glm::vec3 lo(vertices[0].Position), hi(vertices[0].Position);
    for (const Vertex& v : vertices) {
        lo = glm::min(lo, v.Position);
        hi = glm::max(hi, v.Position);
    }
    const glm::vec3 extent = hi - lo;
    const float longest = std::max({extent.x, extent.y, extent.z});
    if (longest <= 0.0f) { // вырожденный габарит — упрощать нечего
        out.Vertices = vertices;
        out.Indices = indices;
        return out;
    }
    const float cell = longest / (float)gridSize;

    // 1) Раскладываем вершины по ячейкам и усредняем внутри каждой.
    //
    // Усредняем, а не берём первую попавшуюся: представитель-«первый» дёргает
    // поверхность на величину ячейки, и грубый уровень начинает мерцать при
    // движении камеры. Среднее лежит внутри облака и ведёт себя спокойнее.
    std::unordered_map<std::uint64_t, Cluster> clusters;
    clusters.reserve(vertices.size() / 2 + 1);
    std::vector<std::uint64_t> vertexCell(vertices.size());

    for (size_t i = 0; i < vertices.size(); ++i) {
        const Vertex& v = vertices[i];
        const int cx = (int)std::floor((v.Position.x - lo.x) / cell);
        const int cy = (int)std::floor((v.Position.y - lo.y) / cell);
        const int cz = (int)std::floor((v.Position.z - lo.z) / cell);
        const std::uint64_t key = CellKey(cx, cy, cz);
        vertexCell[i] = key;

        Cluster& c = clusters[key];
        c.Position += v.Position;
        c.Normal += v.Normal;
        c.Uv += v.TexCoords;
        c.Uv2 += v.TexCoords2;
        c.Tangent += v.Tangent;
        ++c.Count;
    }

    // 2) Выпускаем по вершине на ячейку.
    out.Vertices.reserve(clusters.size());
    for (auto& kv : clusters) {
        Cluster& c = kv.second;
        const float inv = 1.0f / (float)std::max(c.Count, 1);
        Vertex v;
        v.Position = c.Position * inv;
        const glm::vec3 n = c.Normal * inv;
        // Нормали внутри ячейки могли смотреть в разные стороны и погасить друг
        // друга (острая кромка, два листа рядом). Нулевую нормаль отдавать
        // нельзя — шейдер получит чёрное пятно; берём направление наружу от
        // центра габарита как заведомо осмысленную замену.
        const float len = glm::length(n);
        v.Normal = len > 1e-5f ? n / len : glm::normalize(v.Position - (lo + hi) * 0.5f + glm::vec3(1e-4f));
        v.TexCoords = c.Uv * inv;
        v.TexCoords2 = c.Uv2 * inv;
        const glm::vec3 t = glm::vec3(c.Tangent) * inv;
        const float tlen = glm::length(t);
        v.Tangent = glm::vec4(tlen > 1e-5f ? t / tlen : glm::vec3(1.0f, 0.0f, 0.0f),
                              c.Tangent.w >= 0.0f ? 1.0f : -1.0f);
        c.Index = (unsigned int)out.Vertices.size();
        out.Vertices.push_back(v);
    }

    // 3) Переписываем треугольники, выбрасывая схлопнувшиеся.
    //
    // Порядок сохраняется, поэтому границы подмешей переносятся простым счётом:
    // сколько треугольников отрезка дожило до выхода, столько и занимает его
    // отрезок там.
    out.Indices.reserve(indices.size());
    size_t nextBoundary = 0;      // конец текущего входного отрезка (в индексах)
    size_t submeshCursor = 0;     // какой отрезок сейчас разбираем
    unsigned int emittedStart = 0;// начало его выходного отрезка
    if (keepMarkup) nextBoundary = submeshes[0].IndexCount;

    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        if (keepMarkup) {
            // Закрываем отрезки, которые закончились на этом треугольнике.
            // Цикл, а не if: пустой подмеш (материал без геометрии) закрывается
            // тем же треугольником, что и предыдущий.
            while (submeshCursor < submeshes.size() && i >= nextBoundary) {
                Submesh s = submeshes[submeshCursor];
                s.FirstIndex = emittedStart;
                s.IndexCount = (unsigned int)out.Indices.size() - emittedStart;
                out.Submeshes.push_back(s);
                emittedStart = (unsigned int)out.Indices.size();
                if (++submeshCursor < submeshes.size())
                    nextBoundary += submeshes[submeshCursor].IndexCount;
            }
        }
        const std::uint64_t k0 = vertexCell[indices[i]];
        const std::uint64_t k1 = vertexCell[indices[i + 1]];
        const std::uint64_t k2 = vertexCell[indices[i + 2]];
        // Два угла в одной ячейке — треугольник выродился в отрезок. Именно на
        // этом и достигается экономия.
        if (k0 == k1 || k1 == k2 || k0 == k2) continue;
        out.Indices.push_back(clusters[k0].Index);
        out.Indices.push_back(clusters[k1].Index);
        out.Indices.push_back(clusters[k2].Index);
    }
    // Хвост: отрезки, закрывшиеся вместе с концом буфера.
    while (keepMarkup && submeshCursor < submeshes.size()) {
        Submesh s = submeshes[submeshCursor];
        s.FirstIndex = emittedStart;
        s.IndexCount = (unsigned int)out.Indices.size() - emittedStart;
        out.Submeshes.push_back(s);
        emittedStart = (unsigned int)out.Indices.size();
        ++submeshCursor;
    }

    // 4) Выкидываем вершины, на которые больше никто не ссылается: после отсева
    //    треугольников их остаётся заметно, и таскать их в вершинном буфере
    //    значило бы экономить треугольники, но не память.
    {
        std::vector<int> remap(out.Vertices.size(), -1);
        std::vector<Vertex> packed;
        packed.reserve(out.Vertices.size());
        for (unsigned int& idx : out.Indices) {
            if (remap[idx] < 0) {
                remap[idx] = (int)packed.size();
                packed.push_back(out.Vertices[idx]);
            }
            idx = (unsigned int)remap[idx];
        }
        out.Vertices.swap(packed);
    }

    out.Triangles = (int)(out.Indices.size() / 3);
    out.Ratio = out.SourceTriangles > 0 ? (float)out.Triangles / (float)out.SourceTriangles : 1.0f;
    return out;
}

SimplifyResult SimplifyToRatio(const std::vector<Vertex>& vertices,
                               const std::vector<unsigned int>& indices, float targetRatio,
                               const std::vector<Submesh>& submeshes) {
    targetRatio = std::clamp(targetRatio, 0.01f, 1.0f);
    SimplifyResult best;
    best.Vertices = vertices;
    best.Indices = indices;
    if (MarkupIsSequential(submeshes, indices.size())) best.Submeshes = submeshes;
    best.SourceTriangles = (int)(indices.size() / 3);
    best.Triangles = best.SourceTriangles;
    if (targetRatio >= 0.999f || indices.size() < 3) return best;

    // Двоичный поиск по размеру сетки. Прямой формулы «сетка → число
    // треугольников» нет: результат зависит от того, как геометрия распределена
    // в пространстве, а не только от её объёма. Восьми проб хватает, чтобы
    // сузить диапазон 2..256 до одной-двух ячеек, и это по-прежнему мгновенно
    // на фоне разбора файла модели.
    int loGrid = 2, hiGrid = 256;
    float bestErr = 1e9f;
    for (int probe = 0; probe < 8 && loGrid <= hiGrid; ++probe) {
        const int mid = (loGrid + hiGrid) / 2;
        SimplifyResult r = SimplifyByClustering(vertices, indices, mid, submeshes);
        const float err = std::fabs(r.Ratio - targetRatio);
        if (err < bestErr && r.Triangles > 0) {
            bestErr = err;
            best = std::move(r);
        }
        if (r.Ratio > targetRatio) hiGrid = mid - 1; // слишком подробно — грубее сетку
        else loGrid = mid + 1;
    }
    return best;
}

} // namespace sage::render
