#include "sage/physics/PhysicsScene.h"

#include <algorithm>

#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cstring>
#include <sstream>

#include "sage/assets/AssetDatabase.h"
#include "sage/core/Log.h"
#include "sage/render/MeshData.h"
#include "sage/render/ModelLoader.h"
#include "sage/scene/Scene.h"
#include "sage/scene/Components.h"

using namespace sage::physics;

namespace {

// Transform хранит поворот углами Эйлера, физика — кватернионами. Перевод —
// общими функциями движка (Transform.h): порядок осей один на всех.
glm::quat EulerToQuat(const glm::vec3& eulerDeg) { return sage::QuatFromEulerDegrees(eulerDeg); }
glm::vec3 QuatToEuler(const glm::quat& q) { return sage::EulerDegreesFromQuat(q); }

// Мировой Transform сущности: позиция, поворот и масштаб, УЖЕ учитывающие
// родителей.
//
// Физика раньше брала ЛОКАЛЬНЫЙ Transform, и это работало ровно до первой
// сущности с родителем. У корневой локальное и мировое совпадают, поэтому
// ошибка была невидима во всех тестах и во всех сценах, где тела висят на
// корне. А стоило прицепить объект к другому — и тело оказывалось не там, где
// объект: на экране всё правильно (рендер-то мировую матрицу учитывает), а
// столкновения происходят в стороне, будто коллизия «не поворачивается вместе
// с объектом». Найти это по симптому почти невозможно.
struct WorldTransform {
    glm::vec3 Position{0.0f};
    glm::quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 Scale{1.0f};
};

WorldTransform DecomposeWorld(const glm::mat4& m) {
    WorldTransform out;
    out.Position = glm::vec3(m[3]);
    // Масштаб — длины столбцов; поворот — те же столбцы, нормированные. Полный
    // glm::decompose здесь избыточен: скос физике всё равно не передать, у формы
    // столкновения его нет по определению.
    glm::vec3 c0(m[0]), c1(m[1]), c2(m[2]);
    out.Scale = {glm::length(c0), glm::length(c1), glm::length(c2)};
    if (out.Scale.x > 1e-6f) c0 /= out.Scale.x;
    if (out.Scale.y > 1e-6f) c1 /= out.Scale.y;
    if (out.Scale.z > 1e-6f) c2 /= out.Scale.z;
    glm::mat3 rot(c0, c1, c2);
    out.Rotation = glm::quat_cast(rot);
    return out;
}

// Мировая матрица РОДИТЕЛЯ (единичная, если родителя нет). Нужна для обратного
// перевода: физика отвечает в мировых координатах, Transform хранит локальные.
glm::mat4 ParentWorldMatrix(const Scene& scene, entt::entity e) {
    const HierarchyComponent* h = scene.Registry().try_get<HierarchyComponent>(e);
    if (!h || h->Parent == entt::null || !scene.Registry().valid(h->Parent)) return glm::mat4(1.0f);
    return scene.WorldMatrix(h->Parent);
}

// Ведёт ли объект контроллер персонажа (тогда его тело — не хозяин положения).
bool OwnedByCharacter(const entt::registry& reg, entt::entity e) {
    const CharacterControllerComponent* cc = reg.try_get<CharacterControllerComponent>(e);
    return cc && cc->Managed && !cc->Solid;
}

bool IsMeshShape(ShapeType s) { return s == ShapeType::ConvexHull || s == ShapeType::Mesh; }

BodyDesc DescFromEntity(const RigidBodyComponent& rb, const ColliderComponent* col,
                        const WorldTransform& tr, MeshGeometryPtr geometry = nullptr) {
    BodyDesc d;
    d.Type = rb.Type;
    d.Mass = rb.Mass;
    d.Friction = rb.Friction;
    d.Restitution = rb.Restitution;
    d.Layer = rb.Layer;
    d.Sensor = rb.Sensor;
    d.LinearDamping = rb.LinearDamping;
    d.AngularDamping = rb.AngularDamping;
    d.GravityScale = rb.GravityScale;
    d.Continuous = rb.Continuous;
    d.Locks = rb.Locks;
    d.Position = tr.Position;
    d.Rotation = tr.Rotation;

    glm::vec3 scale = glm::abs(tr.Scale);
    float uniform = glm::max(scale.x, glm::max(scale.y, scale.z));
    if (col && !col->Parts.empty()) {
        // Составная форма: каждая дочерняя часть масштабируется как одиночная.
        for (const ColliderComponent::Part& p : col->Parts) {
            ChildShape cs;
            cs.Shape = p.Shape;
            cs.HalfExtents = p.HalfExtents * scale;
            cs.Radius = p.Radius * uniform;
            cs.HalfHeight = p.HalfHeight * scale.y;
            cs.Position = p.Offset * scale;
            cs.Rotation = EulerToQuat(p.EulerDeg);
            d.Children.push_back(cs);
        }
    } else if (col && IsMeshShape(col->Shape)) {
        // Геометрия уже с масштабом (см. ColliderGeometry). Размеры коробки —
        // запасная форма бэкенда, если из геометрии ничего не построится.
        d.Shape = col->Shape;
        d.Geometry = std::move(geometry);
        d.HalfExtents = col->HalfExtents * scale;
    } else if (col && col->Center != glm::vec3(0.0f)) {
        // Смещённый центр — та же форма одной частью со смещением: у
        // бэкендов уже есть путь «часть внутри тела», второй не нужен.
        ChildShape cs;
        cs.Shape = col->Shape;
        cs.HalfExtents = col->HalfExtents * scale;
        cs.Radius = col->Radius * uniform;
        cs.HalfHeight = col->HalfHeight * scale.y;
        cs.Position = col->Center * scale;
        d.Children.push_back(cs);
    } else if (col) {
        d.Shape = col->Shape;
        d.HalfExtents = col->HalfExtents * scale;              // Box масштабируется по осям
        d.Radius = col->Radius * uniform;                     // равномерно
        d.HalfHeight = col->HalfHeight * scale.y;
    } else {
        // Нет коллайдера — единичный бокс по масштабу сущности.
        d.Shape = ShapeType::Box;
        d.HalfExtents = 0.5f * scale;
    }
    return d;
}

// --- Геометрия зон для персонажей ---------------------------------------------
//
// Персонаж — не тело мира, и бэкенд о его входе в зону не сообщает. Поэтому
// касание «капсула персонажа — форма зоны» считается здесь. Расстояние от
// точки до выпуклой формы — выпуклая функция, и вдоль отрезка оси капсулы у
// неё один минимум: троичный поиск находит его точно, без особых случаев
// «отрезок против коробки» на каждую пару форм.

// Расстояние от точки (в ЛОКАЛЬНЫХ осях формы, капсула — вдоль Y) до формы;
// 0 — точка внутри.
float DistToShape(const glm::vec3& p, ShapeType shape, const glm::vec3& half, float radius,
                  float halfHeight) {
    switch (shape) {
        case ShapeType::Sphere: return glm::max(glm::length(p) - radius, 0.0f);
        case ShapeType::Capsule: {
            const glm::vec3 axis(0.0f, glm::clamp(p.y, -halfHeight, halfHeight), 0.0f);
            return glm::max(glm::length(p - axis) - radius, 0.0f);
        }
        case ShapeType::Box:
        default: return glm::length(glm::max(glm::abs(p) - half, glm::vec3(0.0f)));
    }
}

// Ближе всего отрезок [a, b] подходит к форме на этом расстоянии.
float SegmentToShape(const glm::vec3& a, const glm::vec3& b, ShapeType shape, const glm::vec3& half,
                     float radius, float halfHeight) {
    float lo = 0.0f, hi = 1.0f;
    for (int i = 0; i < 40; ++i) {
        const float m1 = lo + (hi - lo) / 3.0f, m2 = hi - (hi - lo) / 3.0f;
        const float d1 = DistToShape(glm::mix(a, b, m1), shape, half, radius, halfHeight);
        const float d2 = DistToShape(glm::mix(a, b, m2), shape, half, radius, halfHeight);
        if (d1 > d2) lo = m1;
        else hi = m2;
    }
    return DistToShape(glm::mix(a, b, 0.5f * (lo + hi)), shape, half, radius, halfHeight);
}

// Касается ли капсула (ось [a, b] в мире, радиус r) тела, описанного desc.
bool CapsuleTouchesBody(const glm::vec3& a, const glm::vec3& b, float r, const BodyDesc& desc) {
    const glm::quat inv = glm::inverse(desc.Rotation);
    const glm::vec3 la = inv * (a - desc.Position), lb = inv * (b - desc.Position);
    // Зона по форме модели — по её габариту: точного расстояния до сетки
    // здесь не посчитать, а зона-триггер с точностью до габарита и так
    // рисуется в редакторе коробкой.
    if (desc.Children.empty() && IsMeshShape(desc.Shape) && desc.Geometry && !desc.Geometry->Empty()) {
        glm::vec3 lo = desc.Geometry->Points.front(), hi = lo;
        for (const glm::vec3& p : desc.Geometry->Points) { lo = glm::min(lo, p); hi = glm::max(hi, p); }
        const glm::vec3 c = (lo + hi) * 0.5f;
        return SegmentToShape(la - c, lb - c, ShapeType::Box, (hi - lo) * 0.5f, 0.0f, 0.0f) <= r;
    }
    if (desc.Children.empty())
        return SegmentToShape(la, lb, desc.Shape, desc.HalfExtents, desc.Radius, desc.HalfHeight) <= r;
    for (const ChildShape& c : desc.Children) {
        const glm::quat ci = glm::inverse(c.Rotation);
        if (SegmentToShape(ci * (la - c.Position), ci * (lb - c.Position), c.Shape, c.HalfExtents,
                           c.Radius, c.HalfHeight) <= r)
            return true;
    }
    return false;
}

// Снимок всего, из чего строится тело: настройки тела, форма и масштаб. Строкой
// байтов, а не структурой с operator==: поле, добавленное в компонент и
// забытое в сравнении, молча перестало бы пересобирать тело, а здесь его
// забыть можно только вместе с записью в дескриптор.
template <class T> void PutRaw(std::string& out, const T& v) {
    out.append(reinterpret_cast<const char*>(&v), sizeof(T));
}
std::string SettingsKey(const RigidBodyComponent& rb, const ColliderComponent* col, const glm::vec3& scale,
                        const MeshRendererComponent* mr) {
    std::string k;
    k.reserve(128);
    PutRaw(k, rb.Type); PutRaw(k, rb.Mass); PutRaw(k, rb.Friction); PutRaw(k, rb.Restitution);
    PutRaw(k, rb.Layer); PutRaw(k, rb.Sensor);
    PutRaw(k, rb.LinearDamping); PutRaw(k, rb.AngularDamping); PutRaw(k, rb.GravityScale);
    PutRaw(k, rb.Continuous); PutRaw(k, rb.Locks);
    PutRaw(k, scale);
    if (!col) return k;
    PutRaw(k, col->Shape); PutRaw(k, col->HalfExtents); PutRaw(k, col->Radius); PutRaw(k, col->HalfHeight);
    PutRaw(k, col->Center);
    for (const ColliderComponent::Part& p : col->Parts) {
        PutRaw(k, p.Shape); PutRaw(k, p.HalfExtents); PutRaw(k, p.Radius); PutRaw(k, p.HalfHeight);
        PutRaw(k, p.Offset); PutRaw(k, p.EulerDeg);
    }
    if (IsMeshShape(col->Shape)) {
        k += col->MeshPath;
        k += '|';
        // Меш самого объекта сменили (другая модель) — форма тоже другая.
        if (col->MeshPath.empty() && mr) {
            PutRaw(k, mr->Ref.type);
            PutRaw(k, mr->Ref.part);
            k += mr->Ref.path;
        }
    }
    return k;
}

// Сварка вершин по положению. Меш для рендера режет вершины по швам нормалей
// и развёртки: у куба их 24 вместо 8. Для сетки столкновений это не мелочь —
// Jolt узнаёт внутреннее ребро (между двумя треугольниками одной поверхности)
// по ОБЩИМ индексам вершин, и несваренная сетка вся состоит из «краёв»: ящик,
// едущий по полу из такой модели, подпрыгивает на каждом стыке треугольников.
std::shared_ptr<MeshGeometry> Weld(const std::vector<Vertex>& verts, const std::vector<unsigned int>& indices) {
    auto g = std::make_shared<MeshGeometry>();
    std::unordered_map<std::string, uint32_t> seen;
    std::vector<uint32_t> remap(verts.size());
    for (size_t i = 0; i < verts.size(); ++i) {
        // Ключ — позиция, округлённая до десятой миллиметра: вершины шва
        // совпадают бит в бит не всегда (экспорт пишет их с разной ошибкой).
        const glm::ivec3 q = glm::ivec3(glm::round(verts[i].Position * 10000.0f));
        std::string key(reinterpret_cast<const char*>(&q), sizeof(q));
        auto [it, fresh] = seen.emplace(std::move(key), (uint32_t)g->Points.size());
        if (fresh) g->Points.push_back(verts[i].Position);
        remap[i] = it->second;
    }
    g->Indices.reserve(indices.size());
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const unsigned a = indices[i], b = indices[i + 1], c = indices[i + 2];
        if (a >= verts.size() || b >= verts.size() || c >= verts.size()) continue;
        // Треугольник, схлопнувшийся при сварке в линию, — не поверхность.
        if (remap[a] == remap[b] || remap[b] == remap[c] || remap[a] == remap[c]) continue;
        // Лицевая сторона — туда, куда смотрят нормали вершин. Jolt сталкивает
        // сетку только с лицевой стороны, а обход треугольников в моделях (и
        // даже в примитиве «плоскость») бывает любым — рендеру он безразличен,
        // освещение идёт по нормалям. Пол с обходом «вниз» пропускал сквозь
        // себя всё, что на него падало.
        const glm::vec3 face = glm::cross(verts[b].Position - verts[a].Position,
                                          verts[c].Position - verts[a].Position);
        const bool flip = glm::dot(face, verts[a].Normal + verts[b].Normal + verts[c].Normal) < 0.0f;
        g->Indices.push_back(remap[a]);
        g->Indices.push_back(remap[flip ? c : b]);
        g->Indices.push_back(remap[flip ? b : c]);
    }
    return g;
}

} // namespace

sage::physics::MeshGeometryPtr PhysicsScene::ColliderGeometry(Scene& scene, entt::entity e, const glm::vec3& scale) {
    const ColliderComponent* col = scene.Registry().try_get<ColliderComponent>(e);
    if (!col) return nullptr;
    const MeshRendererComponent* mr = scene.Registry().try_get<MeshRendererComponent>(e);

    // Откуда геометрия: своя модель коллайдера или меш объекта.
    MeshRef::Type type = MeshRef::Type::Model;
    std::string path = col->MeshPath;
    int part = -1;   // своя модель коллайдера — целиком
    const Mesh* gpu = nullptr;
    if (path.empty()) {
        if (!mr || mr->Ref.type == MeshRef::Type::None) return nullptr;
        type = mr->Ref.type;
        path = mr->Ref.path;
        part = mr->Ref.part;
        gpu = mr->MeshPtr.get();
    }
    // НОМЕР ЧАСТИ — В КЛЮЧЕ. Модель, поставленная иерархией, — это объекты
    // одного файла, каждый со своим узлом (стена, пол, колонна). Ключ без
    // части отдавал ВСЕМ им геометрию того узла, что попал в кэш первым:
    // коллайдер по мешу «работал» у одного объекта, а остальные сталкивались
    // его формой в своих местах — то есть не там, где нарисованы.
    const std::string sourceKey = std::to_string((int)type) + "|" + std::to_string(part) + "|" + path;

    std::shared_ptr<const MeshGeometry>& source = m_meshSources[sourceKey];
    if (!source) {
        std::shared_ptr<MeshGeometry> built;
        switch (type) {
            case MeshRef::Type::Cube: { auto d = sage::render::BuildCube(); built = Weld(d.Vertices, d.Indices); break; }
            case MeshRef::Type::Sphere: { auto d = sage::render::BuildSphere(); built = Weld(d.Vertices, d.Indices); break; }
            case MeshRef::Type::Plane: { auto d = sage::render::BuildPlane(); built = Weld(d.Vertices, d.Indices); break; }
            case MeshRef::Type::Cylinder: { auto d = sage::render::BuildCylinder(); built = Weld(d.Vertices, d.Indices); break; }
            case MeshRef::Type::Cone: { auto d = sage::render::BuildCone(); built = Weld(d.Vertices, d.Indices); break; }
            case MeshRef::Type::Capsule: { auto d = sage::render::BuildCapsule(); built = Weld(d.Vertices, d.Indices); break; }
            case MeshRef::Type::Model:
            default: {
                // Копия в памяти у меша уже есть (редактор держит её для выбора
                // мышью) — берём её, а не читаем файл второй раз.
                if (gpu && gpu->CpuVertices() && gpu->CpuIndices()) {
                    built = Weld(*gpu->CpuVertices(), *gpu->CpuIndices());
                } else if (!path.empty()) {
                    const std::string file = sage::AssetDatabase::Instance().LocatePath(path);
                    try {
                        const sage::render::MeshData d = part >= 0 ? ModelLoader::LoadMeshPartData(file, part)
                                                                   : ModelLoader::LoadMeshData(file);
                        built = Weld(d.Vertices, d.Indices);
                    } catch (const std::exception& ex) {
                        // Узла нет (модель переэкспортировали) — коробка ниже, с сообщением.
                        LOG_WARN("Physics") << "Коллайдер по мешу: часть №" << part << " модели «" << path
                                            << "» не прочитана: " << ex.what();
                    }
                }
                break;
            }
        }
        if (!built || built->Empty()) {
            LOG_WARN("Physics") << "Коллайдер по мешу: геометрия «" << (path.empty() ? "примитив" : path)
                                << "» пуста — тело получит коробку по размерам коллайдера";
            m_meshSources.erase(sourceKey);
            return nullptr;
        }
        source = std::move(built);
    }

    // Масштаб вшивается в точки: растянутый камень должен сталкиваться
    // растянутым. Одинаковый масштаб у разных сущностей — одна геометрия, и
    // бэкенд узнаёт общую форму по общему указателю.
    std::string scaledKey = sourceKey;
    PutRaw(scaledKey, scale);
    MeshGeometryPtr& scaled = m_meshScaled[scaledKey];
    if (!scaled) {
        auto g = std::make_shared<MeshGeometry>(*source);
        for (glm::vec3& p : g->Points) p *= scale;
        // Отрицательный масштаб по нечётному числу осей выворачивает
        // треугольники наизнанку — возвращаем им обход, иначе сетка
        // сталкивается «изнутри».
        if (scale.x * scale.y * scale.z < 0.0f)
            for (size_t i = 0; i + 2 < g->Indices.size(); i += 3) std::swap(g->Indices[i + 1], g->Indices[i + 2]);
        scaled = std::move(g);
    }
    return scaled;
}

namespace {
} // namespace

bool sage::physics::CapsuleTouchesBodyForTest(const glm::vec3& a, const glm::vec3& b, float r,
                                              const BodyDesc& desc) {
    return CapsuleTouchesBody(a, b, r, desc);
}

PhysicsScene::PhysicsScene(Backend backend, Scene& scene) {
    m_world = PhysicsWorld::Create(backend);
    m_world->SetGravity({0.0f, -9.81f, 0.0f});

    // Хэндлы прошлой симуляции сбрасываем: сцена могла уже играться (Play ->
    // Stop -> Play), и в компонентах остались указатели на тела УМЕРШЕГО мира.
    // SyncBodies опознаёт «нет тела» именно по kInvalidBody, так что без этой
    // чистки второй запуск молча остался бы вообще без физики.
    for (auto e : scene.Registry().view<RigidBodyComponent>()) {
        scene.Registry().get<RigidBodyComponent>(e).RuntimeBody = kInvalidBody;
    }
    SyncBodies(scene);

    // Второй проход — соединения (все тела уже созданы, партнёров можно
    // резолвить по id). Хэндлы прошлого прогона сбрасываются так же, как у тел.
    for (auto e : scene.Registry().view<JointComponent>())
        scene.Registry().get<JointComponent>(e).RuntimeJoint = kInvalidJoint;
    SyncJoints(scene);

    LOG_INFO("Physics") << "PhysicsScene: бэкенд " << m_world->BackendName()
                        << ", тел " << m_bodyCount << ", соединений " << m_jointCount
                        << (m_world->IsAvailable() ? "" : " (симуляция отключена)");
}

void PhysicsScene::SyncBodies(Scene& scene) {
    if (!m_world) return;
    auto& reg = scene.Registry();

    // Тело по сущности: мировая поза и масштаб, форма, геометрия модели.
    auto build = [&](entt::entity e, const RigidBodyComponent& rb, const glm::vec3& linear,
                     const glm::vec3& angular) {
        const WorldTransform tr = DecomposeWorld(scene.WorldMatrix(e));
        const ColliderComponent* col = reg.try_get<ColliderComponent>(e);
        MeshGeometryPtr geometry;
        if (col && col->Parts.empty() && IsMeshShape(col->Shape))
            geometry = ColliderGeometry(scene, e, glm::abs(tr.Scale));
        BodyDesc desc = DescFromEntity(rb, col, tr, std::move(geometry));
        desc.LinearVelocity = linear;
        desc.AngularVelocity = angular;
        return m_world->CreateBody(desc);
    };
    // МАСШТАБ — МИРОВОЙ, как и тот, из которого строится форма (build выше).
    // Ключ по локальному не замечал растянутого РОДИТЕЛЯ: пол в группе, которую
    // увеличили вдвое, рисовался большим, а сталкивался прежним.
    auto settingsOf = [&](entt::entity e, const RigidBodyComponent& rb) {
        return SettingsKey(rb, reg.try_get<ColliderComponent>(e), DecomposeWorld(scene.WorldMatrix(e)).Scale,
                           reg.try_get<MeshRendererComponent>(e));
    };
    // Тело сущности: своё (RigidBodyComponent) или неявное статическое.
    auto bodyOf = [&](entt::entity e, bool implicit) -> RigidBodyComponent* {
        if (!reg.valid(e)) return nullptr;
        if (!implicit) return reg.try_get<RigidBodyComponent>(e);
        if (!NeedsImplicitBody(scene, e)) return nullptr;
        auto it = m_implicit.find(e);
        return it == m_implicit.end() ? nullptr : &it->second;
    };

    // 1. Осиротевшие тела (сущность уничтожена или с неё сняли компонент) и
    // тела, чьи настройки поменяли на ходу.
    // Без первого мир копил бы невидимые тела уничтоженных объектов — игра,
    // которая порождает и убирает предметы каждую секунду, за минуту набивала
    // бы физический мир мусором, с которым продолжали бы сталкиваться живые.
    size_t alive = 0;
    for (size_t i = 0; i < m_tracked.size(); ++i) {
        Tracked& t = m_tracked[i];
        RigidBodyComponent* rb = bodyOf(t.Entity, t.Implicit);
        // Контроллер персонажа, добавленный скриптом в Start(), приходит
        // ПОСЛЕ тела — тело снимается тут же (см. OwnedByCharacter ниже).
        const bool character = rb && !t.Implicit && OwnedByCharacter(reg, t.Entity);
        if (character) rb->RuntimeBody = kInvalidBody;
        if (!rb || character || rb->RuntimeBody != t.Body) {
            m_world->RemoveBody(t.Body);
            m_bodyToEntity.erase(t.Body);
            if (t.Implicit) m_implicit.erase(t.Entity);
            --m_bodyCount;
            continue;
        }
        std::string now = settingsOf(t.Entity, *rb);
        if (now != t.Settings) {
            // Пересборка с сохранением движения: ящик, которому в инспекторе
            // поменяли массу в полёте, продолжает лететь, а не замирает.
            const glm::vec3 v = m_world->GetLinearVelocity(t.Body);
            const glm::vec3 w = m_world->GetAngularVelocity(t.Body);
            m_world->RemoveBody(t.Body);
            m_bodyToEntity.erase(t.Body);
            const BodyHandle fresh = build(t.Entity, *rb, v, w);
            rb->RuntimeBody = fresh;
            if (fresh == kInvalidBody) {
                if (t.Implicit) m_implicit.erase(t.Entity);
                --m_bodyCount;
                continue;
            }
            t.Body = fresh;
            t.Settings = std::move(now);
            t.Pose = scene.WorldMatrix(t.Entity);
            m_bodyToEntity[fresh] = t.Entity;
        } else if (rb->Type == BodyType::Static) {
            // НЕПОДВИЖНОЕ ТЕЛО ИДЁТ ЗА ОБЪЕКТОМ, если объект переставили
            // (скрипт, редактор во время Play): иначе стена уезжала на экране,
            // а сталкивались со старым местом. Только при смене позы — тело,
            // которое не трогают, не будит соседей каждый кадр.
            const glm::mat4 pose = scene.WorldMatrix(t.Entity);
            if (pose != t.Pose) {
                const WorldTransform tr = DecomposeWorld(pose);
                m_world->SetBodyTransform(t.Body, tr.Position, tr.Rotation);
                t.Pose = pose;
            }
        }
        if (alive != i) m_tracked[alive] = std::move(t);
        ++alive;
    }
    m_tracked.resize(alive);

    // 2. Новые сущности с RigidBodyComponent, но без тела. Признак «нет тела» —
    // сам RuntimeBody: сущность, пришедшая из сериализатора или порождённая
    // скриптом, несёт kInvalidBody, и другого маркера не требуется.
    auto add = [&](entt::entity e, RigidBodyComponent& rb, bool implicit) {
        rb.RuntimeBody = build(e, rb, glm::vec3(0.0f), glm::vec3(0.0f));
        if (rb.RuntimeBody == kInvalidBody) return false; // Null-бэкенд: тел не бывает
        m_tracked.push_back(Tracked{e, rb.RuntimeBody, settingsOf(e, rb), implicit, scene.WorldMatrix(e)});
        m_bodyToEntity[rb.RuntimeBody] = e;
        ++m_bodyCount;
        return true;
    };
    auto view = reg.view<RigidBodyComponent, Transform>();
    for (auto e : view) {
        RigidBodyComponent& rb = view.get<RigidBodyComponent>(e);
        if (rb.RuntimeBody != kInvalidBody) continue;
        // У ПЕРСОНАЖА ПОЛОЖЕНИЕ ВЕДЁТ КОНТРОЛЛЕР, и тело ему не нужно — у
        // контроллера своя капсула в мире. Динамическое тело на том же объекте
        // было вторым хозяином: падало набок и писало свой поворот в
        // Transform — капсула заваливалась, дочерняя камера получала крен, а
        // контроллер упирался в собственное тело.
        if (OwnedByCharacter(reg, e)) continue;
        add(e, rb, false);
    }

    // 3. КОЛЛАЙДЕР БЕЗ ТВЁРДОГО ТЕЛА — НЕПОДВИЖНАЯ ГЕОМЕТРИЯ, как в Unity.
    // Раньше такой коллайдер в физику не попадал вовсе: стена, пол и камни
    // уровня с одним лишь Collider (обычно Mesh) были невидимы и для ящиков, и
    // для персонажа, а «работал» только тот объект, которому кто-то добавил
    // Rigid Body. Тело заводится здесь же, в компонент ничего не пишется —
    // сцена после Play остаётся какой была.
    for (auto e : reg.view<ColliderComponent, Transform>()) {
        if (m_implicit.count(e) || !NeedsImplicitBody(scene, e)) continue;
        RigidBodyComponent rb;
        rb.Type = BodyType::Static;
        auto [it, fresh] = m_implicit.emplace(e, rb);
        if (!add(e, it->second, true)) m_implicit.erase(it);
    }
}

bool PhysicsScene::NeedsImplicitBody(Scene& scene, entt::entity e) const {
    const auto& reg = scene.Registry();
    if (!reg.all_of<ColliderComponent, Transform>(e) || reg.all_of<RigidBodyComponent>(e)) return false;
    // У персонажа своя форма (капсула контроллера) — его коллайдер не стена.
    if (reg.all_of<CharacterControllerComponent>(e)) return false;
    // Коллайдер под ДВИЖУЩИМСЯ телом — часть этого тела, а не стена мира:
    // неподвижная копия осталась бы висеть, когда ящик уехал.
    for (const HierarchyComponent* h = reg.try_get<HierarchyComponent>(e); h && h->Parent != entt::null;
         h = reg.try_get<HierarchyComponent>(h->Parent)) {
        if (!reg.valid(h->Parent)) break;
        const RigidBodyComponent* prb = reg.try_get<RigidBodyComponent>(h->Parent);
        if (prb && prb->Type != BodyType::Static) return false;
        // Под персонажем — его снаряжение (оружие, модель тела с коллайдером):
        // неподвижная копия стала бы стеной, в которую он упирается сам.
        if (reg.all_of<CharacterControllerComponent>(h->Parent)) return false;
    }
    return true;
}

void PhysicsScene::SyncJoints(Scene& scene) {
    if (!m_world || !m_world->SupportsJoints()) return;
    auto& reg = scene.Registry();

    // Соединение держится за ТЕЛА, а тела пересобираются на ходу (правка в
    // инспекторе, смена слоя из скрипта). Соединение со старым телом бэкенд
    // уже снял вместе с ним — строим заново по новым. И соединения сущностей,
    // порождённых во время игры (sage.scene.SpawnRagdoll), раньше не строились
    // вовсе: кукла рассыпалась на отдельные кости.
    // Партнёр указан, но тела у него нет (объект удалён, компонент снят, тело
    // ещё не построено) — соединения нет. Крепить в этом случае к миру, как
    // было, значило бы, что кость, у которой отстрелили плечо, повисает в
    // воздухе на невидимом шарнире.
    auto partnerMissing = [&](int id) {
        if (id < 0) return false;
        GameObject target = scene.Get(id);
        const RigidBodyComponent* rb = target.Valid() ? reg.try_get<RigidBodyComponent>(target.Entity()) : nullptr;
        return !rb || rb->RuntimeBody == kInvalidBody;
    };
    auto bodyOf = [&](int id) -> BodyHandle {
        if (id < 0) return kInvalidBody;
        GameObject target = scene.Get(id);
        if (!target.Valid()) return kInvalidBody;
        const RigidBodyComponent* rb = reg.try_get<RigidBodyComponent>(target.Entity());
        return rb ? rb->RuntimeBody : kInvalidBody;
    };

    for (auto it = m_jointBodies.begin(); it != m_jointBodies.end();) {
        const entt::entity owner = it->second.Owner;
        const JointComponent* jc = reg.valid(owner) ? reg.try_get<JointComponent>(owner) : nullptr;
        const RigidBodyComponent* rb = reg.valid(owner) ? reg.try_get<RigidBodyComponent>(owner) : nullptr;
        const bool stale = !jc || !rb || jc->RuntimeJoint != it->first || rb->RuntimeBody != it->second.A ||
                           bodyOf(jc->TargetId) != it->second.B;
        if (!stale) { ++it; continue; }
        m_world->RemoveJoint(it->first);
        if (jc && jc->RuntimeJoint == it->first) reg.get<JointComponent>(owner).RuntimeJoint = kInvalidJoint;
        --m_jointCount;
        it = m_jointBodies.erase(it);
    }

    auto joints = reg.view<JointComponent, RigidBodyComponent, Transform>();
    for (auto e : joints) {
        JointComponent& jc = joints.get<JointComponent>(e);
        const RigidBodyComponent& rb = joints.get<RigidBodyComponent>(e);
        if (jc.RuntimeJoint != kInvalidJoint || rb.RuntimeBody == kInvalidBody) continue;
        if (partnerMissing(jc.TargetId)) continue;
        const BodyHandle partner = bodyOf(jc.TargetId);

        JointDesc jd;
        jd.Type = jc.Type;
        jd.BodyA = rb.RuntimeBody;
        jd.BodyB = partner;
        jd.Anchor = glm::vec3(scene.WorldMatrix(e)[3]) + jc.Anchor; // мировая точка крепления
        jd.Axis = jc.Axis;
        jd.UseLimits = jc.UseLimits;
        jd.MinLimit = jc.MinLimit;
        jd.MaxLimit = jc.MaxLimit;
        jd.MinDistance = jc.MinDistance;
        jd.MaxDistance = jc.MaxDistance;
        jd.ConeHalfAngle = jc.ConeHalfAngle;

        jc.RuntimeJoint = m_world->CreateJoint(jd);
        if (jc.RuntimeJoint == kInvalidJoint) continue;
        m_jointBodies[jc.RuntimeJoint] = JointLink{e, jd.BodyA, jd.BodyB};
        ++m_jointCount;
    }
}

void PhysicsScene::TeleportEntity(Scene& scene, entt::entity e) {
    if (!m_world || !scene.Registry().valid(e)) return;
    RigidBodyComponent* rb = scene.Registry().try_get<RigidBodyComponent>(e);
    if (!rb || rb->RuntimeBody == kInvalidBody) return;

    // Мировая поза, а не локальная: физика живёт в мире, а у объекта может быть
    // родитель. Разбор — тот же, что у кинематики ниже: две формулы для одного
    // и того же однажды разойдутся.
    const WorldTransform tr = DecomposeWorld(scene.WorldMatrix(e));
    m_world->SetBodyTransform(rb->RuntimeBody, tr.Position, tr.Rotation);
    // Скорость обнуляется намеренно: тело ПЕРЕСТАВИЛИ, а не бросили. Иначе
    // предмет, поднятый мышью на метр, улетает с той скоростью, которую набрал,
    // пока падал.
    m_world->SetLinearVelocity(rb->RuntimeBody, glm::vec3(0.0f));
}

void PhysicsScene::Step(Scene& scene, float dt) {
    if (!m_world || !m_world->IsAvailable()) return;

    // Состав мира мог измениться с прошлого кадра (скрипт спавнит и удаляет).
    SyncBodies(scene);
    SyncJoints(scene);
    SyncCharacters(scene);

    // Контроллеры персонажей — ДО шага мира: тяготение, прыжок, склон и
    // ступенька считаются здесь, а не в скрипте (см. StepCharacters).
    StepCharacters(scene, dt);

    // Кинематика: до шага толкаем тела за Transform сущности (её ведёт скрипт).
    auto view = scene.Registry().view<RigidBodyComponent, Transform>();
    for (auto e : view) {
        RigidBodyComponent& rb = view.get<RigidBodyComponent>(e);
        if (rb.Type != BodyType::Kinematic || rb.RuntimeBody == kInvalidBody) continue;
        const WorldTransform tr = DecomposeWorld(scene.WorldMatrix(e));
        m_world->SetBodyTransform(rb.RuntimeBody, tr.Position, tr.Rotation);
    }

    m_world->Step(dt);

    // После шага: позиции динамических тел -> обратно в Transform сущностей.
    for (auto e : view) {
        RigidBodyComponent& rb = view.get<RigidBodyComponent>(e);
        if (rb.Type != BodyType::Dynamic || rb.RuntimeBody == kInvalidBody) continue;
        Transform& tr = view.get<Transform>(e);
        glm::vec3 pos;
        glm::quat rot;
        m_world->GetBodyTransform(rb.RuntimeBody, pos, rot);

        // Физика говорит в МИРОВЫХ координатах, а Transform хранит ЛОКАЛЬНЫЕ.
        // У корневой сущности это одно и то же, поэтому раньше разницы никто не
        // замечал; у ребёнка запись мировой позиции в локальное поле означала
        // бы, что объект каждый кадр уезжает на смещение родителя — и чем
        // дальше родитель от начала координат, тем быстрее.
        const glm::mat4 parent = ParentWorldMatrix(scene, e);
        if (parent == glm::mat4(1.0f)) {
            tr.Position = pos;
            tr.Rotation = QuatToEuler(rot);
        } else {
            const glm::mat4 world = glm::translate(glm::mat4(1.0f), pos) * glm::mat4_cast(rot);
            const WorldTransform local = DecomposeWorld(glm::inverse(parent) * world);
            tr.Position = local.Position;
            tr.Rotation = QuatToEuler(local.Rotation);
        }
    }

    PullCharacters(scene, dt);

    // События столкновений: забираем накопленное за шаг и переводим в сущности.
    // Именно здесь, а не по запросу: очередь бэкенда одна, и кто прочитал её
    // первым, тот и забрал события у всех остальных. Список живёт до
    // следующего Step — читать его можно откуда угодно и сколько угодно раз.
    m_contacts.clear();
    if (m_world->SupportsContacts()) {
        m_world->PollContacts(m_rawContacts);
        m_contacts.reserve(m_rawContacts.size());
        for (const ContactEvent& raw : m_rawContacts) {
            auto ia = m_bodyToEntity.find(raw.A);
            auto ib = m_bodyToEntity.find(raw.B);
            // Тело без сущности — не ошибка: сущность могли удалить в этом же
            // кадре, и событие про неё игре уже не адресовать.
            if (ia == m_bodyToEntity.end() || ib == m_bodyToEntity.end()) continue;
            // Зона — не удар: её касания копятся в списке «кто внутри», а
            // события входа и выхода строит UpdateTriggers по его разнице.
            if (raw.Sensor) {
                NoteSensorContact(scene, ia->second, ib->second,
                                  raw.When == ContactEvent::Phase::Begin);
                continue;
            }
            EntityContact c;
            c.Begin = raw.When == ContactEvent::Phase::Begin;
            c.A = ia->second;
            c.B = ib->second;
            c.Point = raw.Point;
            c.Normal = raw.Normal;
            c.Impulse = raw.Impulse;
            c.Sensor = raw.Sensor;
            m_contacts.push_back(c);
        }
    }
    UpdateCharacterContacts(scene);
    UpdateTriggers(scene);
}

PhysicsScene::EntityHit PhysicsScene::Raycast(const glm::vec3& origin, const glm::vec3& direction,
                                              float maxDistance, LayerMask mask) const {
    EntityHit out;
    if (!m_world) return out;
    RayHit hit;
    if (!m_world->Raycast(origin, direction, maxDistance, hit, mask)) return out;
    auto it = m_bodyToEntity.find(hit.Body);
    if (it == m_bodyToEntity.end()) return out;   // тело без сущности игре не адресовать
    out.Hit = true;
    out.Entity = it->second;
    out.Point = hit.Point;
    out.Normal = hit.Normal;
    out.Distance = hit.Distance;
    return out;
}

int PhysicsScene::OverlapSphere(const glm::vec3& center, float radius,
                                std::vector<entt::entity>& out, LayerMask mask) const {
    out.clear();
    if (!m_world) return 0;
    std::vector<BodyHandle> bodies;
    m_world->OverlapSphere(center, radius, bodies, mask);
    for (BodyHandle b : bodies) {
        auto it = m_bodyToEntity.find(b);
        if (it != m_bodyToEntity.end()) out.push_back(it->second);
    }
    return (int)out.size();
}

// --- Триггер-зоны ------------------------------------------------------------

void PhysicsScene::NoteSensorContact(Scene& scene, entt::entity a, entt::entity b, bool begin) {
    auto& reg = scene.Registry();
    auto note = [&](entt::entity zone, entt::entity guest) {
        const RigidBodyComponent* rb = reg.valid(zone) ? reg.try_get<RigidBodyComponent>(zone) : nullptr;
        if (!rb || !rb->Sensor) return;
        const std::pair<entt::entity, entt::entity> key(zone, guest);
        auto it = std::find(m_bodyInside.begin(), m_bodyInside.end(), key);
        if (begin && it == m_bodyInside.end()) m_bodyInside.push_back(key);
        else if (!begin && it != m_bodyInside.end()) m_bodyInside.erase(it);
    };
    // Обе стороны: две зоны, коснувшиеся друг друга, — гость каждая у другой.
    note(a, b);
    note(b, a);
}

void PhysicsScene::UpdateTriggers(Scene& scene) {
    auto& reg = scene.Registry();
    std::vector<TriggerOverlap> prev;
    prev.swap(m_inside);

    auto zoneOf = [&](entt::entity e) -> const RigidBodyComponent* {
        if (!reg.valid(e)) return nullptr;
        const RigidBodyComponent* rb = reg.try_get<RigidBodyComponent>(e);
        return rb && rb->Sensor && rb->RuntimeBody != kInvalidBody ? rb : nullptr;
    };
    // Слой гостя: у тела — свой, у персонажа — свой. Нет ни того ни другого —
    // гостя больше нет в физике, и в зоне ему делать нечего.
    auto layerOf = [&](entt::entity e, LayerMask& out) -> bool {
        if (!reg.valid(e)) return false;
        if (const auto* rb = reg.try_get<RigidBodyComponent>(e)) { out = rb->Layer; return true; }
        if (const auto* cc = reg.try_get<CharacterControllerComponent>(e)) { out = cc->Layer; return true; }
        return false;
    };
    auto admit = [&](entt::entity zone, entt::entity guest) {
        const RigidBodyComponent* rb = zoneOf(zone);
        LayerMask layer = 0;
        if (!rb || zone == guest || !layerOf(guest, layer)) return;
        if ((layer & rb->TriggerMask) == 0) return;
        for (const TriggerOverlap& o : m_inside)
            if (o.Trigger == zone && o.Other == guest) return;
        m_inside.push_back({zone, guest, false});
    };

    // 1. Тело с телом — по сообщениям бэкенда. Пары удалённых и переставших
    // быть зонами выбрасываются здесь: иначе они жили бы вечно.
    m_bodyInside.erase(std::remove_if(m_bodyInside.begin(), m_bodyInside.end(),
                                      [&](const auto& p) {
                                          return !zoneOf(p.first) || !reg.valid(p.second) ||
                                                 !reg.all_of<RigidBodyComponent>(p.second);
                                      }),
                       m_bodyInside.end());
    for (const auto& [zone, guest] : m_bodyInside) admit(zone, guest);

    // 2. Персонажи — геометрией (бэкенд о них не сообщает).
    auto characters = reg.view<CharacterControllerComponent, Transform>();
    if (characters.begin() != characters.end()) {
        for (const Tracked& tracked : m_tracked) {
            const entt::entity zone = tracked.Entity;
            const RigidBodyComponent* rb = zoneOf(zone);
            if (!rb) continue;
            const ColliderComponent* col = reg.try_get<ColliderComponent>(zone);
            const WorldTransform ztr = DecomposeWorld(scene.WorldMatrix(zone));
            MeshGeometryPtr geometry;
            if (col && col->Parts.empty() && IsMeshShape(col->Shape))
                geometry = ColliderGeometry(scene, zone, glm::abs(ztr.Scale));
            const BodyDesc desc = DescFromEntity(*rb, col, ztr, std::move(geometry));
            for (auto e : characters) {
                const CharacterControllerComponent& cc = characters.get<CharacterControllerComponent>(e);
                // Подошвы — там, где их ставит контроллер: точка объекта + Center.
                const glm::vec3 feet = glm::vec3(scene.WorldMatrix(e)[3]) + cc.Center;
                const float r = glm::max(cc.Radius, 0.001f);
                const float top = glm::max(cc.Height - r, r);
                if (CapsuleTouchesBody(feet + glm::vec3(0.0f, r, 0.0f), feet + glm::vec3(0.0f, top, 0.0f),
                                       r, desc))
                    admit(zone, e);
            }
        }
    }

    // 3. Разница с прошлым шагом — события входа и выхода.
    auto had = [](const std::vector<TriggerOverlap>& list, const TriggerOverlap& o) {
        for (const TriggerOverlap& x : list)
            if (x.Trigger == o.Trigger && x.Other == o.Other) return true;
        return false;
    };
    for (TriggerOverlap& o : m_inside) {
        o.Entered = !had(prev, o);
        if (!o.Entered) continue;
        EntityContact c;
        c.Begin = true;
        c.A = o.Trigger;
        c.B = o.Other;
        c.Sensor = true;
        m_contacts.push_back(c);
    }
    // Выход — и для удалённого гостя: скрипт зоны, считающий «сколько внутри»,
    // обязан узнать, что одного не стало, даже если того уже нет в сцене.
    for (const TriggerOverlap& o : prev) {
        if (had(m_inside, o)) continue;
        EntityContact c;
        c.Begin = false;
        c.A = o.Trigger;
        c.B = o.Other;
        c.Sensor = true;
        m_contacts.push_back(c);
    }
}

int PhysicsScene::ObjectsInTrigger(entt::entity trigger, std::vector<entt::entity>& out) const {
    out.clear();
    for (const TriggerOverlap& o : m_inside)
        if (o.Trigger == trigger) out.push_back(o.Other);
    return (int)out.size();
}

int PhysicsScene::TriggersOf(entt::entity other, std::vector<entt::entity>& out) const {
    out.clear();
    for (const TriggerOverlap& o : m_inside)
        if (o.Other == other) out.push_back(o.Trigger);
    return (int)out.size();
}
