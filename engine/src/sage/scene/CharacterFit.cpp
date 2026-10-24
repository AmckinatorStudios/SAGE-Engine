#include "sage/scene/CharacterFit.h"

#include <algorithm>

#include "sage/render/MeshData.h"
#include "sage/scene/Components.h"

namespace sage::scene {

namespace {

bool BoundsOf(const MeshRendererComponent& mr, glm::vec3& mn, glm::vec3& mx) {
    if (mr.MeshPtr) {
        mn = mr.MeshPtr->BoundsMin();
        mx = mr.MeshPtr->BoundsMax();
        if (mx.y > mn.y) return true;
    }
    // Встроенная форма без загруженного меша (без видеокарты, до первого
    // кадра): габариты — из той же процедуры, которой форма строится.
    sage::render::MeshData data;
    switch (mr.Ref.type) {
        case MeshRef::Type::Cube: data = sage::render::BuildCube(); break;
        case MeshRef::Type::Sphere: data = sage::render::BuildSphere(8, 8); break;
        case MeshRef::Type::Cylinder: data = sage::render::BuildCylinder(8); break;
        case MeshRef::Type::Cone: data = sage::render::BuildCone(8); break;
        case MeshRef::Type::Capsule: data = sage::render::BuildCapsule(4, 8); break;
        default: return false;
    }
    if (data.Vertices.empty()) return false;
    mn = mx = data.Vertices.front().Position;
    for (const auto& v : data.Vertices) {
        mn = glm::min(mn, v.Position);
        mx = glm::max(mx, v.Position);
    }
    return mx.y > mn.y;
}

} // namespace

bool FitCharacterToMesh(entt::registry& reg, entt::entity e) {
    CharacterControllerComponent* cc = reg.try_get<CharacterControllerComponent>(e);
    const MeshRendererComponent* mr = reg.try_get<MeshRendererComponent>(e);
    const Transform* tr = reg.try_get<Transform>(e);
    if (!cc || !mr || !tr) return false;
    glm::vec3 mn, mx;
    if (!BoundsOf(*mr, mn, mx)) return false;
    const glm::vec3 s = glm::abs(tr->Scale);
    const float height = (mx.y - mn.y) * s.y;
    const float width = std::max((mx.x - mn.x) * s.x, (mx.z - mn.z) * s.z);
    if (height <= 1e-4f || width <= 1e-4f) return false;
    cc->Height = height;
    // Радиус не больше половины роста: иначе капсула вырождается в шар шире
    // самого персонажа.
    cc->Radius = std::min(width * 0.5f, height * 0.5f);
    // Подошвы — низ меша; по горизонтали — его середина (модель может быть
    // сдвинута относительно своей точки).
    cc->Center = glm::vec3((mn.x + mx.x) * 0.5f * s.x, mn.y * s.y, (mn.z + mx.z) * 0.5f * s.z);
    // Ступенька — не выше трети роста: маленький персонаж с ногами по колено
    // взрослому не должен вшагивать на стол.
    cc->StepOffset = std::min(cc->StepOffset, height / 3.0f);
    return true;
}

} // namespace sage::scene
