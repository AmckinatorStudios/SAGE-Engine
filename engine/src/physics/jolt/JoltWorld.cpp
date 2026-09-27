#include "physics/jolt/JoltWorld.h"

#include <algorithm>

// Jolt требует, чтобы его собственный зонтичный заголовок включался ПЕРВЫМ.
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/CollidePointResult.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <mutex>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/ConeConstraint.h>

#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <thread>

#include "sage/core/Log.h"

using namespace sage::physics;

// ============================================================================
//  Слои столкновений Jolt (минимальная схема: подвижное / неподвижное).
//    NON_MOVING — статика (пол/стены); MOVING — динамика и кинематика.
//  Broadphase-слои повторяют объектные один-к-одному. Неподвижное с
//  неподвижным не сталкивается — всё остальное сталкивается.
// ============================================================================
namespace {

namespace ObjectLayers {
static constexpr JPH::ObjectLayer NON_MOVING = 0;
static constexpr JPH::ObjectLayer MOVING = 1;
static constexpr JPH::ObjectLayer NUM = 2;
} // namespace ObjectLayers

namespace BroadPhaseLayers {
static constexpr JPH::BroadPhaseLayer NON_MOVING(0);
static constexpr JPH::BroadPhaseLayer MOVING(1);
static constexpr JPH::uint NUM = 2;
} // namespace BroadPhaseLayers

// Трассировка Jolt -> наш лог (иначе она уходит в printf).
static void JoltTrace(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    LOG_INFO("Jolt") << buf;
}

// Разовая инициализация глобального состояния Jolt (аллокатор, фабрика типов).
// Живёт на весь процесс — несколько миров переиспользуют её (защита флагом).
static void EnsureJoltGlobals() {
    static bool initialized = false;
    if (initialized) return;
    JPH::RegisterDefaultAllocator();
    JPH::Trace = JoltTrace;
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();
    initialized = true;
}

} // namespace

// ============================================================================
//  Реализации обязательных фильтров слоёв (объявлены в JoltWorld.h).
// ============================================================================
namespace sage::physics {

class JoltBPLayerInterface final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::NUM; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        return layer == ObjectLayers::NON_MOVING ? BroadPhaseLayers::NON_MOVING
                                                 : BroadPhaseLayers::MOVING;
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
        return layer == BroadPhaseLayers::NON_MOVING ? "NON_MOVING" : "MOVING";
    }
#endif
};

class JoltObjectVsBPFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer bpLayer) const override {
        if (layer == ObjectLayers::NON_MOVING) return bpLayer == BroadPhaseLayers::MOVING;
        return true; // MOVING сталкивается со всем
    }
};

class JoltObjectLayerPairFilter final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        if (a == ObjectLayers::NON_MOVING) return b == ObjectLayers::MOVING;
        return true; // MOVING сталкивается со всем
    }
};

} // namespace sage::physics

// ============================================================================
//  JoltWorld
// ============================================================================
namespace {

JPH::RVec3 ToJolt(const glm::vec3& v) { return JPH::RVec3(v.x, v.y, v.z); }
JPH::Quat ToJoltQuat(const glm::quat& q) { return JPH::Quat(q.x, q.y, q.z, q.w); }
// В одинарной точности RVec3 == Vec3 — один перегруз покрывает оба (GetPosition
// возвращает RVec3, GetLinearVelocity — Vec3).
glm::vec3 FromJolt(const JPH::Vec3& v) { return glm::vec3(v.GetX(), v.GetY(), v.GetZ()); }
glm::quat FromJoltQuat(const JPH::Quat& q) { return glm::quat(q.GetW(), q.GetX(), q.GetY(), q.GetZ()); }

// Одна выпуклая форма Jolt по параметрам (box/sphere/capsule) — общий код для
// одиночных и составных тел. Возвращает null при ошибке (вызывающий пропустит
// подшейп/тело). Тонкие боксы безопасны: convex radius адаптируется под
// наименьшую полуось (иначе Jolt делает форму вырожденной и падает при сборке
// compound — реальный баг, найденный на «гантели» со сплюснутой перекладиной).
JPH::ShapeRefC MakeShape(ShapeType shape, const glm::vec3& halfExtents, float radius, float halfHeight) {
    JPH::Shape::ShapeResult res;
    switch (shape) {
        case ShapeType::Sphere:
            res = JPH::SphereShapeSettings(glm::max(radius, 0.01f)).Create();
            break;
        case ShapeType::Capsule:
            res = JPH::CapsuleShapeSettings(glm::max(halfHeight, 0.01f), glm::max(radius, 0.01f)).Create();
            break;
        case ShapeType::Box:
        default: {
            glm::vec3 he = glm::max(halfExtents, glm::vec3(0.01f));
            float minHalf = glm::min(he.x, glm::min(he.y, he.z));
            // Convex radius не больше 0.05 (дефолт Jolt) и не больше 90% самой
            // тонкой полуоси — тонкая перекладина/пластина остаётся валидной.
            float convex = glm::min(0.05f, minHalf * 0.9f);
            JPH::BoxShapeSettings s(JPH::Vec3(he.x, he.y, he.z), convex);
            res = s.Create();
            break;
        }
    }
    if (res.HasError()) {
        LOG_ERROR("Jolt") << "Форма не создана: " << res.GetError().c_str();
        return {};
    }
    return res.Get();
}

// Коробка по габариту точек — запасная форма, когда из геометрии модели
// честную форму построить нельзя (все точки в одной плоскости, пустой меш).
// Тело без формы вообще не появилось бы, и объект молча висел бы в воздухе —
// габарит хотя бы стоит там, где объект нарисован.
JPH::ShapeRefC BoundsBox(const MeshGeometry& g) {
    glm::vec3 lo(FLT_MAX), hi(-FLT_MAX);
    for (const glm::vec3& p : g.Points) { lo = glm::min(lo, p); hi = glm::max(hi, p); }
    if (g.Points.empty()) lo = hi = glm::vec3(0.0f);
    JPH::ShapeRefC box = MakeShape(ShapeType::Box, glm::max((hi - lo) * 0.5f, glm::vec3(0.01f)), 0, 0);
    const glm::vec3 c = (hi + lo) * 0.5f;
    if (!box || glm::length(c) < 1e-6f) return box;
    JPH::Shape::ShapeResult r =
        JPH::RotatedTranslatedShapeSettings(JPH::Vec3(c.x, c.y, c.z), JPH::Quat::sIdentity(), box).Create();
    return r.HasError() ? box : r.Get();
}

// Выпуклая оболочка точек модели. Jolt сам сокращает её до 256 вершин, так
// что отдавать ему можно меш любой плотности.
JPH::ShapeRefC MakeHull(const MeshGeometry& g) {
    JPH::Array<JPH::Vec3> pts;
    pts.reserve(g.Points.size());
    glm::vec3 lo(FLT_MAX), hi(-FLT_MAX);
    for (const glm::vec3& p : g.Points) {
        pts.push_back(JPH::Vec3(p.x, p.y, p.z));
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    if (pts.size() < 4) return {};
    // Скругление не толще пятой части самого тонкого габарита: у плоской
    // плитки дефолтные 5 см съели бы всю толщину.
    const glm::vec3 ext = hi - lo;
    const float thin = glm::min(ext.x, glm::min(ext.y, ext.z));
    JPH::ConvexHullShapeSettings s(pts, glm::clamp(thin * 0.2f, 0.0f, JPH::cDefaultConvexRadius));
    JPH::Shape::ShapeResult r = s.Create();
    if (r.HasError()) {
        LOG_WARN("Jolt") << "Выпуклая оболочка не построена (" << r.GetError().c_str()
                         << ") — беру габаритную коробку";
        return {};
    }
    return r.Get();
}

// Сетка треугольников как есть. Вырожденные треугольники (нулевой площади,
// повторяющиеся) Jolt отбрасывает сам (Sanitize в конструкторе настроек).
JPH::ShapeRefC MakeTriangleMesh(const MeshGeometry& g) {
    if (g.Points.empty() || g.Indices.size() < 3) return {};
    JPH::VertexList verts;
    verts.reserve(g.Points.size());
    for (const glm::vec3& p : g.Points) verts.push_back(JPH::Float3(p.x, p.y, p.z));
    JPH::IndexedTriangleList tris;
    tris.reserve(g.Indices.size() / 3);
    const uint32_t count = (uint32_t)g.Points.size();
    for (size_t i = 0; i + 2 < g.Indices.size(); i += 3) {
        const uint32_t a = g.Indices[i], b = g.Indices[i + 1], c = g.Indices[i + 2];
        // Битый индекс из файла модели — не повод читать чужую память.
        if (a >= count || b >= count || c >= count) continue;
        tris.push_back(JPH::IndexedTriangle(a, b, c, 0));
    }
    if (tris.empty()) return {};
    JPH::MeshShapeSettings s(std::move(verts), std::move(tris));
    JPH::Shape::ShapeResult r = s.Create();
    if (r.HasError()) {
        LOG_WARN("Jolt") << "Сетка коллайдера не построена (" << r.GetError().c_str()
                         << ") — беру габаритную коробку";
        return {};
    }
    return r.Get();
}

// Перпендикуляр к оси (для рамок Hinge/Slider, которым нужна нормаль ⟂ оси).
JPH::Vec3 PerpendicularTo(const JPH::Vec3& axis) {
    JPH::Vec3 a = axis.NormalizedOr(JPH::Vec3::sAxisY());
    JPH::Vec3 ref = std::abs(a.GetX()) < 0.9f ? JPH::Vec3::sAxisX() : JPH::Vec3::sAxisY();
    return a.Cross(ref).NormalizedOr(JPH::Vec3::sAxisZ());
}

} // namespace

// ---------------------------------------------------------------------------
//  Слушатель контактов
// ---------------------------------------------------------------------------
//
// Jolt зовёт эти методы ИЗ РАБОЧИХ ПОТОКОВ посреди шага симуляции. Делать здесь
// что-либо, кроме записи в защищённый буфер, нельзя: сцена в этот момент
// принадлежит физике, а скрипты вообще однопоточные. Поэтому события только
// копятся, а разбирает их главный поток в PollContacts.
class JoltWorld::ContactCollector : public JPH::ContactListener {
public:
    explicit ContactCollector(JoltWorld* owner) : m_owner(owner) {}

    void OnContactAdded(const JPH::Body& a, const JPH::Body& b,
                        const JPH::ContactManifold& manifold,
                        JPH::ContactSettings& settings) override {
        (void)settings;
        Push(a, b, manifold, ContactEvent::Phase::Begin);
    }

    void OnContactRemoved(const JPH::SubShapeIDPair& pair) override {
        ContactEvent e;
        e.When = ContactEvent::Phase::End;
        e.A = m_owner->HandleOf(pair.GetBody1ID().GetIndexAndSequenceNumber());
        e.B = m_owner->HandleOf(pair.GetBody2ID().GetIndexAndSequenceNumber());
        if (e.A == kInvalidBody || e.B == kInvalidBody) return;  // тело уже удалили
        std::lock_guard<std::mutex> lock(m_mutex);
        m_events.push_back(e);
    }

    void Take(std::vector<ContactEvent>& out) {
        std::lock_guard<std::mutex> lock(m_mutex);
        out.swap(m_events);
        m_events.clear();
    }

private:
    void Push(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& manifold,
              ContactEvent::Phase phase) {
        ContactEvent e;
        e.When = phase;
        e.A = m_owner->HandleOf(a.GetID().GetIndexAndSequenceNumber());
        e.B = m_owner->HandleOf(b.GetID().GetIndexAndSequenceNumber());
        if (e.A == kInvalidBody || e.B == kInvalidBody) return;
        const JPH::Vec3 p = manifold.GetWorldSpaceContactPointOn1(0);
        e.Point = glm::vec3(p.GetX(), p.GetY(), p.GetZ());
        const JPH::Vec3 n = manifold.mWorldSpaceNormal;
        e.Normal = glm::vec3(n.GetX(), n.GetY(), n.GetZ());
        e.Sensor = a.IsSensor() || b.IsSensor();
        // Сила удара — по скорости сближения и массам. Точное значение импульса
        // Jolt отдаёт только в OnContactPersisted следующего шага, а событие
        // «врезался» нужно в тот же кадр: игра по нему играет звук и трясёт
        // камеру, и опоздание на кадр слышно.
        if (!e.Sensor) {
            const JPH::Vec3 va = a.GetLinearVelocity();
            const JPH::Vec3 vb = b.GetLinearVelocity();
            const JPH::Vec3 rel = va - vb;
            const float closing = std::abs(rel.Dot(manifold.mWorldSpaceNormal));
            const float ma = a.GetMotionType() == JPH::EMotionType::Dynamic
                                 ? 1.0f / std::max(a.GetMotionProperties()->GetInverseMass(), 1e-6f)
                                 : 0.0f;
            const float mb = b.GetMotionType() == JPH::EMotionType::Dynamic
                                 ? 1.0f / std::max(b.GetMotionProperties()->GetInverseMass(), 1e-6f)
                                 : 0.0f;
            const float m = (ma > 0.0f && mb > 0.0f) ? (ma * mb) / (ma + mb) : std::max(ma, mb);
            e.Impulse = closing * m;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        // Предохранитель: за шаг может случиться сколько угодно контактов, а
        // очередь читает главный поток. Расти без предела ей нельзя — иначе
        // куча песка на полу съедает память быстрее, чем игра успевает читать.
        if (m_events.size() < 8192) m_events.push_back(e);
    }

    JoltWorld* m_owner;
    std::vector<ContactEvent> m_events;
    std::mutex m_mutex;
};

// Виртуальный контроллер персонажа Jolt плюс наши настройки.
struct JoltWorld::CharacterEntry {
    JPH::Ref<JPH::CharacterVirtual> Ptr;
    LayerMask CollidesWith = kAllLayers;
    // Высота ступеньки хранится ЗДЕСЬ, потому что Jolt получает её не при
    // создании контроллера, а на каждый шаг (ExtendedUpdateSettings). Раньше в
    // этом месте стояла константа 0.35, и поле CharacterDesc::StepHeight не
    // читал никто: игра выставляла ступеньку, движок её молча игнорировал.
    float StepHeight = 0.35f;
};

struct JoltWorld::CachedShape {
    MeshGeometryPtr Geometry;   // держит адрес-ключ живым
    JPH::ShapeRefC Hull, Mesh;  // строятся лениво: у одной геометрии бывают обе
};

BodyHandle JoltWorld::HandleOf(uint32_t joltId) const {
    auto it = m_byJoltId.find(joltId);
    return it == m_byJoltId.end() ? kInvalidBody : it->second;
}

LayerMask JoltWorld::LayerOf(BodyHandle body) const {
    auto it = m_layers.find(body);
    return it == m_layers.end() ? kLayerDefault : it->second;
}

// Фильтр тел по НАШЕЙ маске слоёв. Отдельный фильтр, а не проверка результата
// постфактум: узкая фаза, отбросив тело сразу, не считает по нему пересечение,
// а «ближайшее» в её ответе уже учитывает отбор — иначе пришлось бы повторять
// запрос с обрезанной дальностью, пока не найдётся подходящее.
namespace {
class MaskBodyFilter : public JPH::BodyFilter {
public:
    MaskBodyFilter(const JoltWorld* world, LayerMask mask) : m_world(world), m_mask(mask) {}
    bool ShouldCollideLocked(const JPH::Body& body) const override {
        const BodyHandle h = m_world->HandleOf(body.GetID().GetIndexAndSequenceNumber());
        return h != kInvalidBody && (m_world->LayerOf(h) & m_mask) != 0;
    }

private:
    const JoltWorld* m_world;
    LayerMask m_mask;
};
} // namespace

bool JoltWorld::Raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
                        RayHit& out, LayerMask mask) const {
    out = RayHit{};
    if (!m_system) return false;
    const float len = glm::length(direction);
    if (len < 1e-6f || maxDistance <= 0.0f) return false;
    const glm::vec3 dir = direction / len;

    const JPH::RRayCast ray{
        JPH::RVec3(origin.x, origin.y, origin.z),
        JPH::Vec3(dir.x * maxDistance, dir.y * maxDistance, dir.z * maxDistance)};

    JPH::RayCastResult hit;
    const MaskBodyFilter filter(this, mask);
    if (!m_system->GetNarrowPhaseQuery().CastRay(ray, hit, {}, {}, filter)) return false;

    const BodyHandle handle = HandleOf(hit.mBodyID.GetIndexAndSequenceNumber());
    if (handle == kInvalidBody) return false;

    const JPH::RVec3 point = ray.GetPointOnRay(hit.mFraction);
    out.Hit = true;
    out.Body = handle;
    out.Point = glm::vec3(point.GetX(), point.GetY(), point.GetZ());
    out.Distance = hit.mFraction * maxDistance;
    // Нормаль спрашиваем у тела под блокировкой: узкая фаза отдаёт только
    // дескриптор подформы, а поверхность знает само тело в своей позе.
    JPH::BodyLockRead lock(m_system->GetBodyLockInterface(), hit.mBodyID);
    if (lock.Succeeded()) {
        const JPH::Vec3 n = lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, point);
        out.Normal = glm::vec3(n.GetX(), n.GetY(), n.GetZ());
    }
    return true;
}

int JoltWorld::OverlapSphere(const glm::vec3& center, float radius, std::vector<BodyHandle>& out,
                             LayerMask mask) const {
    out.clear();
    if (!m_system || radius <= 0.0f) return 0;
    // Узкая фаза, а не габариты: раньше отвечала широкая фаза, то есть «кто
    // пересекает коробку вокруг тела». Взрыв рядом с длинной диагональной
    // балкой или с сеткой ландшафта задевал всё, что попадало в их огромный
    // габарит, — даже за десять метров от точки.
    JPH::SphereShape sphere(radius);
    sphere.SetEmbedded();   // форма на стеке: счётчик ссылок не должен её удалять
    JPH::CollideShapeSettings settings;
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> hits;
    const MaskBodyFilter filter(this, mask);
    m_system->GetNarrowPhaseQuery().CollideShape(
        &sphere, JPH::Vec3::sOne(), JPH::RMat44::sTranslation(ToJolt(center)), settings,
        ToJolt(center), hits, {}, {}, filter);
    for (const JPH::CollideShapeResult& r : hits.mHits) {
        const BodyHandle handle = HandleOf(r.mBodyID2.GetIndexAndSequenceNumber());
        if (handle == kInvalidBody) continue;
        if (std::find(out.begin(), out.end(), handle) == out.end()) out.push_back(handle);
    }
    return (int)out.size();
}

void JoltWorld::PollContacts(std::vector<ContactEvent>& out) {
    out.clear();
    if (m_contacts) m_contacts->Take(out);
}

CharacterHandle JoltWorld::CreateCharacter(const CharacterDesc& desc) {
    if (!m_system) return kInvalidCharacter;
    // Капсула стоит на земле нижней точкой, а начало координат Jolt держит в
    // центре: поэтому форма поднимается на половину высоты. Иначе персонаж
    // рождается по пояс в полу и первым же шагом выталкивается вверх.
    const float halfCyl = std::max(0.05f, desc.Height * 0.5f - desc.Radius);
    JPH::Ref<JPH::Shape> shape =
        JPH::RotatedTranslatedShapeSettings(JPH::Vec3(0, halfCyl + desc.Radius, 0),
                                            JPH::Quat::sIdentity(),
                                            new JPH::CapsuleShape(halfCyl, desc.Radius))
            .Create()
            .Get();

    JPH::Ref<JPH::CharacterVirtualSettings> settings = new JPH::CharacterVirtualSettings();
    settings->mShape = shape;
    settings->mMaxSlopeAngle = glm::radians(desc.MaxSlopeDeg);
    settings->mMass = desc.Mass;
    // Плоскость опоры чуть ниже подошвы: без неё контроллер «не видит» пол,
    // когда стоит на нём вплотную, и считает себя в воздухе через кадр.
    settings->mSupportingVolume = JPH::Plane(JPH::Vec3(0, 1, 0), -desc.Radius);

    auto entry = std::make_unique<CharacterEntry>();
    entry->CollidesWith = desc.CollidesWith;
    // Не выше девяти десятых роста: ступенька в собственный рост — это уже не
    // шаг, а перепрыгивание препятствия целиком, и лечится оно прыжком.
    entry->StepHeight = std::clamp(desc.StepHeight, 0.0f, desc.Height * 0.9f);
    entry->Ptr = new JPH::CharacterVirtual(
        settings, JPH::RVec3(desc.Position.x, desc.Position.y, desc.Position.z),
        JPH::Quat::sIdentity(), m_system.get());

    const CharacterHandle handle = m_nextCharacter++;
    m_characters[handle] = std::move(entry);
    return handle;
}

void JoltWorld::RemoveCharacter(CharacterHandle character) { m_characters.erase(character); }

void JoltWorld::MoveCharacter(CharacterHandle character, const glm::vec3& velocity, float dt) {
    auto it = m_characters.find(character);
    if (it == m_characters.end() || !m_system || dt <= 0.0f) return;
    JPH::CharacterVirtual* ch = it->second->Ptr;
    ch->SetLinearVelocity(JPH::Vec3(velocity.x, velocity.y, velocity.z));

    JPH::CharacterVirtual::ExtendedUpdateSettings upd;
    // Шаг вверх/вниз — то, чем контроллер отличается от капсулы на пружине:
    // на ступеньку он ВЗБИРАЕТСЯ, а не упирается и не подпрыгивает. Высоту
    // задаёт игра (CharacterDesc::StepHeight); здесь стояла константа, и
    // настройка не работала вовсе.
    upd.mWalkStairsStepUp = JPH::Vec3(0, it->second->StepHeight, 0);
    ch->ExtendedUpdate(dt, m_system->GetGravity(), upd,
                       m_system->GetDefaultBroadPhaseLayerFilter(ObjectLayers::MOVING),
                       m_system->GetDefaultLayerFilter(ObjectLayers::MOVING), {}, {}, *m_tempAllocator);
}

CharacterState JoltWorld::GetCharacterState(CharacterHandle character) const {
    CharacterState st;
    auto it = m_characters.find(character);
    if (it == m_characters.end()) return st;
    const JPH::CharacterVirtual* ch = it->second->Ptr;
    const JPH::RVec3 p = ch->GetPosition();
    const JPH::Vec3 v = ch->GetLinearVelocity();
    const JPH::Vec3 n = ch->GetGroundNormal();
    st.Position = glm::vec3(p.GetX(), p.GetY(), p.GetZ());
    st.Velocity = glm::vec3(v.GetX(), v.GetY(), v.GetZ());
    st.GroundNormal = glm::vec3(n.GetX(), n.GetY(), n.GetZ());
    st.Grounded = ch->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
    st.OnSteepSlope = ch->GetGroundState() == JPH::CharacterBase::EGroundState::OnSteepGround;
    return st;
}

void JoltWorld::SetCharacterPosition(CharacterHandle character, const glm::vec3& position) {
    auto it = m_characters.find(character);
    if (it == m_characters.end()) return;
    it->second->Ptr->SetPosition(JPH::RVec3(position.x, position.y, position.z));
}

JoltWorld::JoltWorld() {
    EnsureJoltGlobals();

    m_tempAllocator = std::make_unique<JPH::TempAllocatorImpl>(16 * 1024 * 1024);
    unsigned threads = std::thread::hardware_concurrency();
    m_jobSystem = std::make_unique<JPH::JobSystemThreadPool>(
        JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
        (int)(threads > 1 ? threads - 1 : 1));

    m_bpLayers = std::make_unique<JoltBPLayerInterface>();
    m_objectVsBpFilter = std::make_unique<JoltObjectVsBPFilter>();
    m_objectLayerFilter = std::make_unique<JoltObjectLayerPairFilter>();

    m_system = std::make_unique<JPH::PhysicsSystem>();
    const JPH::uint kMaxBodies = 4096;
    const JPH::uint kNumBodyMutexes = 0; // авто
    const JPH::uint kMaxBodyPairs = 4096;
    const JPH::uint kMaxContacts = 4096;
    m_system->Init(kMaxBodies, kNumBodyMutexes, kMaxBodyPairs, kMaxContacts,
                   *m_bpLayers, *m_objectVsBpFilter, *m_objectLayerFilter);

    // Настройки контакта — от них зависит, видно ли проникновение глазом.
    JPH::PhysicsSettings ps = m_system->GetPhysicsSettings();
    // Порог включения CCD — доля внутреннего радиуса формы, пройденная за шаг.
    // По умолчанию 0.75: ящик в метр начинал проверять путь только с 22 м/с, а
    // всё медленнее падало дискретно и тонуло в полу на путь за шаг минус
    // 2 см зазора — 6–8 см на обычных 10 м/с. Дискретный шаг безопасен, пока
    // путь за шаг не длиннее зазора спекулятивного контакта (2 см) плюс
    // допуска (0.5 см); 0.01 держит это для тел до пяти метров в поперечнике.
    // CCD-тело Jolt останавливает, заходя в поверхность не глубже того же
    // допуска, — поэтому и допуск ниже уменьшен.
    ps.mLinearCastThreshold = 0.01f;
    // Допустимое проникновение, которое решатель не выталкивает. 2 см по
    // умолчанию видны на мелких предметах: кубик в 10 см стоял бы в полу на
    // пятую часть своей высоты. Полсантиметра не видно, и дрожи стопки ещё
    // нет — меньше этого выталкивание начинает спорить со сном тел.
    ps.mPenetrationSlop = 0.005f;
    // Выталкивание делится между итерациями положения: три вместо двух — и
    // тяжёлый ящик на лёгком не проседает, а стопка стоит ровно.
    ps.mNumPositionSteps = 3;
    m_system->SetPhysicsSettings(ps);

    // Слушатель контактов ставится сразу: события копятся с первого же шага, и
    // «включить их потом» значило бы потерять всё, что случилось до включения.
    m_contacts = std::make_unique<ContactCollector>(this);
    m_system->SetContactListener(m_contacts.get());
}

JoltWorld::~JoltWorld() {
    // Отпускаем удержанные ссылки на соединения (AddRef при создании). Сами
    // Constraint'ы уедут вместе с PhysicsSystem; наш Release балансирует ref-счёт.
    for (auto& kv : m_joints)
        if (kv.second) kv.second->Release();
    m_joints.clear();
    // Тела удаляются вместе с PhysicsSystem. Глобальную фабрику Jolt намеренно
    // НЕ рушим — она разделяется всеми мирами на время жизни процесса.
    // Персонажи держат ссылку на систему — снимаем их ДО неё.
    m_characters.clear();
    if (m_system) m_system->SetContactListener(nullptr);
    m_system.reset();
    m_contacts.reset();
    m_jobSystem.reset();
    m_tempAllocator.reset();
}


void JoltWorld::SetGravity(const glm::vec3& gravity) {
    if (m_system) m_system->SetGravity(JPH::Vec3(gravity.x, gravity.y, gravity.z));
}

JPH::ShapeRefC JoltWorld::ShapeFromGeometry(const BodyDesc& desc, bool dynamic) {
    if (!desc.Geometry || desc.Geometry->Empty()) {
        LOG_WARN("Jolt") << "Коллайдер из меша без геометрии — беру коробку по размерам";
        return MakeShape(ShapeType::Box, desc.HalfExtents, 0, 0);
    }
    std::unique_ptr<CachedShape>& slot = m_shapeCache[desc.Geometry.get()];
    if (!slot) {
        slot = std::make_unique<CachedShape>();
        slot->Geometry = desc.Geometry;
    }
    // Треугольная сетка без объёма: массы и инерции у неё нет, а Jolt не
    // сталкивает сетку с сеткой. Падающее тело с «Mesh» поэтому строится
    // оболочкой — ящик с вогнутой моделью ведёт себя как её выпуклый чехол,
    // но падает, крутится и лежит, а не проходит сквозь другие сетки.
    const bool wantMesh = desc.Shape == ShapeType::Mesh && !dynamic;
    if (wantMesh) {
        if (!slot->Mesh) slot->Mesh = MakeTriangleMesh(*desc.Geometry);
        if (slot->Mesh) return slot->Mesh;
    } else {
        if (desc.Shape == ShapeType::Mesh)
            LOG_INFO("Jolt") << "Dynamic-тело с коллайдером Mesh строится выпуклой оболочкой: "
                                "у сетки треугольников нет объёма и массы";
        if (!slot->Hull) slot->Hull = MakeHull(*desc.Geometry);
        if (slot->Hull) return slot->Hull;
    }
    return BoundsBox(*desc.Geometry);
}

BodyHandle JoltWorld::CreateBody(const BodyDesc& desc) {
    if (!m_system) return kInvalidBody;

    // Все шесть осей заморожены — тело не может двигаться вовсе, а Jolt такого
    // динамического тела не принимает (EAllowedDOFs::None). Честнее всего это
    // кинематика, стоящая на месте: толкает других и не падает.
    const uint8_t locks = desc.Locks & (kLockPosAll | kLockRotAll);
    BodyType type = desc.Type;
    if (type == BodyType::Dynamic && locks == (kLockPosAll | kLockRotAll)) type = BodyType::Kinematic;
    const bool dynamic = type == BodyType::Dynamic;

    // Форма коллайдера: одиночная или СОСТАВНАЯ (StaticCompoundShape из
    // дочерних форм с локальными смещениями — «молоток», кластер примитивов).
    JPH::ShapeRefC shape;
    if (!desc.Children.empty()) {
        JPH::StaticCompoundShapeSettings compound;
        // Держим ссылки на дочерние формы живыми до Create() (компаунду нужен ≥1
        // валидный подшейп; StaticCompoundShape требует минимум 2 — при одной
        // добавляем её же копию, чтобы не падать).
        std::vector<JPH::ShapeRefC> keepAlive;
        for (const ChildShape& c : desc.Children) {
            JPH::ShapeRefC child = MakeShape(c.Shape, c.HalfExtents, c.Radius, c.HalfHeight);
            if (!child) continue;
            keepAlive.push_back(child);
            compound.AddShape(JPH::Vec3(c.Position.x, c.Position.y, c.Position.z),
                              ToJoltQuat(c.Rotation), child.GetPtr());
        }
        if (keepAlive.empty()) {
            LOG_ERROR("Jolt") << "Составная форма без валидных подшейпов";
            return kInvalidBody;
        }
        // StaticCompoundShape требует ≥2 подшейпа — дублируем единственный.
        if (keepAlive.size() == 1) {
            compound.AddShape(JPH::Vec3::sZero(), JPH::Quat::sIdentity(), keepAlive[0].GetPtr());
        }
        JPH::Shape::ShapeResult res = compound.Create();
        if (res.HasError()) {
            LOG_ERROR("Jolt") << "Составная форма не создана: " << res.GetError().c_str();
            return kInvalidBody;
        }
        shape = res.Get();
    } else if (desc.Shape == ShapeType::ConvexHull || desc.Shape == ShapeType::Mesh) {
        shape = ShapeFromGeometry(desc, dynamic);
    } else {
        shape = MakeShape(desc.Shape, desc.HalfExtents, desc.Radius, desc.HalfHeight);
    }
    if (!shape) return kInvalidBody; // форма не создалась — тела не будет (не падаем)

    JPH::EMotionType motion = JPH::EMotionType::Dynamic;
    JPH::ObjectLayer layer = ObjectLayers::MOVING;
    switch (type) {
        case BodyType::Static:    motion = JPH::EMotionType::Static;    layer = ObjectLayers::NON_MOVING; break;
        case BodyType::Kinematic: motion = JPH::EMotionType::Kinematic; layer = ObjectLayers::MOVING; break;
        case BodyType::Dynamic:   motion = JPH::EMotionType::Dynamic;   layer = ObjectLayers::MOVING; break;
    }

    JPH::BodyCreationSettings settings(shape, ToJolt(desc.Position), ToJoltQuat(desc.Rotation),
                                       motion, layer);
    settings.mFriction = glm::max(desc.Friction, 0.0f);
    settings.mRestitution = glm::clamp(desc.Restitution, 0.0f, 1.0f);
    // Сенсор обнаруживает касание, но не отталкивает. Событие о нём приходит
    // тем же PollContacts — игре незачем знать, зона это или стена, пока она
    // не решит, что с этим делать.
    settings.mIsSensor = desc.Sensor;
    if (desc.Sensor) settings.mCollideKinematicVsNonDynamic = true;
    if (dynamic) {
        if (desc.Mass > 0.0f) {
            // Масса задана, инерция — ОТ ФОРМЫ, приведённая к этой массе: доска
            // крутится вдоль длинной оси легче, чем поперёк, при любой массе.
            settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            settings.mMassPropertiesOverride.mMass = desc.Mass;
        }
        settings.mLinearDamping = glm::clamp(desc.LinearDamping, 0.0f, 1.0f);
        settings.mAngularDamping = glm::clamp(desc.AngularDamping, 0.0f, 1.0f);
        settings.mGravityFactor = desc.GravityScale;
        // CCD. Без него Jolt ищет контакты только в конечной точке шага с
        // запасом в 2 см: тело на 10 м/с проходит за шаг 17 см и оказывается
        // в полу на 15, а выталкивание (Baumgarte) возвращает его несколько
        // кадров подряд. Непрерывное тело проверяет путь целиком и
        // останавливается у поверхности.
        settings.mMotionQuality =
            desc.Continuous ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete;
        // Гироскопический момент — то, что делает вращение правильным, а не
        // «примерно»: подброшенная ракетка переворачивается вокруг средней
        // оси, волчок не заваливается от численной ошибки.
        settings.mApplyGyroscopicForce = true;
        // Скольжение по сетке без подскоков на внутренних рёбрах соседних
        // треугольников (ящик, едущий по ландшафту, «спотыкался» на стыках).
        settings.mEnhancedInternalEdgeRemoval = true;
        settings.mAllowedDOFs = (JPH::EAllowedDOFs)((uint8_t)JPH::EAllowedDOFs::All & ~locks);
        settings.mLinearVelocity = JPH::Vec3(desc.LinearVelocity.x, desc.LinearVelocity.y, desc.LinearVelocity.z);
        settings.mAngularVelocity = JPH::Vec3(desc.AngularVelocity.x, desc.AngularVelocity.y, desc.AngularVelocity.z);
    }

    JPH::BodyInterface& bi = m_system->GetBodyInterface();
    JPH::BodyID id = bi.CreateAndAddBody(
        settings, type == BodyType::Static ? JPH::EActivation::DontActivate
                                           : JPH::EActivation::Activate);
    if (id.IsInvalid()) return kInvalidBody;

    BodyHandle h = m_next++;
    const uint32_t joltId = id.GetIndexAndSequenceNumber();
    m_bodies[h] = joltId;
    m_byJoltId[joltId] = h;
    m_layers[h] = desc.Layer;
    return h;
}

void JoltWorld::RemoveBody(BodyHandle body) {
    auto it = m_bodies.find(body);
    if (it == m_bodies.end() || !m_system) return;
    JPH::BodyID id(it->second);
    // Соединения этого тела снимаются ДО него: Jolt держит в соединении сырой
    // указатель на тело, и шаг с соединением на удалённое тело читает
    // освобождённую память. Удалить объект, прикреплённый суставом, значило
    // уронить игру на следующем кадре.
    for (auto j = m_joints.begin(); j != m_joints.end();) {
        const auto* c = static_cast<const JPH::TwoBodyConstraint*>(j->second);
        if (c->GetBody1()->GetID() == id || c->GetBody2()->GetID() == id) {
            ReleaseConstraint(j->second);
            j = m_joints.erase(j);
        } else {
            ++j;
        }
    }
    JPH::BodyInterface& bi = m_system->GetBodyInterface();
    bi.RemoveBody(id);
    bi.DestroyBody(id);
    m_byJoltId.erase(it->second);
    m_layers.erase(body);
    m_forces.erase(body);
    m_kinTargets.erase(body);
    m_kinMoving.erase(body);
    m_bodies.erase(it);

    // Формы, которые больше никому не нужны (ссылку держит только кэш), —
    // отпускаем: игра, перебирающая модели, иначе копила бы их до конца сцены.
    for (auto c = m_shapeCache.begin(); c != m_shapeCache.end();) {
        const bool hullFree = !c->second->Hull || c->second->Hull->GetRefCount() == 1;
        const bool meshFree = !c->second->Mesh || c->second->Mesh->GetRefCount() == 1;
        c = hullFree && meshFree ? m_shapeCache.erase(c) : std::next(c);
    }
}

void JoltWorld::Step(float dt) {
    if (!m_system || !(dt > 0.0f)) return;
    m_lastSubSteps = 0;

    // ПОЛУФИКСИРОВАННЫЙ шаг: кадр делится на равные подшаги не длиннее 1/120 с.
    //
    // Раньше был строго фиксированный 1/60 с накопителем, и при частоте кадров,
    // не кратной 60, одни кадры получали шаг физики, а другие — нет: на 144 Гц
    // тело стояло на месте два-три кадра и прыгало на третьем, а на 50 Гц раз в
    // пять кадров делало двойной шаг. Отсюда рывки при ровном падении. Теперь
    // каждый кадр продвигает физику ровно на своё время, а устойчивость держит
    // верхний предел длины подшага.
    //
    // Предел — 1/120, а не 1/60: на 60 Гц стопка из десяти ящиков с перепадом
    // масс 10:1 расползалась и падала, на 120 Гц стоит (проверено перебором
    // масс, высот и частот кадров). Цена — два шага решателя на кадр при 60
    // кадрах, для сцен игрового размера это доли миллисекунды.
    //
    // Нижний предел — чтобы редактор на 1000 кадрах в секунду не гонял решатель
    // тысячу раз: очень короткий кадр копится до 1/240 с.
    constexpr float kMaxStep = 1.0f / 120.0f;
    constexpr float kMinStep = 1.0f / 240.0f;
    constexpr int kMaxSubSteps = 16;
    // Угловая скорость, с которой тело считается вращающимся (≈3°/с). Ниже —
    // дрожь покоя: сбрасывать кэш из-за неё значит раскачивать стопки.
    constexpr float kSpinningSq = 0.05f * 0.05f;
    // Потолок кадра: после паузы (свернули окно, грузилась сцена) физика не
    // пытается отсчитать секунды разом — это стоило бы кадра и взрыва мира.
    const float frame = glm::min(dt, kMaxStep * kMaxSubSteps);
    m_accum += frame;

    // Силы, заказанные на этот кадр, превращаются в импульс за его время —
    // тогда разгон не зависит от того, на сколько подшагов кадр разобьётся.
    for (auto& [h, f] : m_forces) {
        f.LinearImpulse += f.Force * frame;
        f.AngularImpulse += f.Torque * frame;
        f.Force = f.Torque = glm::vec3(0.0f);
    }
    if (m_accum < kMinStep) return;

    const int n = std::clamp((int)std::ceil(m_accum / kMaxStep - 1e-3f), 1, kMaxSubSteps);
    const float h = m_accum / (float)n;
    m_accum = 0.0f;

    JPH::BodyInterface& bi = m_system->GetBodyInterface();

    // Кинематика, которую в этом кадре не вели, должна остановиться: скорость,
    // выставленная MoveKinematic, у Jolt сохраняется, и забытая платформа
    // уехала бы в бесконечность.
    for (auto it = m_kinMoving.begin(); it != m_kinMoving.end();) {
        if (m_kinTargets.count(*it)) { ++it; continue; }
        auto b = m_bodies.find(*it);
        if (b != m_bodies.end())
            bi.SetLinearAndAngularVelocity(JPH::BodyID(b->second), JPH::Vec3::sZero(), JPH::Vec3::sZero());
        it = m_kinMoving.erase(it);
    }
    struct KinPath {
        JPH::BodyID Id;
        glm::vec3 P0, P1;
        glm::quat Q0, Q1;
    };
    std::vector<KinPath> kin;
    kin.reserve(m_kinTargets.size());
    for (const auto& [handle, target] : m_kinTargets) {
        auto b = m_bodies.find(handle);
        if (b == m_bodies.end()) continue;
        KinPath k;
        k.Id = JPH::BodyID(b->second);
        k.P0 = FromJolt(bi.GetPosition(k.Id));
        k.Q0 = FromJoltQuat(bi.GetRotation(k.Id));
        k.P1 = target.Position;
        k.Q1 = glm::normalize(target.Rotation);
        kin.push_back(k);
        m_kinMoving.insert(handle);
    }
    m_kinTargets.clear();

    for (int i = 0; i < n; ++i) {
        const float t = (float)(i + 1) / (float)n;
        for (const KinPath& k : kin)
            bi.MoveKinematic(k.Id, ToJolt(glm::mix(k.P0, k.P1, t)), ToJoltQuat(glm::slerp(k.Q0, k.Q1, t)), h);
        // Jolt обнуляет накопленные силы после каждого шага — поэтому доля
        // импульса отдаётся силой на КАЖДОМ подшаге.
        for (const auto& [handle, f] : m_forces) {
            auto b = m_bodies.find(handle);
            if (b == m_bodies.end()) continue;
            const JPH::BodyID id(b->second);
            const glm::vec3 force = f.LinearImpulse / (h * (float)n);
            const glm::vec3 torque = f.AngularImpulse / (h * (float)n);
            if (glm::dot(force, force) > 0.0f) bi.AddForce(id, JPH::Vec3(force.x, force.y, force.z));
            if (glm::dot(torque, torque) > 0.0f) bi.AddTorque(id, JPH::Vec3(torque.x, torque.y, torque.z));
        }
        // Кэш пар тел: Jolt не пересчитывает контакт, пока тела сдвинулись
        // друг относительно друга меньше чем на 1 мм и повернулись меньше чем
        // на 2°, — а берёт старый манифольд. Лежащим телам это экономит
        // время и держит стопки неподвижными. КАТЯЩИМСЯ — вредит: точка
        // контакта на шаре уезжает вместе с его поверхностью, нормаль
        // наклоняется, и контакт толкает шар вперёд и вдавливает в пол.
        // Медленно катящийся шар поэтому не останавливался никогда и
        // постепенно тонул. Кэш сбрасываем только у вращающихся тел.
        // Список активных тел читается «небезопасно» — здесь это можно: шаг
        // ещё не начат, других потоков у системы сейчас нет.
        const JPH::BodyID* active = m_system->GetActiveBodiesUnsafe(JPH::EBodyType::RigidBody);
        const JPH::uint32 activeCount = m_system->GetNumActiveBodies(JPH::EBodyType::RigidBody);
        for (JPH::uint32 k = 0; k < activeCount; ++k)
            if (bi.GetAngularVelocity(active[k]).LengthSq() > kSpinningSq) bi.InvalidateContactCache(active[k]);
        m_system->Update(h, 1, m_tempAllocator.get(), m_jobSystem.get());
    }
    m_forces.clear();
    m_lastSubSteps = n;
}

void JoltWorld::GetBodyTransform(BodyHandle body, glm::vec3& position, glm::quat& rotation) const {
    auto it = m_bodies.find(body);
    if (it == m_bodies.end() || !m_system) return;
    JPH::BodyID id(it->second);
    const JPH::BodyInterface& bi = m_system->GetBodyInterface();
    position = FromJolt(bi.GetPosition(id));
    rotation = FromJoltQuat(bi.GetRotation(id));
}

void JoltWorld::SetBodyTransform(BodyHandle body, const glm::vec3& position, const glm::quat& rotation) {
    auto it = m_bodies.find(body);
    if (it == m_bodies.end() || !m_system) return;
    JPH::BodyID id(it->second);
    JPH::BodyInterface& bi = m_system->GetBodyInterface();
    // Кинематику ведём «мягко»: цель запоминается и проходится по подшагам
    // следующего Step со скоростью, которую Jolt передаёт тем, кого она
    // толкает. Телепорт кинематики сквозь ящик выбил бы его без всякого
    // трения. Прочие тела переставляем жёстко.
    if (bi.GetMotionType(id) == JPH::EMotionType::Kinematic) {
        m_kinTargets[body] = KinematicTarget{position, rotation};
    } else {
        bi.SetPositionAndRotation(id, ToJolt(position), ToJoltQuat(rotation), JPH::EActivation::Activate);
    }
}

void JoltWorld::SetLinearVelocity(BodyHandle body, const glm::vec3& velocity) {
    auto it = m_bodies.find(body);
    if (it == m_bodies.end() || !m_system) return;
    JPH::BodyID id(it->second);
    m_system->GetBodyInterface().SetLinearVelocity(id, JPH::Vec3(velocity.x, velocity.y, velocity.z));
}

glm::vec3 JoltWorld::GetLinearVelocity(BodyHandle body) const {
    auto it = m_bodies.find(body);
    if (it == m_bodies.end() || !m_system) return glm::vec3(0.0f);
    JPH::BodyID id(it->second);
    return FromJolt(m_system->GetBodyInterface().GetLinearVelocity(id));
}

void JoltWorld::AddImpulse(BodyHandle body, const glm::vec3& impulse) {
    auto it = m_bodies.find(body);
    if (it == m_bodies.end() || !m_system) return;
    JPH::BodyID id(it->second);
    m_system->GetBodyInterface().AddImpulse(id, JPH::Vec3(impulse.x, impulse.y, impulse.z));
}

void JoltWorld::SetAngularVelocity(BodyHandle body, const glm::vec3& w) {
    auto it = m_bodies.find(body);
    if (it == m_bodies.end() || !m_system) return;
    JPH::BodyInterface& bi = m_system->GetBodyInterface();
    const JPH::BodyID id(it->second);
    if (bi.GetMotionType(id) == JPH::EMotionType::Static) return;
    bi.SetAngularVelocity(id, JPH::Vec3(w.x, w.y, w.z));
    // Спящее тело скорость хранит, но не движется — будим, как и при толчке.
    bi.ActivateBody(id);
}

glm::vec3 JoltWorld::GetAngularVelocity(BodyHandle body) const {
    auto it = m_bodies.find(body);
    if (it == m_bodies.end() || !m_system) return glm::vec3(0.0f);
    return FromJolt(m_system->GetBodyInterface().GetAngularVelocity(JPH::BodyID(it->second)));
}

void JoltWorld::AddImpulseAtPoint(BodyHandle body, const glm::vec3& impulse, const glm::vec3& point) {
    auto it = m_bodies.find(body);
    if (it == m_bodies.end() || !m_system) return;
    m_system->GetBodyInterface().AddImpulse(JPH::BodyID(it->second), JPH::Vec3(impulse.x, impulse.y, impulse.z),
                                            ToJolt(point));
}

void JoltWorld::AddAngularImpulse(BodyHandle body, const glm::vec3& impulse) {
    auto it = m_bodies.find(body);
    if (it == m_bodies.end() || !m_system) return;
    m_system->GetBodyInterface().AddAngularImpulse(JPH::BodyID(it->second),
                                                   JPH::Vec3(impulse.x, impulse.y, impulse.z));
}

void JoltWorld::AddForce(BodyHandle body, const glm::vec3& force) {
    if (!m_bodies.count(body)) return;
    m_forces[body].Force += force;
}

void JoltWorld::AddForceAtPoint(BodyHandle body, const glm::vec3& force, const glm::vec3& point) {
    auto it = m_bodies.find(body);
    if (it == m_bodies.end() || !m_system) return;
    // Сила в точке = та же сила в центр масс + момент плеча. Плечо снимаем
    // сейчас: за один кадр тело поворачивается на доли градуса.
    const glm::vec3 com =
        FromJolt(m_system->GetBodyInterface().GetCenterOfMassPosition(JPH::BodyID(it->second)));
    PendingForce& f = m_forces[body];
    f.Force += force;
    f.Torque += glm::cross(point - com, force);
}

void JoltWorld::AddTorque(BodyHandle body, const glm::vec3& torque) {
    if (!m_bodies.count(body)) return;
    m_forces[body].Torque += torque;
}

bool JoltWorld::IsSleeping(BodyHandle body) const {
    auto it = m_bodies.find(body);
    if (it == m_bodies.end() || !m_system) return false;
    const JPH::BodyInterface& bi = m_system->GetBodyInterface();
    const JPH::BodyID id(it->second);
    return bi.GetMotionType(id) == JPH::EMotionType::Dynamic && !bi.IsActive(id);
}

JointHandle JoltWorld::CreateJoint(const JointDesc& desc) {
    if (!m_system) return kInvalidJoint;

    // Тело A обязательно; тело B == invalid -> крепим к неподвижному «миру».
    auto ia = m_bodies.find(desc.BodyA);
    if (ia == m_bodies.end()) return kInvalidJoint;

    const JPH::BodyLockInterface& lock = m_system->GetBodyLockInterfaceNoLock();
    JPH::Body* a = lock.TryGetBody(JPH::BodyID(ia->second));
    if (!a) return kInvalidJoint;

    JPH::Body* b = &JPH::Body::sFixedToWorld;
    if (desc.BodyB != kInvalidBody) {
        auto ib = m_bodies.find(desc.BodyB);
        if (ib == m_bodies.end()) return kInvalidJoint;
        b = lock.TryGetBody(JPH::BodyID(ib->second));
        if (!b) return kInvalidJoint;
    }

    JPH::RVec3 anchor = ToJolt(desc.Anchor);
    JPH::Vec3 axis = JPH::Vec3(desc.Axis.x, desc.Axis.y, desc.Axis.z).NormalizedOr(JPH::Vec3::sAxisY());

    JPH::Constraint* constraint = nullptr;
    switch (desc.Type) {
        case JointType::Fixed: {
            JPH::FixedConstraintSettings s;
            s.mSpace = JPH::EConstraintSpace::WorldSpace;
            s.mAutoDetectPoint = true; // фиксируем текущее взаимное положение тел
            constraint = s.Create(*a, *b);
            break;
        }
        case JointType::Point: {
            JPH::PointConstraintSettings s;
            s.mSpace = JPH::EConstraintSpace::WorldSpace;
            s.mPoint1 = s.mPoint2 = anchor;
            constraint = s.Create(*a, *b);
            break;
        }
        case JointType::Hinge: {
            JPH::HingeConstraintSettings s;
            s.mSpace = JPH::EConstraintSpace::WorldSpace;
            s.mPoint1 = s.mPoint2 = anchor;
            s.mHingeAxis1 = s.mHingeAxis2 = axis;
            JPH::Vec3 normal = PerpendicularTo(axis);
            s.mNormalAxis1 = s.mNormalAxis2 = normal;
            if (desc.UseLimits) {
                s.mLimitsMin = glm::radians(desc.MinLimit);
                s.mLimitsMax = glm::radians(desc.MaxLimit);
            }
            constraint = s.Create(*a, *b);
            break;
        }
        case JointType::Slider: {
            JPH::SliderConstraintSettings s;
            s.mSpace = JPH::EConstraintSpace::WorldSpace;
            s.mPoint1 = s.mPoint2 = anchor;
            s.SetSliderAxis(axis); // выставляет ось скольжения + нормали
            if (desc.UseLimits) {
                s.mLimitsMin = desc.MinLimit;
                s.mLimitsMax = desc.MaxLimit;
            }
            constraint = s.Create(*a, *b);
            break;
        }
        case JointType::Distance: {
            // Трос: точка крепления — на ВТОРОМ теле (крюк, столб, мир), а
            // первое тело держится за свой ЦЕНТР. Крепить оба конца к одной
            // точке, как у остальных соединений, здесь нельзя: длина между
            // ними тогда равна нулю, ограничение расталкивает тела в
            // произвольную сторону, а тело уезжает на длину троса плюс вылет
            // своего крепления — маятник ложится на пол вместо того, чтобы
            // качаться.
            JPH::DistanceConstraintSettings s;
            s.mSpace = JPH::EConstraintSpace::WorldSpace;
            s.mPoint1 = a->GetCenterOfMassPosition();
            s.mPoint2 = anchor;
            s.mMinDistance = desc.MinDistance;
            s.mMaxDistance = desc.MaxDistance;
            constraint = s.Create(*a, *b);
            break;
        }
        case JointType::Cone: {
            JPH::ConeConstraintSettings s;
            s.mSpace = JPH::EConstraintSpace::WorldSpace;
            s.mPoint1 = s.mPoint2 = anchor;
            s.mTwistAxis1 = s.mTwistAxis2 = axis;
            s.mHalfConeAngle = glm::radians(glm::clamp(desc.ConeHalfAngle, 0.0f, 180.0f));
            constraint = s.Create(*a, *b);
            break;
        }
    }
    if (!constraint) return kInvalidJoint;

    constraint->AddRef(); // держим ссылку сами (карта m_joints) — иначе удалится
    m_system->AddConstraint(constraint);
    JointHandle h = m_nextJoint++;
    m_joints[h] = constraint;
    return h;
}

void JoltWorld::ReleaseConstraint(JPH::Constraint* constraint) {
    // Тела соединения будим: улёгшийся маятник спит, и без этого груз, у
    // которого перерезали трос, так и висел бы в воздухе — снятие соединения
    // Jolt сном не считает.
    const auto* c = static_cast<const JPH::TwoBodyConstraint*>(constraint);
    JPH::BodyInterface& bi = m_system->GetBodyInterfaceNoLock();
    for (const JPH::Body* b : {c->GetBody1(), c->GetBody2()})
        if (b && !b->IsStatic() && b->IsInBroadPhase()) bi.ActivateBody(b->GetID());
    m_system->RemoveConstraint(constraint);
    constraint->Release();
}

void JoltWorld::RemoveJoint(JointHandle joint) {
    auto it = m_joints.find(joint);
    if (it == m_joints.end() || !m_system) return;
    ReleaseConstraint(it->second);
    m_joints.erase(it);
}
