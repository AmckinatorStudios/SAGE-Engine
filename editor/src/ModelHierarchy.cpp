#include "ModelHierarchy.h"

#include <vector>

#include "Project.h"
#include "sage/assets/AssetDatabase.h"
#include "sage/assets/import/Importer.h"
#include "sage/assets/import/ModelProbe.h"
#include "sage/core/Log.h"
#include "sage/render/ModelLoader.h"
#include "sage/render/ResourceManager.h"
#include "sage/scene/Scene.h"

namespace sage::editor::modelhierarchy {

namespace {

// Дерево модели, если её ставить иерархией; false — ставить целиком.
bool TreeFor(const std::string& modelRef, sage::assets::ImportedScene& scene, glm::mat4& root) {
    const std::string path = sage::AssetDatabase::Instance().LocatePath(modelRef);
    if (path.empty()) return false;
    if (!ModelLoader::LoadImportSettings(path).Hierarchy) return false;
    if (sage::assets::ModelHasSkeleton(path)) return false;
    std::string err;
    if (!ModelLoader::LoadModelTree(path, scene, root, err)) return false;
    if (scene.Groups.empty()) return false;
    // Геометрия вне дерева — ей не нашлось бы объекта: такую модель честнее
    // поставить целиком, чем потерять часть.
    int withGeometry = 0;
    std::vector<bool> counted(scene.Groups.size(), false);
    for (const sage::assets::ImportedNode& n : scene.Nodes) {
        if (n.Group < 0 || n.Group >= (int)scene.Groups.size()) return false;
        if (!counted[(size_t)n.Group]) {
            counted[(size_t)n.Group] = true;
            ++withGeometry;
        }
    }
    return withGeometry > 1;
}

void Add(ModelMaterialImportResult& total, const ModelMaterialImportResult& part) {
    total.Created += part.Created;
    total.Assigned += part.Assigned;
    total.AnyMaps = total.AnyMaps || part.AnyMaps;
    if (total.FirstWarning.empty()) total.FirstWarning = part.FirstWarning;
    total.FileMaterials = std::max(total.FileMaterials, part.FileMaterials);
    total.Parts += part.Parts;
}

} // namespace

bool PlacesAsHierarchy(const std::string& modelRef) {
    sage::assets::ImportedScene scene;
    glm::mat4 root(1.0f);
    return TreeFor(modelRef, scene, root);
}

int Place(const Project& project, Scene& scene, const std::string& modelRef, const std::string& rootName,
          ModelMaterialImportResult& report) {
    sage::assets::ImportedScene tree;
    glm::mat4 rootMatrix(1.0f);
    if (!TreeFor(modelRef, tree, rootMatrix)) return -1;

    const size_t count = tree.Groups.size();
    // Узлы без геометрии во всём своём поддереве (пустые маркеры экспортёра,
    // камеры, лампы) в сцену не идут: объект, которому нечего рисовать и
    // нечего держать, — только мусор в списке.
    std::vector<bool> useful(count, false);
    for (const sage::assets::ImportedNode& n : tree.Nodes)
        for (int g = n.Group; g >= 0 && g < (int)count && !useful[(size_t)g]; g = tree.Groups[(size_t)g].Parent)
            useful[(size_t)g] = true;
    std::vector<bool> hasMesh(count, false);
    for (const sage::assets::ImportedNode& n : tree.Nodes) hasMesh[(size_t)n.Group] = true;

    GameObject root = scene.CreateEmptyObject(rootName);
    root.GetTransform().SetFromMatrix(rootMatrix);

    // Родитель всегда раньше ребёнка (импортёры кладут узлы обходом сверху
    // вниз), но опираться на это нельзя: FBX перечисляет модели как угодно.
    // Поэтому объекты заводятся все сразу, а связи ставятся вторым проходом.
    std::vector<entt::entity> entityOf(count, entt::entity{entt::null});
    for (size_t g = 0; g < count; ++g) {
        if (!useful[g]) continue;
        const sage::assets::ImportedGroup& group = tree.Groups[g];
        GameObject obj = hasMesh[g] ? scene.CreateObject(group.Name) : scene.CreateEmptyObject(group.Name);
        obj.GetTransform().SetFromMatrix(tree.GroupLocal((int)g));
        entityOf[g] = obj.Entity();
        if (hasMesh[g]) {
            Add(report, SetEntityMesh(project, scene.Registry(), obj.Entity(), MeshRef::Type::Model, modelRef,
                                      ResourceManager::Instance().GetModel(modelRef, (int)g), (int)g));
        }
    }
    for (size_t g = 0; g < count; ++g) {
        if (entityOf[g] == entt::null) continue;
        const int parent = tree.Groups[g].Parent;
        const entt::entity p =
            parent >= 0 && parent < (int)count && entityOf[(size_t)parent] != entt::null ? entityOf[(size_t)parent]
                                                                                          : root.Entity();
        scene.SetParent(entityOf[g], p);
    }
    LOG_INFO("Editor") << "Модель поставлена иерархией: " << modelRef << " (узлов " << count << ")";
    return root.Id();
}

} // namespace sage::editor::modelhierarchy
