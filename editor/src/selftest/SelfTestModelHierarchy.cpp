#include "SelfTestCommon.h"
#include "ModelHierarchy.h"

// ---------------------------------------------------------------------------
// МОДЕЛЬ ИЗ НЕСКОЛЬКИХ УЗЛОВ — В СЦЕНУ ДЕРЕВОМ ОБЪЕКТОВ.
//
// Персонаж Blockbench (tests/data/blockbench_skin) кладётся в проект и
// ставится обычным «добавить в сцену». Проверяется то, что видно человеку:
// корень с именем модели, под ним объекты с именами узлов файла, у частей —
// свой меш (только своего узла) и материал, а вся модель стоит там же, где
// стояла бы целиком.
// ---------------------------------------------------------------------------
bool EditorLayer::SelfTestModelHierarchy() {
    bool ok = true;
    auto fail = [&](const std::string& why) {
        LOG_ERROR("Editor") << "SELFTEST: model-hierarchy: " << why;
        ok = false;
    };
    const fs::path src = fs::path(SAGE_TEST_DATA_DIR) / "blockbench_skin";
    const fs::path dir = m_project.AssetsDir() / "selftest_steve";
    std::error_code ec;
    fs::create_directories(dir, ec);
    for (const char* f : {"model.gltf", "05119a8b74f4a93d.png"})
        fs::copy_file(src / f, dir / f, fs::copy_options::overwrite_existing, ec);
    const std::string ref = m_project.AssetRef(dir / "model.gltf");
    if (!sage::editor::modelhierarchy::PlacesAsHierarchy(ref)) {
        fail("модель из 12 частей не ставится иерархией");
        return false;
    }
    const size_t before = m_scene->Count();
    if (!AddAssetToScene(dir / "model.gltf")) fail("модель не встала в сцену");
    GameObject root = m_scene->Get(m_selection.Primary());
    if (!root.Valid() || root.Name() != "model") {
        fail("выделен не корень модели");
        return false;
    }
    // Все объекты дерева: корень, пояс, голова/тело/руки/ноги и их части.
    int parts = 0, named = 0;
    glm::vec3 lo(1e9f), hi(-1e9f);
    std::vector<entt::entity> stack{root.Entity()};
    while (!stack.empty()) {
        const entt::entity e = stack.back();
        stack.pop_back();
        for (entt::entity c : m_scene->ChildrenOf(e)) stack.push_back(c);
        GameObject o(&m_scene->Registry(), e);
        if (o.Name() == "Head" || o.Name() == "Hat Layer" || o.Name() == "Left Leg") ++named;
        const MeshRendererComponent* mr = m_scene->Registry().try_get<MeshRendererComponent>(e);
        if (!mr || !mr->MeshPtr) continue;
        if (mr->Ref.part < 0) fail("часть модели рисует модель целиком: " + o.Name());
        if (!mr->MaterialPtr) fail("у части нет материала: " + o.Name());
        ++parts;
        const glm::mat4 w = m_scene->WorldMatrix(e);
        for (int k = 0; k < 8; ++k) {
            const glm::vec3 b0 = mr->MeshPtr->BoundsMin(), b1 = mr->MeshPtr->BoundsMax();
            const glm::vec3 p((k & 1) ? b1.x : b0.x, (k & 2) ? b1.y : b0.y, (k & 4) ? b1.z : b0.z);
            const glm::vec3 q = glm::vec3(w * glm::vec4(p, 1.0f)) - glm::vec3(root.GetTransform().Position);
            lo = glm::min(lo, q);
            hi = glm::max(hi, q);
        }
    }
    if (parts != 12) fail("частей с мешем " + std::to_string(parts) + " вместо 12");
    if (named < 3) fail("у объектов нет имён узлов файла");
    // Вся модель — на своём месте: ступни на полу, макушка на 2.03.
    if (std::abs(lo.y + 0.016f) > 0.01f || std::abs(hi.y - 2.031f) > 0.01f || std::abs(hi.x - 0.516f) > 0.01f)
        fail("части стоят не на своих местах");
    // И сохраняется: часть после перезагрузки сцены рисует ту же геометрию.
    const std::string saved = SceneSerializer::SaveToString(*m_scene);
    std::unique_ptr<Scene> back = SceneSerializer::LoadFromString(saved);
    int partsBack = 0;
    if (back)
        for (auto e : back->Registry().view<MeshRendererComponent>())
            if (back->Registry().get<MeshRendererComponent>(e).Ref.part >= 0 &&
                back->Registry().get<MeshRendererComponent>(e).MeshPtr)
                ++partsBack;
    if (partsBack != 12) fail("после сохранения частей " + std::to_string(partsBack));
    Undo();
    if (m_scene->Count() != before) fail("один Ctrl+Z не убрал модель целиком");
    if (ok) LOG_INFO("Editor") << "SELFTEST: model-hierarchy OK";
    return ok;
}
