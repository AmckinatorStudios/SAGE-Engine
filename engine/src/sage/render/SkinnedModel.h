#pragma once
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "sage/anim/Skeleton.h"
#include "sage/render/Texture.h"
#include "sage/rhi/Resources.h"

struct LightingEnvironment; // sage/scene/Light.h — вперёд, чтобы не тянуть весь заголовок

// ---------------------------------------------------------------------------
// SkinnedModel — анимируемая скелетная модель: геометрия с весами костей +
// скелет + набор анимационных клипов. Разделяемый неизменяемый ассет (skeleton
// и клипы одни на всех; покадровое состояние держит Animator у каждой сущности).
//
//   auto model = SkinnedModel::Load("assets/models/hero.glb");
//   Animator anim; anim.SetRig(&model->GetSkeleton(), &model->Clips());
//   anim.Play("Walk");
//   ... каждый кадр: anim.Update(dt); model->Draw(m, v, p, env, anim.BoneMatrices());
//
// Рендерит СВОИМ встроенным скиннинг-шейдером (полусферический ambient + солнце),
// не завязываясь на большой lit-конвейер редактора/рантайма — так анимированную
// модель может нарисовать кто угодно, у кого есть LightingEnvironment.
// ---------------------------------------------------------------------------
namespace sage::render {

// Вершина скина: геометрия + до 4 влияющих костей (индексы) и их веса.
struct SkinnedVertex {
    glm::vec3 Position{0.0f};
    glm::vec3 Normal{0.0f, 1.0f, 0.0f};
    glm::vec2 TexCoords{0.0f};
    glm::vec4 Joints{0.0f};  // индексы костей (как float — читаются в шейдере через int())
    glm::vec4 Weights{0.0f}; // веса (нормируются при загрузке; сумма ~1)
};

class SkinnedMesh {
public:
    SkinnedMesh(const std::vector<SkinnedVertex>& vertices, const std::vector<unsigned int>& indices);
    SkinnedMesh(const SkinnedMesh&) = delete;
    SkinnedMesh& operator=(const SkinnedMesh&) = delete;
    void Draw() const;

private:
    std::unique_ptr<sage::rhi::Geometry> m_geometry;
    size_t m_indexCount = 0;
};

struct SkinnedSubMesh {
    std::shared_ptr<SkinnedMesh> Mesh;
    std::shared_ptr<Texture> Diffuse; // может быть nullptr — тогда только Tint
    glm::vec3 Tint{1.0f};
    float Metallic = 0.0f;   // PBR (metallic-roughness) — из glTF-материала или дефолт
    float Roughness = 0.6f;
};

class SkinnedModel {
public:
    // Загружает скелетную модель из .gltf/.glb или .fbx (skins + animations).
    // Бросает при ошибке или если в файле нет скина. Меши без скина игнорируются.
    static std::unique_ptr<SkinnedModel> Load(const std::string& path);

    // Процедурная демонстрация: гибкий сегментированный «щупалец» из нескольких
    // костей со встроенным клипом «Wave» (волна изгиба). Без внешних ассетов —
    // используется для примеров и headless-тестов пайплайна анимации.
    static std::unique_ptr<SkinnedModel> CreateDemoTentacle(int segments = 6);

    const sage::anim::Skeleton& GetSkeleton() const { return m_skeleton; }
    const std::vector<sage::anim::AnimationClip>& Clips() const { return m_clips; }
    int SubMeshCount() const { return (int)m_subMeshes.size(); }

    // Рисует все submesh со скиннингом по палитре костей bones (из Animator),
    // с ПОЛНЫМ освещением сцены (ambient/солнце+тени/точечные/прожекторы/туман) —
    // тем же, что и статические меши. lightMatrix/shadowMap/shadowsEnabled —
    // карта теней от солнца (как в статическом lit-проходе). Если bones пуст —
    // bind-поза (единичные кости).
    void Draw(const glm::mat4& model, const glm::mat4& view, const glm::mat4& proj,
              const glm::vec3& viewPos, const LightingEnvironment& env,
              const std::vector<glm::mat4>& bones,
              const glm::mat4& lightMatrix, unsigned int shadowMap, bool shadowsEnabled) const;

    // Рисует геометрию ТОЛЬКО в глубину для карты теней, со скиннингом в текущей
    // позе — чтобы анимированная модель ОТБРАСЫВАЛА тень. Вызывается внутри
    // depth-прохода солнца (после static-геометрии, до EndRender). lightMatrix —
    // uLightSpace прохода. bones пуст — bind-поза.
    void DrawDepth(const glm::mat4& model, const glm::mat4& lightMatrix,
                   const std::vector<glm::mat4>& bones) const;

private:
    static std::unique_ptr<SkinnedModel> LoadGltf(const std::string& path, bool binary);
    static std::unique_ptr<SkinnedModel> LoadFbx(const std::string& path);

    std::vector<SkinnedSubMesh> m_subMeshes;
    sage::anim::Skeleton m_skeleton;
    std::vector<sage::anim::AnimationClip> m_clips;
};

} // namespace sage::render
