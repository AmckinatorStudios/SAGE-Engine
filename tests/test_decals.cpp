// Наклейки (decals): проекция изображения на существующую геометрию.
//
// Проверяется именно ГЕОМЕТРИЯ — обрезка по коробке проектора, UV, отбор граней
// по углу и подъём над поверхностью. Всё на CPU, без GL: это вычисление, а не
// рисование, и мерить его картинкой значило бы проверять заодно и весь рендер.
#include "TestFramework.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <glm/gtc/matrix_transform.hpp>

#include "sage/ecs/DecalSystem.h"
#include "sage/render/DecalProjection.h"
#include "sage/render/MeshData.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"

using sage::render::DecalProjectOptions;
using sage::render::MeshData;
using sage::render::ProjectDecal;

namespace {

// Пол 4x4 в плоскости XZ на высоте y=0, нормалью вверх (+Y), из двух
// треугольников. Приёмник по умолчанию для большинства проверок.
MeshData Floor(float half = 2.0f) {
    MeshData m;
    auto add = [&](float x, float z, float u, float v) {
        Vertex vert{};
        vert.Position = glm::vec3(x, 0.0f, z);
        vert.Normal = glm::vec3(0.0f, 1.0f, 0.0f);
        vert.TexCoords = glm::vec2(u, v);
        m.Vertices.push_back(vert);
    };
    add(-half, -half, 0, 0); add(-half, half, 0, 1);
    add(half, half, 1, 1);   add(half, -half, 1, 0);
    // Порядок обхода подобран так, чтобы ГЕОМЕТРИЧЕСКАЯ нормаль (cross рёбер)
    // смотрела вверх, в +Y. Именно её, а не Vertex::Normal, читает проекция —
    // сглаженная вершинная нормаль соврала бы про то, куда грань повёрнута на
    // самом деле, и наклейка легла бы на изнанку.
    m.Indices = {0, 1, 2, 0, 2, 3};
    return m;
}

// Матрица «мир -> наклейка» для проектора, который смотрит ВНИЗ: его ось +Z
// (то, откуда он светит) направлена по +Y мира, проекция идёт по -Y.
//
// Именно ПОВОРОТ, а не перестановка осей: перестановка меняет знак
// определителя, вместе с ним разворачивается обход треугольников, и тест
// начинает мерить не то, что думает.
glm::mat4 DownProjector(float cx, float cz, float size) {
    glm::mat4 decalToWorld = glm::translate(glm::mat4(1.0f), glm::vec3(cx, 0.0f, cz));
    decalToWorld = glm::rotate(decalToWorld, glm::radians(-90.0f), glm::vec3(1, 0, 0));
    decalToWorld = glm::scale(decalToWorld, glm::vec3(size));
    return glm::inverse(decalToWorld);
}

// Габариты результата по осям пространства наклейки.
void Bounds(const MeshData& m, glm::vec3& lo, glm::vec3& hi) {
    lo = glm::vec3(1e9f); hi = glm::vec3(-1e9f);
    for (const Vertex& v : m.Vertices) { lo = glm::min(lo, v.Position); hi = glm::max(hi, v.Position); }
}

} // namespace

TEST(Decal_projects_onto_a_floor_and_stays_inside_the_box) {
    const MeshData floor = Floor();
    // Проектор 1x1 в центре пола: пол вчетверо больше, значит наклейка обязана
    // быть ОБРЕЗАНА коробкой, а не покрыть весь пол.
    const MeshData decal = ProjectDecal(floor, DownProjector(0.0f, 0.0f, 1.0f));

    CHECK_TRUE(!decal.Empty());
    CHECK_TRUE(decal.Indices.size() % 3 == 0);

    glm::vec3 lo, hi;
    Bounds(decal, lo, hi);
    // Всё внутри единичной коробки. Допуск — на подъём над поверхностью.
    const float eps = 1e-3f;
    CHECK_TRUE(lo.x >= -0.5f - eps && hi.x <= 0.5f + eps);
    CHECK_TRUE(lo.y >= -0.5f - eps && hi.y <= 0.5f + eps);
    // И при этом коробка заполнена: пол сплошной, значит наклейка достаёт до
    // краёв. Без этого проверка прошла бы и на пустой обрезке в точку.
    CHECK_NEAR(lo.x, -0.5f, 1e-3);
    CHECK_NEAR(hi.x, 0.5f, 1e-3);
    CHECK_NEAR(lo.y, -0.5f, 1e-3);
    CHECK_NEAR(hi.y, 0.5f, 1e-3);
}

TEST(Decal_uv_covers_the_whole_image_exactly_once) {
    const MeshData decal = ProjectDecal(Floor(), DownProjector(0.0f, 0.0f, 1.0f));
    CHECK_TRUE(!decal.Empty());

    float uMin = 1e9f, uMax = -1e9f, vMin = 1e9f, vMax = -1e9f;
    for (const Vertex& v : decal.Vertices) {
        uMin = std::min(uMin, v.TexCoords.x); uMax = std::max(uMax, v.TexCoords.x);
        vMin = std::min(vMin, v.TexCoords.y); vMax = std::max(vMax, v.TexCoords.y);
    }
    // Картинка ложится ровно один раз: 0..1 по обеим осям, без выхода за
    // пределы (иначе повтор текстуры дал бы вторую копию у края).
    CHECK_NEAR(uMin, 0.0f, 1e-3);
    CHECK_NEAR(uMax, 1.0f, 1e-3);
    CHECK_NEAR(vMin, 0.0f, 1e-3);
    CHECK_NEAR(vMax, 1.0f, 1e-3);
}

TEST(Decal_ignores_faces_turned_away_from_the_projector) {
    // Тот же пол, но нормалью ВНИЗ (обход обратный) — проектор смотрит ему в
    // спину. Наклейка на изнанку стены попадать не должна.
    MeshData flipped = Floor();
    for (size_t i = 0; i + 2 < flipped.Indices.size(); i += 3)
        std::swap(flipped.Indices[i + 1], flipped.Indices[i + 2]);
    for (Vertex& v : flipped.Vertices) v.Normal = -v.Normal;

    const MeshData decal = ProjectDecal(flipped, DownProjector(0.0f, 0.0f, 1.0f));
    CHECK_TRUE(decal.Empty());
}

TEST(Decal_angle_limit_rejects_grazing_surfaces) {
    // Пол, наклонённый на 60° к проектору. При пределе 80° он ещё принимает
    // наклейку, при 45° — уже нет: изображение на таком скосе растянулось бы
    // полосами, ради чего предел и существует.
    MeshData tilted = Floor();
    const glm::mat4 rot = glm::rotate(glm::mat4(1.0f), glm::radians(60.0f), glm::vec3(1, 0, 0));
    for (Vertex& v : tilted.Vertices) {
        v.Position = glm::vec3(rot * glm::vec4(v.Position, 1.0f));
        v.Normal = glm::vec3(rot * glm::vec4(v.Normal, 0.0f));
    }

    DecalProjectOptions wide; wide.AngleLimitDeg = 80.0f;
    DecalProjectOptions narrow; narrow.AngleLimitDeg = 45.0f;
    // Коробку берём побольше, чтобы наклонная плоскость точно через неё прошла
    // и разница была именно в отборе по углу, а не в промахе мимо коробки.
    const glm::mat4 toDecal = DownProjector(0.0f, 0.0f, 4.0f);

    CHECK_TRUE(!ProjectDecal(tilted, toDecal, wide).Empty());
    CHECK_TRUE(ProjectDecal(tilted, toDecal, narrow).Empty());
}

TEST(Decal_lifts_off_the_surface_to_avoid_z_fighting) {
    DecalProjectOptions opts;
    opts.Offset = 0.02f;
    const MeshData decal = ProjectDecal(Floor(), DownProjector(0.0f, 0.0f, 1.0f), opts);
    CHECK_TRUE(!decal.Empty());

    // Пол лежит в плоскости z=0 пространства наклейки (проектор смотрит вдоль
    // -Z). Подъём идёт вдоль нормали грани, то есть к проектору: +Z.
    for (const Vertex& v : decal.Vertices) {
        CHECK_NEAR(v.Position.z, opts.Offset, 1e-4);
    }
}

TEST(Decal_wraps_around_a_corner_of_two_walls) {
    // Две перпендикулярные плоскости — пол и стена, — обе под проектором.
    // Наклейка обязана лечь на ОБЕ: ради этого проекция и делается по
    // геометрии, а не квадратом.
    MeshData corner = Floor(1.0f);
    const size_t base = corner.Vertices.size();
    auto addWall = [&](float y0, float y1) {
        auto add = [&](float x, float y) {
            Vertex v{};
            v.Position = glm::vec3(x, y, 1.0f); // стена в плоскости z=+1
            v.Normal = glm::vec3(0.0f, 0.0f, -1.0f);
            corner.Vertices.push_back(v);
        };
        add(-1.0f, y0); add(1.0f, y0); add(1.0f, y1); add(-1.0f, y1);
    };
    addWall(0.0f, 1.0f);
    // Обход — так, чтобы геометрическая нормаль стены смотрела в -Z, то есть
    // внутрь угла, навстречу проектору (см. Floor: читается именно она).
    corner.Indices.push_back((unsigned)base + 0); corner.Indices.push_back((unsigned)base + 2);
    corner.Indices.push_back((unsigned)base + 1);
    corner.Indices.push_back((unsigned)base + 0); corner.Indices.push_back((unsigned)base + 3);
    corner.Indices.push_back((unsigned)base + 2);

    // Проектор над углом, смотрит по диагонали вниз-вперёд: направление
    // проекции (0, -1, +1)/√2, то есть его собственная ось +Z смотрит в
    // (0, +1, -1)/√2. Обе поверхности при этом повёрнуты к нему лицом: пол
    // нормалью (0,1,0) даёт с ней 0.71, стена нормалью (0,0,-1) — тоже 0.71.
    // Поворот на -135° вокруг X переводит (0,0,1) ровно в эту ось.
    glm::mat4 d2w = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.4f, 0.6f));
    d2w = glm::rotate(d2w, glm::radians(-135.0f), glm::vec3(1, 0, 0));
    d2w = glm::scale(d2w, glm::vec3(1.6f));
    const MeshData decal = ProjectDecal(corner, glm::inverse(d2w));

    CHECK_TRUE(!decal.Empty());
    // Обе поверхности представлены: у пола нормаль в пространстве наклейки
    // имеет одну ориентацию, у стены — заметно другую. Достаточно проверить,
    // что среди граней есть ДВЕ РАЗНЫЕ нормали.
    //
    // Под if, а не сразу по Vertices[0]: провалившийся CHECK не прерывает тест,
    // и обращение к первому элементу пустого результата уронило бы весь набор
    // сегфолтом вместо честного «провалено».
    if (!decal.Vertices.empty()) {
        const glm::vec3 first = decal.Vertices[0].Normal;
        bool foundOther = false;
        for (const Vertex& v : decal.Vertices)
            if (glm::dot(v.Normal, first) < 0.9f) { foundOther = true; break; }
        CHECK_TRUE(foundOther);
    }
}

TEST(Decal_misses_geometry_outside_the_box) {
    // Пол сдвинут далеко в сторону — под проектором пусто. Пустой результат, а
    // не мусор из растянутых треугольников.
    MeshData far = Floor(1.0f);
    for (Vertex& v : far.Vertices) v.Position.x += 50.0f;
    CHECK_TRUE(ProjectDecal(far, DownProjector(0.0f, 0.0f, 1.0f)).Empty());
}

// --- Система наклеек поверх сцены -------------------------------------------
//
// Здесь проверяется не математика проекции (она выше), а то, ради чего она
// вообще нужна: наклейка, поставленная в сцене, находит геометрию сама и едет
// вместе с тем, к чему прикреплена.
TEST(DecalSystem_builds_geometry_from_the_scene) {
    Scene scene("decals");

    // Приёмник: куб 4x4x4 в начале координат.
    GameObject wall = scene.CreateObject("Wall");
    MeshRendererComponent& wmr = wall.Renderer();
    wmr.Ref = MeshRef{MeshRef::Type::Cube, ""};
    wall.GetTransform().Scale = glm::vec3(4.0f);

    // Наклейка над верхней гранью куба (грань на y=+2), смотрит вниз.
    GameObject decal = scene.CreateObject("Decal");
    decal.Renderer().Ref = MeshRef{MeshRef::Type::None, ""};
    decal.GetTransform().Position = glm::vec3(0.0f, 2.0f, 0.0f);
    decal.GetTransform().Rotation = glm::vec3(-90.0f, 0.0f, 0.0f); // -Z смотрит вниз
    decal.GetTransform().Scale = glm::vec3(1.0f);
    scene.Registry().emplace<DecalComponent>(decal.Entity());

    // makeMesh = nullptr: GPU-контекста в тестах нет, и он тут не нужен —
    // система обязана считать геометрию без него.
    const sage::ecs::DecalBuildStats stats = sage::ecs::BuildDecals(scene, nullptr);
    CHECK_TRUE(stats.Rebuilt == 1);
    CHECK_TRUE(stats.Triangles > 0);

    const DecalComponent& dc = scene.Registry().get<DecalComponent>(decal.Entity());
    CHECK_TRUE(dc.Triangles > 0);
    // Флаг погашен — второй вызов ничего не пересобирает. Иначе наклейки
    // считались бы каждый кадр, ради чего флаг и заведён.
    CHECK_FALSE(dc.Dirty);
    CHECK_TRUE(sage::ecs::BuildDecals(scene, nullptr).Rebuilt == 0);
}

TEST(DecalSystem_follows_the_object_it_is_attached_to) {
    // Наклейка — дочерняя сущность приёмника. Приёмник уезжает; наклейка
    // обязана остаться НА НЁМ, а не в точке мира. Ради этого обе матрицы и
    // берутся у сцены, с учётом всей цепочки родителей.
    Scene scene("moving");

    GameObject ship = scene.CreateObject("Ship");
    MeshRendererComponent& smr = ship.Renderer();
    smr.Ref = MeshRef{MeshRef::Type::Cube, ""};
    ship.GetTransform().Scale = glm::vec3(4.0f);

    GameObject decal = scene.CreateObject("Decal");
    decal.Renderer().Ref = MeshRef{MeshRef::Type::None, ""};
    // Координаты ЛОКАЛЬНЫЕ: и положение, и размер наследуют масштаб родителя
    // (×4). 0.5 по Y — это ровно верхняя грань куба в мире, 0.25 масштаба —
    // коробка размером 1. Ставить сюда мировые числа значило бы промахнуться
    // ровно во столько же раз.
    decal.GetTransform().Position = glm::vec3(0.0f, 0.5f, 0.0f);
    decal.GetTransform().Rotation = glm::vec3(-90.0f, 0.0f, 0.0f);
    decal.GetTransform().Scale = glm::vec3(0.25f);
    scene.Registry().emplace<DecalComponent>(decal.Entity());
    scene.SetParent(decal.Entity(), ship.Entity());

    // Забираем саму геометрию через makeMesh: сравнивать будем ЕЁ, а не число
    // треугольников. Счётчик — плохая мера: на удалении в километр обрезка
    // разбивает многоугольник чуть иначе (точность float), и равенство
    // счётчиков ломалось бы, хотя наклейка лежит там же.
    MeshData captured;
    auto capture = [&](const MeshData& m) -> std::shared_ptr<Mesh> {
        captured = m;
        return nullptr; // GPU-меша в тестах не бывает, и он здесь не нужен
    };

    CHECK_TRUE(sage::ecs::BuildDecals(scene, capture).Triangles > 0);
    glm::vec3 lo0, hi0;
    Bounds(captured, lo0, hi0);

    // Уводим корабль далеко вместе с наклейкой.
    ship.GetTransform().Position = glm::vec3(1000.0f, 0.0f, -500.0f);
    scene.Registry().get<DecalComponent>(decal.Entity()).Dirty = true;
    CHECK_TRUE(sage::ecs::BuildDecals(scene, capture).Triangles > 0);
    glm::vec3 lo1, hi1;
    Bounds(captured, lo1, hi1);

    // Геометрия наклейки — в её собственных координатах, поэтому она обязана
    // совпасть: наклейка осталась на том же месте корабля.
    CHECK_NEAR(lo0.x, lo1.x, 1e-3); CHECK_NEAR(hi0.x, hi1.x, 1e-3);
    CHECK_NEAR(lo0.y, lo1.y, 1e-3); CHECK_NEAR(hi0.y, hi1.y, 1e-3);
    CHECK_NEAR(lo0.z, lo1.z, 1e-3); CHECK_NEAR(hi0.z, hi1.z, 1e-3);
}

TEST(DecalSystem_reports_zero_when_it_hits_nothing) {
    // Наклейка в пустоте. Не ошибка и не падение — просто ноль треугольников,
    // и это видно снаружи: «не построилась» отличимо от «не видно».
    Scene scene("empty");
    GameObject decal = scene.CreateObject("Decal");
    decal.Renderer().Ref = MeshRef{MeshRef::Type::None, ""};
    decal.GetTransform().Position = glm::vec3(0.0f, 100.0f, 0.0f);
    scene.Registry().emplace<DecalComponent>(decal.Entity());

    const sage::ecs::DecalBuildStats stats = sage::ecs::BuildDecals(scene, nullptr);
    CHECK_TRUE(stats.Rebuilt == 1);
    CHECK_TRUE(stats.Triangles == 0);
    CHECK_TRUE(scene.Registry().get<DecalComponent>(decal.Entity()).Triangles == 0);
}

TEST(DecalSystem_does_not_project_decals_onto_each_other) {
    // Две наклейки в одном месте над одним кубом. Каждая обязана лечь на КУБ,
    // и ни одна — на соседку: иначе геометрия росла бы с каждой пересборкой,
    // а вторая наклейка копировала бы первую.
    Scene scene("pair");
    GameObject box = scene.CreateObject("Box");
    box.Renderer().Ref = MeshRef{MeshRef::Type::Cube, ""};
    box.GetTransform().Scale = glm::vec3(4.0f);

    auto makeDecal = [&](const char* name) {
        GameObject d = scene.CreateObject(name);
        d.Renderer().Ref = MeshRef{MeshRef::Type::None, ""};
        d.GetTransform().Position = glm::vec3(0.0f, 2.0f, 0.0f);
        d.GetTransform().Rotation = glm::vec3(-90.0f, 0.0f, 0.0f);
        scene.Registry().emplace<DecalComponent>(d.Entity());
        return d;
    };
    GameObject a = makeDecal("A");
    GameObject b = makeDecal("B");

    sage::ecs::BuildDecals(scene, nullptr);
    const int ta = scene.Registry().get<DecalComponent>(a.Entity()).Triangles;
    const int tb = scene.Registry().get<DecalComponent>(b.Entity()).Triangles;
    CHECK_TRUE(ta > 0);
    CHECK_TRUE(ta == tb); // обе увидели ровно один и тот же куб
}

TEST(Decal_survives_degenerate_input) {
    // Ни вершин, ни индексов, «треугольник» из одной точки, индекс за границей
    // массива — всё это приходит из реальных моделей, и падать здесь нельзя.
    MeshData empty;
    CHECK_TRUE(ProjectDecal(empty, DownProjector(0, 0, 1)).Empty());

    MeshData degenerate;
    Vertex v{}; v.Position = glm::vec3(0.0f); v.Normal = glm::vec3(0, 1, 0);
    degenerate.Vertices = {v, v, v};
    degenerate.Indices = {0, 1, 2};
    CHECK_TRUE(ProjectDecal(degenerate, DownProjector(0, 0, 1)).Empty());

    MeshData badIndex = Floor();
    badIndex.Indices = {0, 1, 999};
    CHECK_TRUE(ProjectDecal(badIndex, DownProjector(0, 0, 1)).Empty());
}
