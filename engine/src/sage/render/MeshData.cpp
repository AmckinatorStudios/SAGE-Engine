#include "sage/render/MeshData.h"

#include <algorithm>
#include <cmath>
#include <glm/gtc/constants.hpp>

// CPU-генераторы примитивов. Перенесены из Mesh.cpp, чтобы геометрию можно
// было получить БЕЗ GL-контекста (бейкер GI, юнит-тесты); Mesh::Create* теперь
// тонкие обёртки над этими функциями.
namespace sage::render {

MeshData BuildCube() {
    MeshData d;
    d.Vertices = {
        // задняя грань (-Z)
        {{0.5f,-0.5f,-0.5f},{0,0,-1},{0,0}}, {{-0.5f,-0.5f,-0.5f},{0,0,-1},{1,0}},
        {{-0.5f, 0.5f,-0.5f},{0,0,-1},{1,1}}, {{0.5f, 0.5f,-0.5f},{0,0,-1},{0,1}},
        // передняя грань (+Z)
        {{-0.5f,-0.5f, 0.5f},{0,0,1},{0,0}}, {{0.5f,-0.5f, 0.5f},{0,0,1},{1,0}},
        {{0.5f, 0.5f, 0.5f},{0,0,1},{1,1}}, {{-0.5f, 0.5f, 0.5f},{0,0,1},{0,1}},
        // левая грань (-X)
        {{-0.5f, 0.5f, 0.5f},{-1,0,0},{1,0}}, {{-0.5f, 0.5f,-0.5f},{-1,0,0},{1,1}},
        {{-0.5f,-0.5f,-0.5f},{-1,0,0},{0,1}}, {{-0.5f,-0.5f, 0.5f},{-1,0,0},{0,0}},
        // правая грань (+X)
        {{0.5f, 0.5f,-0.5f},{1,0,0},{1,0}}, {{0.5f, 0.5f, 0.5f},{1,0,0},{1,1}},
        {{0.5f,-0.5f, 0.5f},{1,0,0},{0,1}}, {{0.5f,-0.5f,-0.5f},{1,0,0},{0,0}},
        // нижняя грань (-Y)
        {{-0.5f,-0.5f,-0.5f},{0,-1,0},{0,1}}, {{0.5f,-0.5f,-0.5f},{0,-1,0},{1,1}},
        {{0.5f,-0.5f, 0.5f},{0,-1,0},{1,0}}, {{-0.5f,-0.5f, 0.5f},{0,-1,0},{0,0}},
        // верхняя грань (+Y)
        {{-0.5f, 0.5f, 0.5f},{0,1,0},{0,1}}, {{0.5f, 0.5f, 0.5f},{0,1,0},{1,1}},
        {{0.5f, 0.5f,-0.5f},{0,1,0},{1,0}}, {{-0.5f, 0.5f,-0.5f},{0,1,0},{0,0}},
    };
    for (unsigned int face = 0; face < 6; ++face) {
        unsigned int o = face * 4;
        d.Indices.insert(d.Indices.end(), { o, o+1, o+2, o+2, o+3, o });
    }
    return d;
}

MeshData BuildSphere(int rings, int sectors) {
    if (rings < 2) rings = 2;
    if (sectors < 3) sectors = 3;
    const float pi = glm::pi<float>();
    const float radius = 0.5f;

    MeshData d;
    for (int r = 0; r <= rings; ++r) {
        float v = (float)r / rings;          // 0..1 сверху вниз
        float phi = v * pi;                  // 0..pi полярный угол
        float y = std::cos(phi);
        float ringR = std::sin(phi);
        for (int s = 0; s <= sectors; ++s) {
            float u = (float)s / sectors;    // 0..1 по окружности
            float theta = u * 2.0f * pi;
            glm::vec3 n(ringR * std::cos(theta), y, ringR * std::sin(theta));
            d.Vertices.push_back({ n * radius, n, {u, 1.0f - v} });
        }
    }

    int stride = sectors + 1;
    for (int r = 0; r < rings; ++r) {
        for (int s = 0; s < sectors; ++s) {
            unsigned int a = r * stride + s;
            unsigned int b = a + stride;
            d.Indices.insert(d.Indices.end(), { a, b, a + 1, a + 1, b, b + 1 });
        }
    }
    return d;
}

MeshData BuildCapsule(int rings, int sectors) {
    if (rings < 2) rings = 2;
    if (sectors < 3) sectors = 3;
    const float pi = glm::pi<float>();
    // РАЗМЕР КАПСУЛЫ ЗАДАЁТ ФИЗИКА, А НЕ ЕДИНИЧНЫЙ КУБ.
    //
    // Здесь стоял радиус 0.25 при высоте 1 — ради того, чтобы форма вписалась в
    // единичный куб, как куб, сфера и цилиндр. Звучит стройно, а на деле
    // означало, что капсула ВДВОЕ УЖЕ всех соседей (у цилиндра и сферы диаметр
    // 1, у капсулы — 0.5) и вдвое уже собственного коллайдера: у
    // ColliderComponent капсула по умолчанию радиус 0.5 и полувысота 0.5, то
    // есть диаметр 1 и высота 2.
    //
    // Расходились не числа, а смысл. Капсулу и завели затем, чтобы ВИДИМОЕ тело
    // совпадало с тем, чем персонаж сталкивается: собранный из каталога
    // персонаж рисовался вдвое тоньше своей физической капсулы, и настраивать
    // рост снова приходилось в двух местах, сверяя на глаз, — ровно то, от чего
    // уходили.
    //
    // Поэтому размер теперь один и тот же с физикой и с любым другим редактором
    // (общепринятый размер): диаметр 1, общая высота 2. В единичный куб такая
    // капсула не вписывается — и не должна: куб описывает куб, а не капсулу.
    const float radius = 0.5f;
    // Половина цилиндрической части. Ровно радиус: полусфера сверху, полусфера
    // снизу, между ними столько же прямой стенки — та же капсула, что у
    // коллайдера по умолчанию.
    const float half = radius;

    MeshData d;
    // Кольца идут сверху вниз одной лентой: верхняя полусфера, шов, нижняя.
    // Шов проходится ДВАЖДЫ (один раз с центром +half, один с -half) — так
    // получается прямая боковая стенка, а не бочка.
    const int total = rings * 2 + 1;
    for (int r = 0; r <= total; ++r) {
        // Полярный угол идёт 0..pi, но центр кольца переезжает на середине.
        const int upper = r <= rings ? r : rings;          // 0..rings
        const int lower = r <= rings ? 0 : r - rings - 1;  // 0..rings-1
        const float phi = (r <= rings) ? (float)upper / rings * (pi * 0.5f)
                                       : (pi * 0.5f) + (float)(lower + 1) / rings * (pi * 0.5f);
        const float cy = (r <= rings) ? half : -half;
        const float ny = std::cos(phi);
        const float ringR = std::sin(phi);
        const float v = (float)r / total;
        for (int s = 0; s <= sectors; ++s) {
            const float u = (float)s / sectors;
            const float theta = u * 2.0f * pi;
            const glm::vec3 n(ringR * std::cos(theta), ny, ringR * std::sin(theta));
            // Нормаль — от ЦЕНТРА СВОЕЙ полусферы, поэтому у боковой стенки она
            // строго горизонтальна, а на шапках расходится веером.
            d.Vertices.push_back({glm::vec3(n.x * radius, cy + n.y * radius, n.z * radius), n,
                                  {u, 1.0f - v}});
        }
    }

    const int stride = sectors + 1;
    for (int r = 0; r < total; ++r) {
        for (int s = 0; s < sectors; ++s) {
            const unsigned int a = (unsigned int)(r * stride + s);
            const unsigned int b = a + (unsigned int)stride;
            d.Indices.insert(d.Indices.end(), {a, b, a + 1, a + 1, b, b + 1});
        }
    }
    return d;
}

MeshData BuildPlane(int subdivisions) {
    if (subdivisions < 1) subdivisions = 1;
    MeshData d;
    for (int z = 0; z <= subdivisions; ++z) {
        for (int x = 0; x <= subdivisions; ++x) {
            float fx = (float)x / subdivisions - 0.5f; // -0.5..0.5
            float fz = (float)z / subdivisions - 0.5f;
            d.Vertices.push_back({ {fx, 0.0f, fz}, {0, 1, 0},
                                   {(float)x / subdivisions, (float)z / subdivisions} });
        }
    }
    int stride = subdivisions + 1;
    for (int z = 0; z < subdivisions; ++z) {
        for (int x = 0; x < subdivisions; ++x) {
            unsigned int a = z * stride + x;
            unsigned int b = a + stride;
            d.Indices.insert(d.Indices.end(), { a, a + 1, b, b, a + 1, b + 1 });
        }
    }
    return d;
}

MeshData BuildCylinder(int sectors) {
    if (sectors < 3) sectors = 3;
    const float pi = glm::pi<float>();
    const float radius = 0.5f;
    const float halfH = 0.5f;

    MeshData d;

    // Боковая поверхность: два кольца (низ/верх), нормали радиальные.
    for (int ring = 0; ring < 2; ++ring) {
        float y = ring == 0 ? -halfH : halfH;
        for (int s = 0; s <= sectors; ++s) {
            float u = (float)s / sectors;
            float theta = u * 2.0f * pi;
            glm::vec3 n(std::cos(theta), 0.0f, std::sin(theta));
            d.Vertices.push_back({ {n.x * radius, y, n.z * radius}, n, {u, (float)ring} });
        }
    }
    int stride = sectors + 1;
    for (int s = 0; s < sectors; ++s) {
        unsigned int a = s;             // низ
        unsigned int b = stride + s;    // верх
        d.Indices.insert(d.Indices.end(), { a, b, a + 1, a + 1, b, b + 1 });
    }

    // Крышки: центр + веер, нормали по ±Y.
    auto addCap = [&](float y, glm::vec3 normal, bool flip) {
        unsigned int center = (unsigned int)d.Vertices.size();
        d.Vertices.push_back({ {0.0f, y, 0.0f}, normal, {0.5f, 0.5f} });
        unsigned int first = (unsigned int)d.Vertices.size();
        for (int s = 0; s <= sectors; ++s) {
            float theta = (float)s / sectors * 2.0f * pi;
            float cx = std::cos(theta), cz = std::sin(theta);
            d.Vertices.push_back({ {cx * radius, y, cz * radius}, normal, {cx * 0.5f + 0.5f, cz * 0.5f + 0.5f} });
        }
        for (int s = 0; s < sectors; ++s) {
            unsigned int a = first + s, b = first + s + 1;
            if (flip) d.Indices.insert(d.Indices.end(), { center, b, a });
            else      d.Indices.insert(d.Indices.end(), { center, a, b });
        }
    };
    addCap(halfH, {0, 1, 0}, false);
    addCap(-halfH, {0, -1, 0}, true);
    return d;
}

MeshData BuildCone(int sectors) {
    if (sectors < 3) sectors = 3;
    const float pi = glm::pi<float>();
    const float radius = 0.5f;
    const float halfH = 0.5f;

    MeshData d;

    // Боковая поверхность: вершина продублирована на каждый сектор, чтобы у
    // конуса были корректные (не усреднённые) боковые нормали. Наклон нормали
    // учитывает высоту — normal = normalize(cos, r/h, sin) в наклонной системе.
    float slope = radius / (2.0f * halfH);
    for (int s = 0; s < sectors; ++s) {
        float t0 = (float)s / sectors * 2.0f * pi;
        float t1 = (float)(s + 1) / sectors * 2.0f * pi;
        float tm = (t0 + t1) * 0.5f;
        glm::vec3 apexN = glm::normalize(glm::vec3(std::cos(tm), slope, std::sin(tm)));
        glm::vec3 n0 = glm::normalize(glm::vec3(std::cos(t0), slope, std::sin(t0)));
        glm::vec3 n1 = glm::normalize(glm::vec3(std::cos(t1), slope, std::sin(t1)));
        unsigned int base = (unsigned int)d.Vertices.size();
        d.Vertices.push_back({ {0.0f, halfH, 0.0f}, apexN, {0.5f, 1.0f} });
        d.Vertices.push_back({ {std::cos(t0) * radius, -halfH, std::sin(t0) * radius}, n0, {(float)s / sectors, 0.0f} });
        d.Vertices.push_back({ {std::cos(t1) * radius, -halfH, std::sin(t1) * radius}, n1, {(float)(s + 1) / sectors, 0.0f} });
        d.Indices.insert(d.Indices.end(), { base, base + 1, base + 2 });
    }

    // Основание (диск, нормаль -Y).
    unsigned int center = (unsigned int)d.Vertices.size();
    d.Vertices.push_back({ {0.0f, -halfH, 0.0f}, {0, -1, 0}, {0.5f, 0.5f} });
    unsigned int first = (unsigned int)d.Vertices.size();
    for (int s = 0; s <= sectors; ++s) {
        float theta = (float)s / sectors * 2.0f * pi;
        float cx = std::cos(theta), cz = std::sin(theta);
        d.Vertices.push_back({ {cx * radius, -halfH, cz * radius}, {0, -1, 0}, {cx * 0.5f + 0.5f, cz * 0.5f + 0.5f} });
    }
    for (int s = 0; s < sectors; ++s) {
        d.Indices.insert(d.Indices.end(), { center, first + s + 1, first + s });
    }
    return d;
}

} // namespace sage::render
