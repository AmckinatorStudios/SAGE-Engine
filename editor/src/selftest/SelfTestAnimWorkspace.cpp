#include "SelfTestCommon.h"

#include <glm/gtc/matrix_transform.hpp>

#include "ModelMaterialImport.h"
#include "sage/anim/AnimationSystem.h"
#include "sage/anim/AnimGraph.h"
#include "sage/anim/ClipFile.h"
#include "sage/anim/PropertyClip.h"
#include "sage/anim/PropertyAnimator.h"

// ПРОСТРАНСТВО ANIMATION на настоящей модели со скелетом: сцена предпросмотра
// — копия цели, правка позы гизмо становится ключом своего клипа, клип
// сохраняется и играет, параметры графа и поза живут только в предпросмотре,
// слои с маской работают на копии — а объект уровня остаётся каким был.
bool EditorLayer::SelfTestAnimWorkspace() {
#ifndef SAGE_TEST_MODEL
    return true;
#else
    bool ok = true;
    auto fail = [&](const std::string& why) {
        LOG_ERROR("Editor") << "SELFTEST: anim-workspace: " << why;
        ok = false;
    };
    std::error_code ec;
    const fs::path dir = m_project.AssetsDir() / "selftest_animws";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const fs::path model = dir / "hero.glb";
    fs::copy_file(SAGE_TEST_MODEL, model, fs::copy_options::overwrite_existing, ec);
    sage::AssetDatabase::Instance().ScanProject(m_project.Dir().string());
    const ClipImportResult clips = ImportModelClips(m_project, m_project.AssetRef(model));
    if (clips.Refs.empty()) {
        fail("у тестовой модели нет клипов");
        return ok;
    }

    GameObject hero = m_scene->CreateObject("AnimWS Hero");
    MeshRendererComponent& mr = hero.Renderer();
    mr.Ref.type = MeshRef::Type::Model;
    mr.Ref.path = m_project.AssetRef(model);
    hero.GetTransform().Position = glm::vec3(3.0f, 0.0f, 2.0f);
    AnimationComponent real;
    real.ClipPath = clips.Refs.front();
    m_scene->Registry().emplace<AnimationComponent>(hero.Entity(), real);
    sage::anim::UpdateAnimators(*m_scene, 0.0f);

    SetWorkspace(EditorWorkspace::Animation);
    m_animStage.SetTarget(*this, hero.Id());
    m_animStage.Tick(*this, 0.016f);
    const sage::anim::Skeleton* sk = m_animStage.Skeleton();
    if (!sk || sk->Count() < 2) fail("копия цели без скелета");
    if (!m_animStage.StageRoot().Valid() || glm::length(m_animStage.StageRoot().GetTransform().Position) > 1e-5f)
        fail("копия цели не в начале координат сцены предпросмотра");

    // 1. Свой клип; поворот кости гизмо -> ключ -> файл.
    const std::string ref = AnimStage::CreateEditableClip(*this, "AnimWS");
    const std::string path = sage::AssetDatabase::Instance().LocatePath(ref);
    if (ref.empty()) fail("свой клип не создан");
    OpenAnimationClip(path);
    if (!m_animation.HasClipOpen()) fail("свой клип не открылся в таймлайне");
    const int bone = 1;
    glm::quat keyed(1, 0, 0, 0);
    if (sk && sk->Count() > bone && !ref.empty()) {
        m_animStage.SetMode(AnimStage::Source::Clip);
        m_animStage.ShowClip(path, sage::anim::LoadClip(path), 1);
        m_animStage.SelectBone(bone);
        m_animStage.Tick(*this, 0.0f);
        const glm::mat4 world =
            m_animStage.BoneWorld(bone) * glm::rotate(glm::mat4(1.0f), glm::radians(35.0f), glm::vec3(0, 0, 1));
        m_animStage.SetBoneWorld(bone, world, false, true, false);
        glm::vec3 t, s;
        if (!m_animStage.BoneLocal(bone, t, keyed, s)) fail("поза кости не читается");
        if (!m_animStage.HasPoseEdits()) fail("правка гизмо не легла в позу копии");
        m_animation.KeySelectedBone(*this, true);
        if (m_animStage.HasPoseEdits()) fail("после ключа ручная поза кости не снята");
        if (!m_animation.SaveCurrent(*this)) fail("свой клип не сохранился");
        try {
            const sage::anim::ClipAsset a = sage::anim::LoadClip(path);
            bool found = false;
            for (const auto& ch : a.Channels)
                found = found || (ch.Joint == sk->Joints[(size_t)bone].Name && ch.Target == sage::anim::AnimPath::Rotation);
            if (!a.Editable) fail("свой клип записан без editable");
            if (!found) fail("в файле нет ключа поворота кости");
            // 2. Клип играет: в момент ключа поза кости — ключевая.
            m_animStage.ShowClip(path, a, 2);
            m_animStage.SetTime(0.0f);
            m_animStage.SetTimeDrivenExternally(true);
            m_animStage.Tick(*this, 0.016f);
            glm::vec3 pt, ps;
            glm::quat pr;
            m_animStage.BoneLocal(bone, pt, pr, ps);
            if (std::fabs(glm::dot(pr, keyed)) < 0.999f) fail("сохранённый ключ не воспроизводится на копии");
        } catch (const std::exception& e) {
            fail(std::string("свой клип не читается: ") + e.what());
        }
        m_animStage.SetTimeDrivenExternally(false);
    }

    // 3. Граф с параметром — только в предпросмотре.
    {
        sage::anim::AnimGraph g = sage::anim::NewAnimGraph();
        g.Params.push_back({"Speed", 0.0f, 0.0f, 1.0f});
        m_animStage.SetGraph(std::make_shared<const sage::anim::AnimGraph>(g), std::string());
        m_animStage.SetMode(AnimStage::Source::Graph);
        m_animStage.SetParamValue("Speed", 0.7f);
        m_animStage.Tick(*this, 0.016f);
        const AnimationComponent* copy = m_animStage.StageAnim();
        if (!copy || !copy->GraphAsset) fail("граф не играет на копии");
        else if (std::fabs(copy->GetFloat("Speed") - 0.7f) > 1e-5f) fail("параметр графа не дошёл до копии");
        m_animStage.SetMode(AnimStage::Source::Clip);
    }

    // 4. Слой с маской — на копии, из описания объекта.
    if (sk && sk->Count() > bone) {
        auto& realAm = m_scene->Registry().get<AnimationComponent>(hero.Entity());
        AnimationLayer layer;
        layer.Name = "Wave";
        layer.ClipPath = ref;
        layer.Mask = {sk->Joints[(size_t)bone].Name, "NoSuchBone"};
        realAm.Layers.push_back(layer);
        m_animStage.Tick(*this, 0.016f);
        const AnimationComponent* copy = m_animStage.StageAnim();
        if (!copy || copy->LayerStates.size() != 1 || !copy->LayerStates[0].HasClip) fail("слой не собран на копии");
        else {
            const AnimationLayerState& st = copy->LayerStates[0];
            if (st.MissingMaskBones != 1) fail("маска не заметила несуществующую кость");
            if (st.JointWeights.size() != (size_t)sk->Count() || st.JointWeights[(size_t)bone] != 1.0f)
                fail("маска не покрыла выбранную кость");
            for (int j = 0; j < sk->Count(); ++j)
                if (sk->Joints[(size_t)j].Parent < 0 && j != bone && st.JointWeights[(size_t)j] != 0.0f)
                    fail("маска захватила корень скелета");
            if (copy->LayerPoses.size() != 1) fail("слой не отдан проигрывателю");
        }
    }

    // 4.5 Клип свойств (.sageclip) на обычном объекте с ребёнком: в этом
    // пространстве он двигает КОПИЮ владельца — и родителя, и ребёнка.
    {
        GameObject crate = m_scene->CreateObject("AnimWS Crate");
        GameObject lid = m_scene->CreateObject("Lid");
        m_scene->SetParent(lid.Entity(), crate.Entity());
        crate.GetTransform().Position = glm::vec3(4.0f, 0.0f, 0.0f);
        sage::anim::PropertyClip pc;
        pc.Name = "Crate";
        pc.Duration = 1.0f;
        sage::anim::Track up;
        up.Property = "object.position";
        sage::anim::SetKey(up, 0.0f, glm::vec4(0, 0, 0, 0));
        sage::anim::SetKey(up, 1.0f, glm::vec4(0, 2, 0, 0));
        sage::anim::Track open;
        open.Target = "Lid";
        open.Property = "object.position";
        sage::anim::SetKey(open, 0.0f, glm::vec4(0, 0, 0, 0));
        sage::anim::SetKey(open, 1.0f, glm::vec4(1, 0, 0, 0));
        pc.Tracks = {up, open};
        const fs::path clipFile = dir / "crate.sageclip";
        std::string err;
        if (!sage::anim::SaveClipFile(pc, clipFile.string(), err)) fail("клип свойств не записан: " + err);
        PropertyAnimatorComponent pa;
        pa.ClipPath = clipFile.string();
        pa.Playing = false;
        m_scene->Registry().emplace<PropertyAnimatorComponent>(crate.Entity(), pa);
        OpenAnimationClip(clipFile.string());
        m_animation.SeekTo(*this, 0.5f);
        GameObject copy = m_animStage.StageRoot();
        if (m_animStage.TargetId() != crate.Id() || !copy.Valid()) fail("клип свойств не сделал владельца целью");
        else {
            if (std::fabs(copy.GetTransform().Position.y - 1.0f) > 1e-3f) fail("клип свойств не сдвинул копию владельца");
            const auto kids = m_animStage.StageScene().ChildrenOf(copy.Entity());
            if (kids.empty() || std::fabs(m_animStage.StageScene().Registry().get<Transform>(kids.front()).Position.x - 0.5f) > 1e-3f)
                fail("клип свойств не сдвинул ребёнка копии");
        }
        if (glm::length(crate.GetTransform().Position - glm::vec3(4, 0, 0)) > 1e-5f ||
            glm::length(lid.GetTransform().Position) > 1e-5f)
            fail("клип свойств в Animation сдвинул объекты уровня");
        m_animStage.SetTarget(*this, -1);
        m_scene->RemoveObject(lid.Id());
        m_scene->RemoveObject(crate.Id());
    }

    // 5. Уровень не тронут: позиция, поза, граф и параметры объекта.
    {
        const auto& realAm = m_scene->Registry().get<AnimationComponent>(hero.Entity());
        if (glm::length(hero.GetTransform().Position - glm::vec3(3, 0, 2)) > 1e-5f) fail("предпросмотр сдвинул объект уровня");
        if (!realAm.PoseOverrides.empty()) fail("правка позы попала в объект уровня");
        if (realAm.GraphAsset || !realAm.Params.Floats.empty()) fail("граф предпросмотра попал в объект уровня");
        if (realAm.ClipPath != clips.Refs.front()) fail("клип объекта уровня подменён");
    }

    // 6. Кнопки компонентов в Inspector: «Create Clip» заводит файл, назначает
    // его объекту и открывает в Animation; «Edit» открывает назначенное.
    std::string createdSkeletal;
    {
        GameObject box = m_scene->CreateObject("AnimWS Box");
        SetWorkspace(EditorWorkspace::Scene);
        if (!CreateAnimationClip(box.Id(), false)) fail("Create Clip клипа свойств не сработал");
        const auto* pa = m_scene->Registry().try_get<PropertyAnimatorComponent>(box.Entity());
        const std::string boxClip = pa ? sage::AssetDatabase::Instance().LocatePath(pa->ClipPath) : std::string();
        if (!pa || pa->ClipPath.empty() || !fs::exists(sage::PathFromUtf8(boxClip)))
            fail("Create Clip не назначил файл .sageclip объекту");
        if (m_workspace != EditorWorkspace::Animation) fail("Create Clip не открыл пространство Animation");
        if (!m_animation.HasClipOpen() || m_animation.ClipOwner(*this) != box.Entity())
            fail("созданный клип свойств не открыт для своего объекта");
        if (m_animStage.TargetId() != box.Id()) fail("Create Clip не сделал объект целью предпросмотра");
        m_animStage.SetTarget(*this, -1);
        m_scene->RemoveObject(box.Id());
        if (!boxClip.empty()) fs::remove(sage::PathFromUtf8(boxClip), ec);

        SetWorkspace(EditorWorkspace::Scene);
        auto& heroAm = m_scene->Registry().get<AnimationComponent>(hero.Entity());
        heroAm.GraphPath = "unused.sageanimgraph";
        if (!CreateAnimationClip(hero.Id(), true)) fail("Create Clip скелетного клипа не сработал");
        createdSkeletal = sage::AssetDatabase::Instance().LocatePath(heroAm.ClipPath);
        if (heroAm.ClipPath.empty() || heroAm.ClipPath == clips.Refs.front()) fail("скелетный клип не назначен");
        if (!heroAm.GraphPath.empty()) fail("новый клип не снял граф: источники позы должны исключать друг друга");
        try {
            if (!sage::anim::LoadClip(createdSkeletal).Editable) fail("созданный скелетный клип не editable");
        } catch (const std::exception& e) {
            fail(std::string("созданный скелетный клип не читается: ") + e.what());
        }
        if (m_workspace != EditorWorkspace::Animation || m_animStage.TargetId() != hero.Id() || !m_animation.HasClipOpen())
            fail("скелетный клип не открыт в Animation");

        // «Edit» назначенного клипа модели — тот же путь.
        SetWorkspace(EditorWorkspace::Scene);
        EditAnimation(hero.Id(), clips.Refs.front());
        if (m_workspace != EditorWorkspace::Animation || m_animStage.TargetId() != hero.Id() || !m_animation.HasClipOpen())
            fail("Edit не открыл клип объекта в Animation");
    }

    m_animStage.SetTarget(*this, -1);
    m_scene->RemoveObject(hero.Id());
    SetWorkspace(EditorWorkspace::Scene);
    if (!createdSkeletal.empty()) fs::remove(sage::PathFromUtf8(createdSkeletal), ec);
    fs::remove_all(dir, ec);
    fs::remove(path, ec);
    if (ok)
        LOG_INFO("Editor") << "ANIM_WORKSPACE: OK — копия цели, ключ кости из гизмо, сохранение и воспроизведение "
                              "клипа, граф и параметры только в предпросмотре, слой с маской, уровень не тронут, Create Clip/Edit из Inspector";
    return ok;
#endif
}
