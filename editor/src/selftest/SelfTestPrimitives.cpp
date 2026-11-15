// ---------------------------------------------------------------------------
// САМОПРОВЕРКА: НАСТРОЙКИ ГЕОМЕТРИИ ПРИМИТИВА (MeshRef::prim) на настоящих
// GPU-мешах — то, что модульным тестам недоступно без видеокарты.
//
// Правка одного объекта строит ЕМУ свой меш и не трогает ни общий меш формы,
// ни соседей; настройки переживают сохранение и загрузку сцены, отмену и
// повтор, дублирование; Transform → Scale от них не зависит; сцена без
// настроек открывается прежней геометрией.
// ---------------------------------------------------------------------------
#include "EditorLayer.h"

#include <algorithm>

#include "sage/core/Log.h"
#include "sage/render/ResourceManager.h"
#include "sage/scene/SceneSerializer.h"

namespace {

// Высота меша по копии геометрии (редактор держит её для выбора мышью).
float MeshHeight(const Mesh* m) {
    if (!m || !m->CpuVertices() || m->CpuVertices()->empty()) return -1.0f;
    float lo = 1e9f, hi = -1e9f;
    for (const Vertex& v : *m->CpuVertices()) {
        lo = std::min(lo, v.Position.y);
        hi = std::max(hi, v.Position.y);
    }
    return hi - lo;
}

} // namespace

bool EditorLayer::SelfTestPrimitives() {
    bool ok = true;
    auto fail = [&](const char* why) {
        LOG_ERROR("Editor") << "SELFTEST: primitive-geometry: " << why;
        ok = false;
    };
    ResourceManager& rm = ResourceManager::Instance();
    GameObject a = CreatePrimitiveEntity("SelftestCapsuleA", MeshRef::Type::Capsule);
    GameObject b = CreatePrimitiveEntity("SelftestCapsuleB", MeshRef::Type::Capsule);
    if (!a.Valid() || !b.Valid()) {
        fail("не создались капсулы");
        return false;
    }
    const std::shared_ptr<Mesh> shared = rm.GetPrimitive(MeshRef::Type::Capsule);
    const glm::vec3 scaleBefore = a.GetTransform().Scale;

    // Правка одного объекта — как её делает инспектор: снимок, настройки, меш.
    PushUndoSnapshot();
    MeshRendererComponent& mra = a.Renderer();
    mra.Ref.prim.Height = 4.0f;
    mra.Ref.prim.Segments = 8;
    mra.Ref.prim.Flat = true;
    mra.MeshPtr = rm.GetPrimitive(mra.Ref.type, mra.Ref.prim);
    if (!mra.MeshPtr || mra.MeshPtr == shared) fail("у объекта не свой меш");
    if (b.Renderer().MeshPtr != shared) fail("сосед того же типа потерял общий меш");
    if (shared && std::abs(MeshHeight(shared.get()) - 2.0f) > 1e-3f) fail("общий меш капсулы изменился");
    if (const float h = MeshHeight(mra.MeshPtr.get()); h >= 0.0f && std::abs(h - 4.0f) > 1e-3f)
        fail("высота капсулы не стала 4");
    if (a.GetTransform().Scale != scaleBefore) fail("высота капсулы тронула Scale");
    // Тот же вариант у второго объекта — тот же меш из кэша.
    b.Renderer().Ref.prim = mra.Ref.prim;
    if (rm.GetPrimitive(MeshRef::Type::Capsule, b.Renderer().Ref.prim) != mra.MeshPtr) fail("одинаковые варианты не делят меш");
    b.Renderer().Ref.prim = {};

    // Сохранение и загрузка.
    const int idA = a.Id();
    {
        const std::string text = SceneSerializer::SaveToString(*m_scene);
        std::unique_ptr<Scene> back = SceneSerializer::LoadFromString(text);
        GameObject la = back ? back->FindByName("SelftestCapsuleA") : GameObject();
        GameObject lb = back ? back->FindByName("SelftestCapsuleB") : GameObject();
        if (!la.Valid() || !lb.Valid()) {
            fail("после загрузки нет капсул");
        } else {
            const MeshRef& r = la.Renderer().Ref;
            if (r.prim.Height != 4.0f || r.prim.Segments != 8 || !r.prim.Flat) fail("настройки не пережили загрузку");
            if (!la.Renderer().MeshPtr || la.Renderer().MeshPtr == shared) fail("после загрузки — общий меш вместо своего");
            if (!lb.Renderer().Ref.prim.IsDefault() || lb.Renderer().MeshPtr != shared)
                fail("сцена без настроек открылась не прежней геометрией");
        }
    }

    // Отмена и повтор: снимок истории — та же сериализация.
    Undo();
    GameObject ua = m_scene->Get(idA);
    if (!ua.Valid() || !ua.Renderer().Ref.prim.IsDefault() || ua.Renderer().MeshPtr != shared)
        fail("отмена не вернула капсулу по умолчанию");
    Redo();
    GameObject ra = m_scene->Get(idA);
    if (!ra.Valid() || ra.Renderer().Ref.prim.Height != 4.0f || ra.Renderer().MeshPtr == shared)
        fail("повтор не вернул настройки");

    // Дублирование — с настройками и тем же мешем варианта.
    if (ra.Valid()) {
        GameObject copy = DuplicateEntity(ra);
        if (!copy.Valid() || !(copy.Renderer().Ref.prim == ra.Renderer().Ref.prim) ||
            copy.Renderer().MeshPtr != ra.Renderer().MeshPtr)
            fail("копия потеряла настройки геометрии");
        if (copy.Valid()) m_scene->RemoveObject(copy.Id());
    }

    for (const char* name : {"SelftestCapsuleA", "SelftestCapsuleB"})
        if (GameObject g = m_scene->FindByName(name); g.Valid()) m_scene->RemoveObject(g.Id());
    m_selection.Clear();
    if (ok) LOG_INFO("Editor") << "SELFTEST: primitive-geometry OK";
    return ok;
}
