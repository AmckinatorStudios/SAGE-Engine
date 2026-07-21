#include "sage/gi/BVH.h"

#include <algorithm>
#include <cmath>

namespace sage::gi {

namespace {

constexpr float kEps = 1e-7f;
constexpr int kLeafSize = 4;    // треугольников в листе
constexpr int kMaxDepth = 32;

// Möller–Trumbore. Возвращает t>0 при пересечении, backface — знак det.
inline bool IntersectTri(const glm::vec3& orig, const glm::vec3& dir, const Tri& tri,
                         float& outT, bool& outBack) {
    glm::vec3 e1 = tri.B - tri.A;
    glm::vec3 e2 = tri.C - tri.A;
    glm::vec3 p = glm::cross(dir, e2);
    float det = glm::dot(e1, p);
    if (std::abs(det) < kEps) return false; // луч параллелен плоскости
    float invDet = 1.0f / det;
    glm::vec3 tv = orig - tri.A;
    float u = glm::dot(tv, p) * invDet;
    if (u < 0.0f || u > 1.0f) return false;
    glm::vec3 q = glm::cross(tv, e1);
    float v = glm::dot(dir, q) * invDet;
    if (v < 0.0f || u + v > 1.0f) return false;
    float t = glm::dot(e2, q) * invDet;
    if (t <= kEps) return false;
    outT = t;
    // Изнанка: луч смотрит В ТУ ЖЕ сторону, что нормаль грани (пришёл сзади).
    outBack = glm::dot(dir, tri.N) > 0.0f;
    return true;
}

// Пересечение луча с AABB (slab-метод); tMax сужает поиск.
inline bool IntersectAABB(const glm::vec3& orig, const glm::vec3& invDir,
                          const glm::vec3& mn, const glm::vec3& mx, float tMax) {
    float t0 = 0.0f, t1 = tMax;
    for (int a = 0; a < 3; ++a) {
        float lo = (mn[a] - orig[a]) * invDir[a];
        float hi = (mx[a] - orig[a]) * invDir[a];
        if (lo > hi) std::swap(lo, hi);
        t0 = std::max(t0, lo);
        t1 = std::min(t1, hi);
        if (t0 > t1) return false;
    }
    return true;
}

} // namespace

void BVH::Build(std::vector<Tri> tris) {
    m_tris = std::move(tris);
    m_nodes.clear();
    m_order.resize(m_tris.size());
    for (size_t i = 0; i < m_order.size(); ++i) m_order[i] = (int)i;
    if (m_tris.empty()) return;

    std::vector<glm::vec3> centroids(m_tris.size());
    for (size_t i = 0; i < m_tris.size(); ++i)
        centroids[i] = (m_tris[i].A + m_tris[i].B + m_tris[i].C) / 3.0f;

    m_nodes.reserve(m_tris.size() * 2);
    BuildNode(0, (int)m_tris.size(), centroids, 0);
}

int BVH::BuildNode(int first, int count, std::vector<glm::vec3>& centroids, int depth) {
    int nodeIndex = (int)m_nodes.size();
    m_nodes.push_back({});

    // AABB узла по вершинам его треугольников.
    glm::vec3 mn(std::numeric_limits<float>::max());
    glm::vec3 mx(-std::numeric_limits<float>::max());
    for (int i = first; i < first + count; ++i) {
        const Tri& t = m_tris[m_order[i]];
        mn = glm::min(mn, glm::min(t.A, glm::min(t.B, t.C)));
        mx = glm::max(mx, glm::max(t.A, glm::max(t.B, t.C)));
    }

    if (count <= kLeafSize || depth >= kMaxDepth) {
        Node& n = m_nodes[nodeIndex];
        n.Min = mn; n.Max = mx; n.First = first; n.Count = count;
        return nodeIndex;
    }

    // Median-split по самой длинной оси AABB центроидов.
    glm::vec3 cmn(std::numeric_limits<float>::max());
    glm::vec3 cmx(-std::numeric_limits<float>::max());
    for (int i = first; i < first + count; ++i) {
        cmn = glm::min(cmn, centroids[m_order[i]]);
        cmx = glm::max(cmx, centroids[m_order[i]]);
    }
    glm::vec3 ext = cmx - cmn;
    int axis = ext.x > ext.y ? (ext.x > ext.z ? 0 : 2) : (ext.y > ext.z ? 1 : 2);

    int mid = first + count / 2;
    std::nth_element(m_order.begin() + first, m_order.begin() + mid,
                     m_order.begin() + first + count,
                     [&](int a, int b) { return centroids[a][axis] < centroids[b][axis]; });

    int left = BuildNode(first, mid - first, centroids, depth + 1);
    int right = BuildNode(mid, first + count - mid, centroids, depth + 1);
    Node& n = m_nodes[nodeIndex];
    n.Min = mn; n.Max = mx; n.Left = left; n.Right = right;
    return nodeIndex;
}

bool BVH::Raycast(const glm::vec3& orig, const glm::vec3& dir, float tMax, RayHit& out) const {
    if (m_nodes.empty()) return false;
    glm::vec3 invDir(1.0f / (std::abs(dir.x) < kEps ? std::copysign(kEps, dir.x) : dir.x),
                     1.0f / (std::abs(dir.y) < kEps ? std::copysign(kEps, dir.y) : dir.y),
                     1.0f / (std::abs(dir.z) < kEps ? std::copysign(kEps, dir.z) : dir.z));

    float best = tMax;
    int bestTri = -1;
    bool bestBack = false;

    int stack[kMaxDepth * 2];
    int sp = 0;
    stack[sp++] = 0;
    while (sp > 0) {
        const Node& n = m_nodes[stack[--sp]];
        if (!IntersectAABB(orig, invDir, n.Min, n.Max, best)) continue;
        if (n.Left < 0) {
            for (int i = n.First; i < n.First + n.Count; ++i) {
                int ti = m_order[i];
                float t; bool back;
                if (IntersectTri(orig, dir, m_tris[ti], t, back) && t < best) {
                    best = t; bestTri = ti; bestBack = back;
                }
            }
        } else {
            stack[sp++] = n.Left;
            stack[sp++] = n.Right;
        }
    }

    if (bestTri < 0) return false;
    const Tri& tri = m_tris[bestTri];
    out.T = best;
    out.Pos = orig + dir * best;
    out.N = tri.N;
    out.Albedo = tri.Albedo;
    out.BackFace = bestBack;
    return true;
}

bool BVH::Occluded(const glm::vec3& orig, const glm::vec3& dir, float tMax) const {
    if (m_nodes.empty()) return false;
    glm::vec3 invDir(1.0f / (std::abs(dir.x) < kEps ? std::copysign(kEps, dir.x) : dir.x),
                     1.0f / (std::abs(dir.y) < kEps ? std::copysign(kEps, dir.y) : dir.y),
                     1.0f / (std::abs(dir.z) < kEps ? std::copysign(kEps, dir.z) : dir.z));

    int stack[kMaxDepth * 2];
    int sp = 0;
    stack[sp++] = 0;
    while (sp > 0) {
        const Node& n = m_nodes[stack[--sp]];
        if (!IntersectAABB(orig, invDir, n.Min, n.Max, tMax)) continue;
        if (n.Left < 0) {
            for (int i = n.First; i < n.First + n.Count; ++i) {
                float t; bool back;
                if (IntersectTri(orig, dir, m_tris[m_order[i]], t, back) && t < tMax)
                    return true; // любое пересечение — тень
            }
        } else {
            stack[sp++] = n.Left;
            stack[sp++] = n.Right;
        }
    }
    return false;
}

} // namespace sage::gi
