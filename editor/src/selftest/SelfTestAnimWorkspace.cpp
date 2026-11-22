#include "SelfTestCommon.h"

#include <glm/gtc/matrix_transform.hpp>

#include "ModelMaterialImport.h"
#include "../animws/AnimClipNames.h"
#include "sage/render/SkinnedModel.h"
#include "sage/anim/AnimationSystem.h"
#include "sage/anim/AnimGraph.h"
#include "sage/anim/ClipFile.h"
#include "sage/anim/PropertyClip.h"
#include "sage/anim/PropertyAnimator.h"

// ПРОСТРАНСТВО ANIMATION на настоящей модели со скелетом: сцена предпросмотра
// — копия цели, правка позы гизмо становится ключом своего клипа, клип
// сохраняется и играет, параметры графа и поза живут только в предпросмотре,
// слои с маской работают на копии — а объект уровня остаётся каким был.
// Посадка на пол, кадр, камера, слежение и цепочка кости на настоящей модели
// (см. вызов: после сборки копии). Без мыши: гизмо и щелчок по виду проверяются
// вручную — здесь то, что стоит за ними и считается без экрана.
void EditorLayer::AnimWorkspaceChecks(GameObject hero, const std::function<void(const std::string&)>& fail) {
    AnimStage& st = m_animStage;
    const sage::anim::Skeleton* sk = st.Skeleton();
    auto maxDiff = [](const glm::mat4& a, const glm::mat4& b) {
        float d = 0.0f, mag = 1.0f;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) {
                d = std::max(d, std::fabs(a[i][j] - b[i][j]));
                mag = std::max(mag, std::fabs(b[i][j]));
            }
        return d / mag;
    };
    // Нижняя точка геометрии ИНАЧЕ, чем в AnimStage::GeometryBounds: углы
    // габарита каждого подмеша модели через настоящую матрицу «сущность *
    // ImportTransform».
    auto lowest = [&]() {
        float low = 1e30f;
        Scene& sc = st.StageScene();
        for (entt::entity e : sc.Registry().view<AnimationComponent>()) {
            const AnimationComponent& a = sc.Registry().get<AnimationComponent>(e);
            if (!a.Model) continue;
            const glm::mat4 m = sc.WorldMatrix(e) * a.Model->ImportTransform();
            for (const auto& sub : a.Model->SubMeshes()) {
                if (sub.BoundsVertices == 0) continue;
                for (int k = 0; k < 8; ++k) {
                    const glm::vec3 corner((k & 1) ? sub.BoundsHi.x : sub.BoundsLo.x, (k & 2) ? sub.BoundsHi.y : sub.BoundsLo.y,
                                           (k & 4) ? sub.BoundsHi.z : sub.BoundsLo.z);
                    low = std::min(low, (m * glm::vec4(corner, 1.0f)).y);
                }
            }
        }
        return low;
    };
    int rootBone = 0;
    for (int i = 0; i < sk->Count(); ++i)
        if (sk->Joints[(size_t)i].Parent < 0) { rootBone = i; break; }

    // --- Посадка на пол -------------------------------------------------------
    if (!st.FloorAligned()) {
        fail("модель не посажена на пол: границы геометрии не найдены");
    } else {
        if (std::fabs(lowest()) > 2e-3f) fail("нижняя точка геометрии не лежит на плоскости пола");
        if (std::fabs(st.StageRoot().GetTransform().Position.y - st.FloorLift()) > 1e-6f)
            fail("смещение копии не равно рассчитанному");
        // Смещение живёт, пока не сменилась цель: проигрывание не пересчитывает его.
        const float lift = st.FloorLift();
        st.SetPlaying(true);
        for (int i = 0; i < 30; ++i) st.Tick(*this, 0.05f);
        st.SetPlaying(false);
        if (std::fabs(st.FloorLift() - lift) > 1e-6f || std::fabs(st.StageRoot().GetTransform().Position.y - lift) > 1e-6f)
            fail("проигрывание пересчитало посадку на пол");
        // Прыжок: корень поднят на метр — копия не возвращается на пол.
        st.Tick(*this, 0.0f);
        const glm::mat4 before = st.BoneWorld(rootBone);
        glm::mat4 up = before;
        up[3].y += 1.0f;
        st.SetBoneWorld(rootBone, up, true, false, false);
        st.Tick(*this, 0.016f);
        if (std::fabs(st.BoneWorld(rootBone)[3].y - before[3].y - 1.0f) > 2e-3f ||
            std::fabs(st.StageRoot().GetTransform().Position.y - lift) > 1e-6f)
            fail("вертикальное движение клипа не сохраняется (модель возвращается на пол)");
        st.ClearPose();
    }
    // Другие повороты и масштабы цели и другие настройки импорта: нижняя точка
    // снова на полу.
    {
        const Transform saved = hero.GetTransform();
        const struct { glm::vec3 rot; float scale; } variants[] = {
            {{90, 0, 0}, 2.0f}, {{0, 137, 0}, 0.01f}, {{-90, 0, 45}, 5.0f}, {{180, 0, 0}, 1.0f}};
        for (const auto& v : variants) {
            glm::mat4 m = glm::translate(glm::mat4(1.0f), saved.Position);
            m = glm::rotate(m, glm::radians(v.rot.x), glm::vec3(1, 0, 0));
            m = glm::rotate(m, glm::radians(v.rot.y), glm::vec3(0, 1, 0));
            m = glm::rotate(m, glm::radians(v.rot.z), glm::vec3(0, 0, 1));
            m = glm::scale(m, glm::vec3(v.scale));
            hero.GetTransform().SetFromMatrix(m);
            st.SetTarget(*this, -1);
            st.SetTarget(*this, hero.Id());
            st.Tick(*this, 0.0f);
            if (!st.FloorAligned() || std::fabs(lowest()) > 2e-3f * std::max(1.0f, v.scale))
                fail("посадка на пол не сошлась при повороте/масштабе цели " + std::to_string(v.scale));
        }
        hero.GetTransform() = saved;
        st.SetTarget(*this, -1);
        st.SetTarget(*this, hero.Id());
        st.Tick(*this, 0.0f);
        AnimationComponent* am = st.StageAnim();
        if (am && am->Model) {
            const glm::mat4 original = am->Model->ImportTransform();
            am->Model->SetImportTransform(glm::rotate(glm::scale(glm::mat4(1.0f), glm::vec3(3.0f)), glm::radians(90.0f), glm::vec3(1, 0, 0)));
            st.Tick(*this, 0.0f);   // смена настроек импорта — посадка заново
            if (!st.FloorAligned() || std::fabs(lowest()) > 2e-3f) fail("посадка не пересчиталась после смены настроек импорта");
            am->Model->SetImportTransform(original);
            st.Tick(*this, 0.0f);
            if (!st.FloorAligned() || std::fabs(lowest()) > 2e-3f) fail("посадка не вернулась после возврата настроек импорта");
        }
    }

    // --- Кадр: модель целиком в кадре и заметна --------------------------------
    {
        glm::vec3 lo, hi;
        if (!st.GeometryBounds(lo, hi, true)) {
            fail("границы геометрии для кадра недоступны");
        } else {
            const glm::mat4 vp = st.Proj(1.6f) * st.View();
            glm::vec2 nlo(1e9f), nhi(-1e9f);
            for (int k = 0; k < 8; ++k) {
                const glm::vec4 c = vp * glm::vec4((k & 1) ? hi.x : lo.x, (k & 2) ? hi.y : lo.y, (k & 4) ? hi.z : lo.z, 1.0f);
                if (c.w <= 0.0f) { fail("кадр: часть модели за камерой"); break; }
                nlo = glm::min(nlo, glm::vec2(c.x, c.y) / c.w);
                nhi = glm::max(nhi, glm::vec2(c.x, c.y) / c.w);
            }
            if (nlo.x < -1.0f || nlo.y < -1.0f || nhi.x > 1.0f || nhi.y > 1.0f) fail("кадр: модель не помещается");
            if (std::max(nhi.x - nlo.x, nhi.y - nlo.y) < 1.0f) fail("кадр: модель занимает меньше четверти кадра");
        }
    }

    // --- Камера -----------------------------------------------------------------
    {
        const glm::vec3 focus0 = st.Focus();
        st.Orbit(25.0f, -10.0f);
        if (glm::length(st.Focus() - focus0) > 1e-5f) fail("орбита сдвинула фокус");
        const glm::vec3 eye0 = st.Eye();
        const float dist0 = st.Distance();
        st.Look(60.0f, -20.0f);
        if (glm::length(st.Eye() - eye0) > 1e-3f) fail("осмотр на месте сдвинул глаз");
        if (std::fabs(st.Distance() - dist0) > 1e-5f) fail("осмотр на месте изменил расстояние");
        const glm::vec3 eyeA = st.Eye(), focusA = st.Focus();
        st.Fly(1.0f, 0.5f, 1.0f, 0.1f, false);
        const glm::vec3 dEye = st.Eye() - eyeA, dFocus = st.Focus() - focusA;
        if (glm::length(dEye) < 1e-4f || glm::length(dEye - dFocus) > 1e-4f) fail("полёт: глаз и фокус едут не вместе");
        if (std::fabs(st.Distance() - dist0) > 1e-5f) fail("полёт изменил расстояние");
        const glm::vec3 eyeB = st.Eye();
        st.Fly(1.0f, 0.0f, 0.0f, 0.1f, true);
        const float fast = glm::length(st.Eye() - eyeB);
        const glm::vec3 eyeC = st.Eye();
        st.Fly(1.0f, 0.0f, 0.0f, 0.1f, false);
        const float slow = glm::length(st.Eye() - eyeC);
        if (fast < slow * 2.5f) fail("Shift не ускоряет полёт");
        st.ResetCamera();
    }

    // --- Follow Target ------------------------------------------------------------
    {
        st.SetPlaying(false);
        st.SetFollow(true);
        st.Tick(*this, 0.016f);   // точка слежения берётся
        const glm::vec3 track0 = st.TrackedPoint(), focus0 = st.Focus();
        const float dist0 = st.Distance();
        glm::mat4 go = st.BoneWorld(rootBone);
        go[3] += glm::vec4(2.0f, 0.0f, -1.0f, 0.0f);
        st.SetBoneWorld(rootBone, go, true, false, false);
        st.Tick(*this, 0.016f);
        const glm::vec3 moved = st.TrackedPoint() - track0;
        if (glm::length(moved) < 0.5f) fail("Follow: модель не сдвинулась в проверке");
        if (glm::length((st.Focus() - focus0) - moved) > 1e-3f) fail("Follow: перемотка не перенесла фокус за моделью");
        if (std::fabs(st.Distance() - dist0) > 1e-5f) fail("Follow: расстояние камеры изменилось");
        // Проигрывание: фокус сходится к тому же смещению плавно, без рывка.
        st.SetFollow(true);
        st.Tick(*this, 0.016f);
        const glm::vec3 track1 = st.TrackedPoint(), focus1 = st.Focus();
        go = st.BoneWorld(rootBone);
        go[3] += glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
        st.SetBoneWorld(rootBone, go, true, false, false);
        // Клип стоит (скорость 0), но проигрывание включено: движется только
        // слежение, и его сглаживание видно без помех от самого клипа.
        const float speed0 = st.Speed();
        st.SetSpeed(0.0f);
        st.SetPlaying(true);
        st.Tick(*this, 0.016f);
        const float firstStep = glm::length(st.Focus() - focus1);
        for (int i = 0; i < 120; ++i) st.Tick(*this, 0.016f);
        st.SetPlaying(false);
        st.SetSpeed(speed0);
        const glm::vec3 moved1 = st.TrackedPoint() - track1;
        if (firstStep >= glm::length(moved1) || firstStep < 1e-5f) fail("Follow: при проигрывании камера не сглажена");
        if (glm::length((st.Focus() - focus1) - moved1) > 2e-2f) fail("Follow: камера не догнала модель");
        // Follow выключен — камера в руках пользователя.
        st.SetFollow(false);
        st.Tick(*this, 0.016f);
        const glm::vec3 focus2 = st.Focus();
        go = st.BoneWorld(rootBone);
        go[3] += glm::vec4(0.0f, 0.0f, 3.0f, 0.0f);
        st.SetBoneWorld(rootBone, go, true, false, false);
        st.Tick(*this, 0.016f);
        if (glm::length(st.Focus() - focus2) > 1e-5f) fail("камера едет за моделью при выключенном Follow");
        st.ClearPose();
        st.Tick(*this, 0.0f);
        st.FrameTarget();
    }

    // --- Цепочка кости: выбор из иерархии -> мировая -> локальная -> поза ---------
    {
        auto depthOf = [&](int b) {
            int d = 0;
            for (int p = sk->Joints[(size_t)b].Parent; p >= 0; p = sk->Joints[(size_t)p].Parent) ++d;
            return d;
        };
        int deep = rootBone;
        for (int i = 0; i < sk->Count(); ++i)
            if (depthOf(i) > depthOf(deep) && depthOf(i) <= 3) deep = i;
        for (int i = 0; i < sk->Count(); ++i)
            if (depthOf(i) > depthOf(deep)) deep = i;
        const int parent = sk->Joints[(size_t)deep].Parent;
        auto isDescendant = [&](int b, int anc) {
            for (int p = sk->Joints[(size_t)b].Parent; p >= 0; p = sk->Joints[(size_t)p].Parent)
                if (p == anc) return true;
            return false;
        };
        st.ClearPose();
        st.Tick(*this, 0.0f);
        std::vector<glm::mat4> snap;
        for (int i = 0; i < sk->Count(); ++i) snap.push_back(st.BoneWorld(i));
        st.SelectBone(deep);   // то же, что делает щелчок в Animation Hierarchy
        if (st.SelectedBone() != deep) fail("выбор кости не дошёл до сцены предпросмотра");

        // Без скачка: «поставить кости её же мировую матрицу» ничего не меняет — как
        // первый кадр перетаскивания гизмо.
        for (int i = 0; i < sk->Count(); ++i) st.SetBoneWorld(i, snap[(size_t)i], true, true, true);
        for (int i = 0; i < sk->Count(); ++i)
            if (maxDiff(st.BoneWorld(i), snap[(size_t)i]) > 2e-3f) { fail("гизмо: скачок позы на первом кадре (кость " + std::to_string(i) + ")"); break; }
        st.ClearPose();
        st.Tick(*this, 0.0f);

        // Move (мировая): кость на месте, потомки сдвинулись так же, остальные — нет.
        glm::mat4 mv = snap[(size_t)deep];
        const glm::vec3 delta(0.1f, 0.2f, -0.1f);
        mv[3] += glm::vec4(delta, 0.0f);
        st.SetBoneWorld(deep, mv, true, false, false);
        if (maxDiff(st.BoneWorld(deep), mv) > 2e-3f) fail("Move: кость не встала в запрошенную мировую позицию");
        for (int i = 0; i < sk->Count(); ++i) {
            const glm::vec3 d = glm::vec3(st.BoneWorld(i)[3] - snap[(size_t)i][3]);
            const bool shouldMove = i == deep || isDescendant(i, deep);
            if (shouldMove && glm::length(d - delta) > 2e-3f * std::max(1.0f, glm::length(delta))) { fail("Move: потомок не поехал вместе с костью"); break; }
            if (!shouldMove && glm::length(d) > 1e-3f) { fail("Move: сдвинулась кость вне цепочки"); break; }
        }
        st.ClearPose();
        st.Tick(*this, 0.0f);

        // Rotate: в мировых осях (вокруг начала кости) и в локальных.
        for (int space = 0; space < 2; ++space) {
            const glm::mat4 R = glm::rotate(glm::mat4(1.0f), glm::radians(35.0f), glm::normalize(glm::vec3(1, 2, 3)));
            glm::mat4 rot;
            if (space == 0) {
                rot = snap[(size_t)deep];
                const glm::vec4 origin = rot[3];
                rot = R * rot;
                rot[3] = origin;
            } else {
                rot = snap[(size_t)deep] * R;
            }
            st.SetBoneWorld(deep, rot, false, true, false);
            if (maxDiff(st.BoneWorld(deep), rot) > 3e-3f) fail(space == 0 ? "Rotate (World): поворот не совпал с запрошенным"
                                                                          : "Rotate (Local): поворот не совпал с запрошенным");
            if (parent >= 0 && maxDiff(st.BoneWorld(parent), snap[(size_t)parent]) > 1e-3f) fail("Rotate: повернулся родитель");
            if (glm::length(glm::vec3(st.BoneWorld(deep)[3] - snap[(size_t)deep][3])) > 1e-3f) fail("Rotate: кость уехала с места");
            st.ClearPose();
            st.Tick(*this, 0.0f);
        }

        // Scale: свои оси кости.
        {
            const glm::mat4 sc = snap[(size_t)deep] * glm::scale(glm::mat4(1.0f), glm::vec3(1.5f, 1.0f, 0.7f));
            st.SetBoneWorld(deep, sc, false, false, true);
            if (maxDiff(st.BoneWorld(deep), sc) > 3e-3f) fail("Scale: масштаб не совпал с запрошенным");
            const AnimationComponent* am = st.StageAnim();
            if (!am || deep >= (int)am->PoseOverrides.size() || !am->PoseOverrides[(size_t)deep].HasScale ||
                am->PoseOverrides[(size_t)deep].HasRotation || am->PoseOverrides[(size_t)deep].HasTranslation)
                fail("PoseOverrides: масштаб записан не только в канал масштаба");
            st.ClearPose();
            st.Tick(*this, 0.0f);
        }
        if (st.HasPoseEdits()) fail("ClearPose оставил ручную позу");
    }

    // --- Имена клипов: короткие, различимые, настоящие не тронуты -------------------
    {
        const std::vector<std::string> names = {"SpringBonnie|SpringBonnie|SpringBonnie|Idle",
                                                "SpringBonnie|SpringBonnie|SpringBonnie|Walk", "Idle", "Jump",
                                                "SpringBonnie|SpringBonnie|SpringBonnie|Idle"};
        std::string prefix;
        const std::vector<std::string> shown = animnames::DisplayNames(names, &prefix);
        if (prefix != "SpringBonnie") fail("имена клипов: общий префикс не вынесен");
        if (shown.size() != names.size() || shown[1] != "Walk" || shown[2] != "Idle (#3)" || shown[3] != "Jump")
            fail("имена клипов: короткие имена не те");
        if (shown[0] == shown[2] || shown[0] == shown[4]) fail("имена клипов: одинаковые имена не различимы");
        if (names[0] != "SpringBonnie|SpringBonnie|SpringBonnie|Idle") fail("имена клипов: исходное имя изменено");
        const std::vector<std::string> plain = animnames::DisplayNames({"Idle", "Walk"}, &prefix);
        if (plain[0] != "Idle" || plain[1] != "Walk" || !prefix.empty()) fail("имена клипов: простые имена исказились");
    }
}

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
    // Выбран Graph, а графа нет: клип НЕ подставляется (стоим, причина названа);
    // вернули Clip — клип снова играет. Пути при этом не тронуты.
    {
        auto& am = m_scene->Registry().get<AnimationComponent>(hero.Entity());
        am.Source = AnimationComponent::SourceKind::Graph;
        sage::anim::UpdateAnimators(*m_scene, 0.016f);
        if (am.Anim.Playing()) fail("Graph без графа: клип играет вместо графа (скрытый fallback)");
        if (am.GraphProblem.empty()) fail("Graph без графа: причина не названа");
        am.Source = AnimationComponent::SourceKind::Clip;
        sage::anim::UpdateAnimators(*m_scene, 0.016f);
        if (!am.Anim.Playing()) fail("возврат к Clip не возобновил клип");
        if (am.ClipPath != clips.Refs.front()) fail("переключение источника тронуло ClipPath");
    }

    SetWorkspace(EditorWorkspace::Animation);
    m_animStage.SetTarget(*this, hero.Id());
    m_animStage.Tick(*this, 0.016f);
    const sage::anim::Skeleton* sk = m_animStage.Skeleton();
    if (!sk || sk->Count() < 2) fail("копия цели без скелета");
    // Копия стоит над началом координат; по высоте её сажает на пол посадка.
    if (!m_animStage.StageRoot().Valid() || std::fabs(m_animStage.StageRoot().GetTransform().Position.x) > 1e-5f ||
        std::fabs(m_animStage.StageRoot().GetTransform().Position.z) > 1e-5f)
        fail("копия цели не в начале координат сцены предпросмотра");
    if (sk && sk->Count() >= 2) AnimWorkspaceChecks(hero, fail);

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
        heroAm.Source = AnimationComponent::SourceKind::Graph;
        if (!CreateAnimationClip(hero.Id(), true)) fail("Create Clip скелетного клипа не сработал");
        createdSkeletal = sage::AssetDatabase::Instance().LocatePath(heroAm.ClipPath);
        if (heroAm.ClipPath.empty() || heroAm.ClipPath == clips.Refs.front()) fail("скелетный клип не назначен");
        if (heroAm.GraphPath != "unused.sageanimgraph") fail("новый клип стёр путь графа");
        if (heroAm.Source != AnimationComponent::SourceKind::Clip) fail("новый клип не выбран источником");
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
