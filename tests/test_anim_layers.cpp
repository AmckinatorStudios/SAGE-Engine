// Слои и маски анимации: слой смешивает ТОЛЬКО свои каналы и только на костях
// маски (кость вместе с потомками), вес слоя — свой или из скрипта
// (Params.Layers), слои переживают сохранение сцены; свой скелетный клип
// помечен editable, вынутый из модели — нет.
#include "TestFramework.h"

#include <cstdio>
#include <memory>
#include <string>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>
#include <glm/gtc/quaternion.hpp>

#include "sage/anim/AnimationComponents.h"
#include "sage/anim/Animator.h"
#include "sage/anim/ClipFile.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "sage/scene/SceneSerializer.h"

using namespace sage::anim;

namespace {

// Hips ─┬─ Spine ── Arm ── Hand
//       └─ Leg
Skeleton Body() {
    Skeleton s;
    auto add = [&s](const char* name, int parent, glm::vec3 t) {
        Joint j;
        j.Name = name;
        j.Parent = parent;
        j.Translation = t;
        s.Joints.push_back(j);
    };
    add("Hips", -1, {0, 1, 0});
    add("Spine", 0, {0, 0.5f, 0});
    add("Arm", 1, {0.3f, 0, 0});
    add("Hand", 2, {0.3f, 0, 0});
    add("Leg", 0, {0, -0.5f, 0});
    return s;
}

// Клип слоя: двигает Arm и Leg по X на +1 (от их позы покоя), Hand не трогает.
AnimationClip Wave(const Skeleton& sk) {
    ClipAsset a;
    a.Name = "Wave";
    a.Duration = 1.0f;
    for (const char* bone : {"Arm", "Leg"}) {
        ClipAsset::Channel ch;
        ch.Joint = bone;
        ch.Target = AnimPath::Translation;
        ch.Times = {0.0f, 1.0f};
        const float x = std::string(bone) == "Arm" ? 1.3f : 1.0f;
        ch.Values = {glm::vec4(x, 0, 0, 0), glm::vec4(x, 0, 0, 0)};
        a.Channels.push_back(ch);
    }
    return Bind(a, sk);
}

} // namespace

TEST(AnimLayers_mask_covers_bone_and_descendants_only) {
    const Skeleton sk = Body();
    int missing = 0;
    const std::vector<float> w = MaskWeights(sk, {"Spine", "NoSuchBone"}, &missing);
    CHECK_EQ(missing, 1);
    CHECK_TRUE(w.size() == 5);
    CHECK_NEAR(w[0], 0.0f, 1e-6);   // Hips — предок, не в маске
    CHECK_NEAR(w[1], 1.0f, 1e-6);   // Spine
    CHECK_NEAR(w[2], 1.0f, 1e-6);   // Arm — потомок
    CHECK_NEAR(w[3], 1.0f, 1e-6);   // Hand — потомок
    CHECK_NEAR(w[4], 0.0f, 1e-6);   // Leg — соседняя ветка
}

TEST(AnimLayers_layer_blends_only_its_channels_on_masked_bones) {
    const Skeleton sk = Body();
    const AnimationClip wave = Wave(sk);
    const std::vector<float> mask = MaskWeights(sk, {"Arm"});

    LocalPose pose;
    SampleClipPose(sk, nullptr, 0.0f, pose);   // поза покоя
    AnimLayerPose layer;
    layer.Clip = &wave;
    layer.Weight = 0.5f;
    layer.JointWeights = &mask;
    ApplyLayer(layer, pose);
    CHECK_NEAR(pose.T[2].x, 0.3f + (1.3f - 0.3f) * 0.5f, 1e-5);   // Arm: половина пути
    CHECK_NEAR(pose.T[4].x, 0.0f, 1e-6);                          // Leg: вне маски
    CHECK_NEAR(pose.T[3].x, 0.3f, 1e-6);                          // Hand: канала нет — как было

    // Без маски — весь скелет, но по-прежнему только свои каналы.
    SampleClipPose(sk, nullptr, 0.0f, pose);
    layer.JointWeights = nullptr;
    layer.Weight = 1.0f;
    ApplyLayer(layer, pose);
    CHECK_NEAR(pose.T[2].x, 1.3f, 1e-5);
    CHECK_NEAR(pose.T[4].x, 1.0f, 1e-5);
    CHECK_NEAR(pose.T[3].x, 0.3f, 1e-6);

    // Вес 0 — слой ничего не делает.
    SampleClipPose(sk, nullptr, 0.0f, pose);
    layer.Weight = 0.0f;
    ApplyLayer(layer, pose);
    CHECK_NEAR(pose.T[2].x, 0.3f, 1e-6);
}

TEST(AnimLayers_animator_applies_layers_over_the_base_pose) {
    const Skeleton sk = Body();
    std::vector<AnimationClip> clips;   // основного клипа нет — поза покоя
    Animator a;
    a.SetRig(&sk, &clips);
    const AnimationClip wave = Wave(sk);
    const std::vector<float> mask = MaskWeights(sk, {"Arm"});
    std::vector<AnimLayerPose> layers(1);
    layers[0].Clip = &wave;
    layers[0].Weight = 1.0f;
    layers[0].JointWeights = &mask;
    a.SetLayers(&layers);
    a.Update(0.0f);
    // Глобальная позиция Arm = Hips(0,1,0) + Spine(0,.5,0) + Arm(1.3,0,0).
    const glm::vec3 arm = glm::vec3(a.GlobalMatrices()[2][3]);
    CHECK_NEAR(arm.x, 1.3f, 1e-5);
    CHECK_NEAR(arm.y, 1.5f, 1e-5);
    // Leg вне маски — на месте.
    CHECK_NEAR(glm::vec3(a.GlobalMatrices()[4][3]).x, 0.0f, 1e-6);
    // Ручное переопределение выигрывает у слоя.
    std::vector<JointPose> overrides(5);
    overrides[2].HasTranslation = true;
    overrides[2].Translation = glm::vec3(-2, 0, 0);
    a.SetPoseOverride(&overrides);
    a.Update(0.0f);
    CHECK_NEAR(glm::vec3(a.GlobalMatrices()[2][3]).x, -2.0f, 1e-5);
}

TEST(AnimLayers_weight_from_script_overrides_the_layer_weight) {
    AnimationComponent am;
    AnimationLayer l;
    l.Name = "Wave";
    l.Weight = 0.8f;
    am.Layers.push_back(l);
    CHECK_NEAR(am.LayerWeight(0), 0.8f, 1e-6);
    am.Params.Layers["Wave"] = 0.25f;   // animator:SetLayerWeight("Wave", 0.25)
    CHECK_NEAR(am.LayerWeight(0), 0.25f, 1e-6);
    CHECK_NEAR(am.LayerWeight(7), 0.0f, 1e-6);
}

TEST(AnimLayers_layers_survive_scene_save_and_old_scenes_have_none) {
    Scene scene("layers");
    GameObject o = scene.CreateObject("Hero");
    AnimationComponent am;
    am.ClipPath = "anims/walk.sageanim";
    AnimationLayer l;
    l.Name = "Wave";
    l.ClipPath = "anims/wave.sageanim";
    l.Weight = 0.6f;
    l.Mask = {"Spine", "Arm"};
    am.Layers.push_back(l);
    scene.Registry().emplace<AnimationComponent>(o.Entity(), am);
    GameObject plain = scene.CreateObject("Plain");
    scene.Registry().emplace<AnimationComponent>(plain.Entity());

    const std::string text = SceneSerializer::SaveToString(scene);
    std::unique_ptr<Scene> loaded = SceneSerializer::LoadFromString(text);
    GameObject h = loaded->FindByName("Hero");
    const AnimationComponent* got = h.Valid() ? loaded->Registry().try_get<AnimationComponent>(h.Entity()) : nullptr;
    CHECK_TRUE(got && got->Layers.size() == 1);
    if (got && got->Layers.size() == 1) {
        CHECK_EQ(got->Layers[0].Name, std::string("Wave"));
        CHECK_EQ(got->Layers[0].ClipPath, std::string("anims/wave.sageanim"));
        CHECK_NEAR(got->Layers[0].Weight, 0.6f, 1e-6);
        CHECK_TRUE(got->Layers[0].Mask == std::vector<std::string>({"Spine", "Arm"}));
    }
    GameObject p = loaded->FindByName("Plain");
    const AnimationComponent* none = p.Valid() ? loaded->Registry().try_get<AnimationComponent>(p.Entity()) : nullptr;
    CHECK_TRUE(none && none->Layers.empty());
    // Объект без слоёв пишется без ключа: старые сцены не меняются.
    const nlohmann::json j = nlohmann::json::parse(text);
    bool plainHasLayers = true;
    for (const auto& obj : j["objects"])
        if (obj.value("name", "") == "Plain") plainHasLayers = obj["animation"].contains("layers");
    CHECK_TRUE(!plainHasLayers);
}

TEST(AnimLayers_own_skeletal_clip_is_editable_and_model_clip_is_not) {
    const std::string own = "sage_test_own.sageanim";
    const std::string model = "sage_test_model.sageanim";
    ClipAsset a;
    a.Name = "Own";
    a.Duration = 1.0f;
    a.Editable = true;
    SaveClip(a, own);
    ClipAsset b = a;
    b.Editable = false;
    SaveClip(b, model);
    CHECK_TRUE(LoadClip(own).Editable);
    CHECK_TRUE(!LoadClip(model).Editable);
    std::remove(own.c_str());
    std::remove(model.c_str());
}
