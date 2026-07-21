#include "sage/gi/LightmapUV.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace sage::gi {

using sage::render::MeshData;

std::vector<Chart> BuildCharts(MeshData& mesh, const glm::mat4& world,
                               float texelsPerUnit, int maxChartSide) {
    std::vector<Chart> charts;
    if (mesh.Empty() || texelsPerUnit <= 0.0f) return charts;

    // Мировые позиции исходных вершин (для биннинга и границ проекции).
    auto worldPos = [&](const Vertex& v) {
        return glm::vec3(world * glm::vec4(v.Position, 1.0f));
    };

    const size_t triCount = mesh.Indices.size() / 3;

    // 1) Бин каждого треугольника — по доминантной оси МИРОВОЙ нормали грани.
    std::vector<int> triBin(triCount, 0);
    for (size_t t = 0; t < triCount; ++t) {
        const glm::vec3 a = worldPos(mesh.Vertices[mesh.Indices[t * 3 + 0]]);
        const glm::vec3 b = worldPos(mesh.Vertices[mesh.Indices[t * 3 + 1]]);
        const glm::vec3 c = worldPos(mesh.Vertices[mesh.Indices[t * 3 + 2]]);
        glm::vec3 n = glm::cross(b - a, c - a);
        if (glm::dot(n, n) < 1e-12f) {
            // Вырожденный треугольник — нормаль из вершин (хоть какая-то ось).
            n = mesh.Vertices[mesh.Indices[t * 3]].Normal;
        }
        triBin[t] = DominantAxis(n);
    }

    // 2) Дублирование вершин: вершина, попавшая в чарты разных осей, получает
    //    копию на каждую ось (у каждой копии — свои UV2). Детерминировано:
    //    порядок обхода треугольников фиксирован.
    std::map<std::pair<unsigned, int>, unsigned> remap; // (исходная вершина, бин) -> новая
    std::vector<Vertex> newVerts;
    newVerts.reserve(mesh.Vertices.size());
    for (size_t t = 0; t < triCount; ++t) {
        for (int k = 0; k < 3; ++k) {
            unsigned orig = mesh.Indices[t * 3 + k];
            auto key = std::make_pair(orig, triBin[t]);
            auto it = remap.find(key);
            unsigned idx;
            if (it == remap.end()) {
                idx = (unsigned)newVerts.size();
                newVerts.push_back(mesh.Vertices[orig]);
                remap.emplace(key, idx);
            } else {
                idx = it->second;
            }
            mesh.Indices[t * 3 + k] = idx;
        }
    }
    mesh.Vertices = std::move(newVerts);

    // 3) Чарты по непустым бинам: границы проекции + размер в текселях.
    const int pad = kChartPadding;
    const int maxInner = std::max(1, maxChartSide - 2 * pad);
    for (int axis = 0; axis < 6; ++axis) {
        Chart chart;
        chart.Axis = axis;
        glm::vec2 mn(std::numeric_limits<float>::max());
        glm::vec2 mx(-std::numeric_limits<float>::max());
        for (size_t t = 0; t < triCount; ++t) {
            if (triBin[t] != axis) continue;
            chart.Tris.push_back((int)t);
            for (int k = 0; k < 3; ++k) {
                glm::vec2 p = ProjectAxis(worldPos(mesh.Vertices[mesh.Indices[t * 3 + k]]), axis);
                mn = glm::min(mn, p);
                mx = glm::max(mx, p);
            }
        }
        if (chart.Tris.empty()) continue;

        chart.WorldMin = mn;
        chart.WorldSize = glm::max(mx - mn, glm::vec2(1e-4f));
        int innerW = std::clamp((int)std::ceil(chart.WorldSize.x * texelsPerUnit), 1, maxInner);
        int innerH = std::clamp((int)std::ceil(chart.WorldSize.y * texelsPerUnit), 1, maxInner);
        chart.W = innerW + 2 * pad;
        chart.H = innerH + 2 * pad;

        // Локальные UV2 (0..1 внутри чарта) — в атлас переведёт
        // ApplyAtlasTransform после упаковки.
        for (int t : chart.Tris) {
            for (int k = 0; k < 3; ++k) {
                Vertex& v = mesh.Vertices[mesh.Indices[t * 3 + k]];
                glm::vec2 p = ProjectAxis(glm::vec3(world * glm::vec4(v.Position, 1.0f)), axis);
                v.TexCoords2 = (p - chart.WorldMin) / chart.WorldSize;
            }
        }
        charts.push_back(std::move(chart));
    }
    return charts;
}

int PackCharts(std::vector<Chart*>& charts, int pageSize) {
    // Полки (shelf packing): чарты по убыванию высоты, каждая полка — ряд
    // фиксированной высоты. Стабильная сортировка -> детерминированный результат.
    std::stable_sort(charts.begin(), charts.end(),
                     [](const Chart* a, const Chart* b) { return a->H > b->H; });

    struct Shelf { int X, Y, H; };
    struct Page { std::vector<Shelf> Shelves; int NextY = 0; };
    std::vector<Page> pages;

    for (Chart* c : charts) {
        // Чарт шире/выше страницы не бывает (BuildCharts ужимает под maxChartSide),
        // но защищаемся: ужмём прямо здесь, UV остаются валидными (0..1 внутри).
        c->W = std::min(c->W, pageSize);
        c->H = std::min(c->H, pageSize);

        bool placed = false;
        for (size_t p = 0; p < pages.size() && !placed; ++p) {
            for (Shelf& s : pages[p].Shelves) {
                if (c->H <= s.H && s.X + c->W <= pageSize) {
                    c->Page = (int)p; c->X = s.X; c->Y = s.Y;
                    s.X += c->W;
                    placed = true;
                    break;
                }
            }
            if (!placed && pages[p].NextY + c->H <= pageSize) {
                pages[p].Shelves.push_back({c->W, pages[p].NextY, c->H});
                c->Page = (int)p; c->X = 0; c->Y = pages[p].NextY;
                pages[p].NextY += c->H;
                placed = true;
            }
        }
        if (!placed) {
            pages.push_back({});
            Page& pg = pages.back();
            pg.Shelves.push_back({c->W, 0, c->H});
            pg.NextY = c->H;
            c->Page = (int)pages.size() - 1; c->X = 0; c->Y = 0;
        }
    }
    return (int)pages.size();
}

void ApplyAtlasTransform(MeshData& mesh, const std::vector<Chart>& charts, int pageSize) {
    const float pad = (float)kChartPadding;
    const float inv = 1.0f / (float)pageSize;
    for (const Chart& c : charts) {
        const glm::vec2 inner((float)(c.W - 2 * kChartPadding), (float)(c.H - 2 * kChartPadding));
        for (int t : c.Tris) {
            for (int k = 0; k < 3; ++k) {
                Vertex& v = mesh.Vertices[mesh.Indices[t * 3 + k]];
                // Вершины чарта уникальны (дублирование в BuildCharts), поэтому
                // повторная запись при общих вершинах треугольников чарта безвредна.
                glm::vec2 local = glm::clamp(v.TexCoords2, glm::vec2(0.0f), glm::vec2(1.0f));
                v.TexCoords2 = (glm::vec2((float)c.X, (float)c.Y) + pad + local * inner) * inv;
            }
        }
    }
}

} // namespace sage::gi
