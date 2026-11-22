// Граф анимации (sage/anim/AnimGraph.h): файл и версия, правила проводов,
// проигрывание клипов, смешивание поз, независимость экземпляров, передача
// позы в Animator и работа на настоящей модели со скелетом (tests/integration/testgame).
#include "TestFramework.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "sage/anim/AnimGraph.h"
#include "sage/anim/Animator.h"
#include "sage/anim/ClipFile.h"
#include "sage/core/Paths.h"
#include "sage/render/ModelData.h"
#include "sage/render/SkinnedModel.h"

using namespace sage::anim;

namespace {

Skeleton TwoBones(bool swapped = false) {
    Skeleton s;
    Joint root, tip;
    root.Name = "Root";
    tip.Name = "Tip";
    tip.Translation = glm::vec3(0, 1, 0);
    if (!swapped) {
        root.Parent = -1;
        tip.Parent = 0;
        s.Joints = {root, tip};
    } else {
        // Те же кости в другом порядке — «переэкспорт» или другой персонаж.
        tip.Parent = 1;
        root.Parent = -1;
        s.Joints = {tip, root};
    }
    return s;
}

// Клип файла: кость Root едет по X от a до b за 1 секунду.
ClipAsset SlideX(const std::string& name, float a, float b) {
    ClipAsset c;
    c.Name = name;
    c.Duration = 1.0f;
    ClipAsset::Channel ch;
    ch.Joint = "Root";
    ch.Target = AnimPath::Translation;
    ch.Times = {0.0f, 1.0f};
    ch.Values = {glm::vec4(a, 0, 0, 0), glm::vec4(b, 0, 0, 0)};
    c.Channels.push_back(ch);
    return c;
}

// Загрузчик клипов без диска: «файлы» — в памяти.
AnimGraphInstance::ClipLoader MemoryLoader(std::shared_ptr<std::map<std::string, ClipAsset>> files) {
    return [files](const std::string& path, ClipAsset& out, std::string& err) {
        auto it = files->find(path);
        if (it == files->end()) {
            err = "нет файла";
            return false;
        }
        out = it->second;
        return true;
    };
}

// Idle (x: 0 -> 0) и Walk (x: 0 -> 10), смешанные параметром Speed.
AnimGraph IdleWalkGraph() {
    AnimGraph g = NewAnimGraph();
    const int idle = g.Add(AnimNodeKind::Clip, {0, 0});
    const int walk = g.Add(AnimNodeKind::Clip, {0, 100});
    const int blend = g.Add(AnimNodeKind::Blend, {200, 50});
    const int speed = g.Add(AnimNodeKind::Float, {0, 200});
    g.Find(idle)->ClipPath = "idle.sageanim";
    g.Find(walk)->ClipPath = "walk.sageanim";
    g.Find(speed)->Param = "Speed";
    g.Params.push_back({"Speed", 0.0f, 0.0f, 1.0f});
    CHECK_TRUE(g.Connect(idle, blend, kBlendPinA));
    CHECK_TRUE(g.Connect(walk, blend, kBlendPinB));
    CHECK_TRUE(g.Connect(speed, blend, kBlendPinWeight));
    CHECK_TRUE(g.Connect(blend, g.OutputNode(), 0));
    return g;
}

std::shared_ptr<std::map<std::string, ClipAsset>> IdleWalkFiles() {
    auto files = std::make_shared<std::map<std::string, ClipAsset>>();
    (*files)["idle.sageanim"] = SlideX("Idle", 0.0f, 0.0f);
    (*files)["walk.sageanim"] = SlideX("Walk", 0.0f, 10.0f);
    return files;
}

} // namespace

// --- Ресурс и файл -----------------------------------------------------------

TEST(AnimGraph_new_has_single_output) {
    AnimGraph g = NewAnimGraph();
    CHECK_EQ((int)g.Nodes.size(), 1);
    CHECK_TRUE(g.OutputNode() != 0);
    CHECK_EQ(g.Add(AnimNodeKind::Output), 0);       // второй выход не создаётся
    CHECK_FALSE(g.Remove(g.OutputNode()));          // и единственный не удаляется
}

TEST(AnimGraph_survives_save_and_load) {
    AnimGraph g = IdleWalkGraph();
    g.Find(2)->Loop = false;
    g.Find(2)->Speed = 1.5f;
    g.Find(4)->Weight = 0.3f;
    g.PreviewModel = "models/totem.glb";

    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / sage::PathFromUtf8("sage_граф_анимации");
    fs::create_directories(dir);
    const std::string file = sage::PathToUtf8(dir / sage::PathFromUtf8("ходьба.sageanimgraph"));
    SaveAnimGraph(g, file);
    const AnimGraph back = LoadAnimGraph(file);
    std::error_code ec;
    fs::remove_all(dir, ec);

    CHECK_EQ(back.Nodes.size(), g.Nodes.size());
    CHECK_EQ(back.Links.size(), g.Links.size());
    CHECK_EQ(back.NextId, g.NextId);
    CHECK_EQ(back.PreviewModel, g.PreviewModel);
    CHECK_EQ((int)back.Params.size(), 1);
    CHECK_EQ(back.Params[0].Name, std::string("Speed"));
    for (const AnimGraphNode& n : g.Nodes) {
        const AnimGraphNode* m = back.Find(n.Id);
        CHECK_TRUE(m != nullptr);
        if (!m) continue;
        CHECK_TRUE(m->Kind == n.Kind);
        CHECK_NEAR(m->Pos.x, n.Pos.x, 1e-5);
        CHECK_NEAR(m->Pos.y, n.Pos.y, 1e-5);
        CHECK_EQ(m->ClipPath, n.ClipPath);
        CHECK_EQ(m->Param, n.Param);
        if (n.Kind == AnimNodeKind::Clip) {
            CHECK_EQ(m->Loop, n.Loop);
            CHECK_NEAR(m->Speed, n.Speed, 1e-5);
        }
        if (n.Kind == AnimNodeKind::Blend) CHECK_NEAR(m->Weight, n.Weight, 1e-5);
    }
    for (const AnimGraphLink& l : g.Links) {
        const AnimGraphLink* m = back.LinkInto(l.To, l.Pin);
        CHECK_TRUE(m != nullptr && m->From == l.From);
    }
}

TEST(AnimGraph_file_is_versioned) {
    const std::string text = AnimGraphToJson(NewAnimGraph());
    CHECK_TRUE(text.find("\"sage_animgraph_version\": 1") != std::string::npos);
    // Файл из будущей версии движка не читается молча по-своему.
    bool threw = false;
    try {
        AnimGraphFromJson("{\"sage_animgraph_version\": 99, \"nodes\": []}");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK_TRUE(threw);
}

TEST(AnimGraph_link_rules) {
    AnimGraph g = NewAnimGraph();
    const int clip = g.Add(AnimNodeKind::Clip);
    const int f = g.Add(AnimNodeKind::Float);
    const int b1 = g.Add(AnimNodeKind::Blend);
    const int b2 = g.Add(AnimNodeKind::Blend);
    std::string why;
    CHECK_FALSE(g.CanLink(f, b1, kBlendPinA, &why));         // число во вход позы
    CHECK_FALSE(g.CanLink(clip, b1, kBlendPinWeight, &why)); // поза во вход веса
    CHECK_FALSE(g.CanLink(clip, clip, 0, &why));             // сам в себя
    CHECK_FALSE(g.CanLink(clip, b1, 7, &why));               // нет такого входа
    CHECK_TRUE(g.Connect(b1, b2, kBlendPinA));
    CHECK_FALSE(g.CanLink(b2, b1, kBlendPinA, &why));        // петля
    // Новый провод во вход заменяет прежний.
    CHECK_TRUE(g.Connect(clip, b2, kBlendPinA));
    CHECK_EQ(g.LinkInto(b2, kBlendPinA)->From, clip);
    CHECK_EQ((int)g.Links.size(), 1);
    // Удаление узла уносит его провода.
    CHECK_TRUE(g.Remove(clip));
    CHECK_TRUE(g.LinkInto(b2, kBlendPinA) == nullptr);
}

TEST(AnimGraph_load_drops_invalid_links) {
    const std::string text = R"({"sage_animgraph_version": 1,
        "nodes": [{"id": 1, "kind": "output"}, {"id": 2, "kind": "float", "param": "X"}],
        "links": [{"from": 2, "to": 1, "pin": 0}]})";
    const AnimGraph g = AnimGraphFromJson(text);
    CHECK_EQ((int)g.Links.size(), 0);   // число на выход позы не проходит
    CHECK_EQ(g.NextId, 3);
}

// --- Проигрывание ------------------------------------------------------------

TEST(AnimGraph_clip_node_plays_and_loops) {
    const Skeleton sk = TwoBones();
    AnimGraph g = NewAnimGraph();
    const int clip = g.Add(AnimNodeKind::Clip);
    g.Find(clip)->ClipPath = "walk.sageanim";
    g.Connect(clip, g.OutputNode(), 0);
    AnimGraphInstance inst;
    inst.Bind(std::make_shared<const AnimGraph>(g), &sk, nullptr, MemoryLoader(IdleWalkFiles()));
    CHECK_TRUE(inst.Problems().empty());
    inst.Update(0.5f);
    CHECK_NEAR(inst.Pose().T[0].x, 5.0f, 1e-4);
    CHECK_NEAR(inst.Pose().T[1].y, 1.0f, 1e-5);   // кость без канала — поза покоя
    inst.Update(0.75f);                            // 1.25 по кругу -> 0.25
    CHECK_NEAR(inst.Pose().T[0].x, 2.5f, 1e-4);

    // Скорость узла и незацикленный клип: стоит на последнем кадре.
    AnimGraph g2 = g;
    g2.Find(clip)->Speed = 2.0f;
    g2.Find(clip)->Loop = false;
    AnimGraphInstance once;
    once.Bind(std::make_shared<const AnimGraph>(g2), &sk, nullptr, MemoryLoader(IdleWalkFiles()));
    once.Update(0.25f);
    CHECK_NEAR(once.Pose().T[0].x, 5.0f, 1e-4);
    once.Update(5.0f);
    CHECK_NEAR(once.Pose().T[0].x, 10.0f, 1e-4);
}

TEST(AnimGraph_missing_clip_is_reported) {
    const Skeleton sk = TwoBones();
    AnimGraph g = NewAnimGraph();
    const int clip = g.Add(AnimNodeKind::Clip);
    g.Find(clip)->ClipPath = "нет.sageanim";
    g.Connect(clip, g.OutputNode(), 0);
    AnimGraphInstance inst;
    inst.Bind(std::make_shared<const AnimGraph>(g), &sk, nullptr, MemoryLoader(IdleWalkFiles()));
    CHECK_EQ((int)inst.Problems().size(), 1);
    CHECK_NEAR(inst.Pose().T[1].y, 1.0f, 1e-5);   // поза покоя, а не мусор
}

TEST(AnimGraph_blend_mixes_bone_poses) {
    const Skeleton sk = TwoBones();
    AnimGraph g = IdleWalkGraph();
    // Поворот кости Tip: Idle — 0°, Walk — 90° вокруг Z. Середина обязана быть
    // 45° (slerp), а не кватернион «половинной длины».
    auto files = IdleWalkFiles();
    ClipAsset::Channel rot;
    rot.Joint = "Tip";
    rot.Target = AnimPath::Rotation;
    rot.Times = {0.0f};
    const glm::quat q90 = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 0, 1));
    rot.Values = {glm::vec4(q90.x, q90.y, q90.z, q90.w)};
    (*files)["walk.sageanim"].Channels.push_back(rot);

    AnimGraphInstance inst;
    inst.Bind(std::make_shared<const AnimGraph>(g), &sk, nullptr, MemoryLoader(files));
    inst.SetFloat("Speed", 0.25f);
    inst.Update(0.5f);   // Idle x=0, Walk x=5
    CHECK_NEAR(inst.Pose().T[0].x, 1.25f, 1e-4);
    inst.SetFloat("Speed", 0.5f);
    inst.Update(0.0f);
    const float angle = glm::degrees(glm::angle(inst.Pose().R[1]));
    CHECK_NEAR(angle, 45.0f, 1e-2);
    // Вес зажимается в [0, 1].
    inst.SetFloat("Speed", 7.0f);
    inst.Update(0.0f);
    CHECK_NEAR(inst.Pose().T[0].x, 5.0f, 1e-4);
    CHECK_NEAR(inst.BlendWeight(4), 1.0f, 1e-6);
    // Без параметра — значение по умолчанию, без провода — вес самого узла.
    AnimGraphInstance fresh;
    fresh.Bind(std::make_shared<const AnimGraph>(g), &sk, nullptr, MemoryLoader(files));
    fresh.Update(0.5f);
    CHECK_NEAR(fresh.Pose().T[0].x, 0.0f, 1e-4);
    AnimGraph unwired = g;
    unwired.Disconnect(4, kBlendPinWeight);
    unwired.Find(4)->Weight = 0.5f;
    AnimGraphInstance own;
    own.Bind(std::make_shared<const AnimGraph>(unwired), &sk, nullptr, MemoryLoader(files));
    own.Update(0.5f);
    CHECK_NEAR(own.Pose().T[0].x, 2.5f, 1e-4);
}

TEST(AnimGraph_instances_are_independent) {
    const Skeleton sk = TwoBones();
    const auto graph = std::make_shared<const AnimGraph>(IdleWalkGraph());
    AnimGraphInstance a, b;
    a.Bind(graph, &sk, nullptr, MemoryLoader(IdleWalkFiles()));
    b.Bind(graph, &sk, nullptr, MemoryLoader(IdleWalkFiles()));
    a.SetFloat("Speed", 1.0f);
    b.SetFloat("Speed", 1.0f);
    a.Update(0.2f);
    b.Update(0.6f);
    CHECK_NEAR(a.Pose().T[0].x, 2.0f, 1e-4);
    CHECK_NEAR(b.Pose().T[0].x, 6.0f, 1e-4);
    b.SetFloat("Speed", 0.0f);
    b.Update(0.0f);
    CHECK_NEAR(a.GetFloat("Speed"), 1.0f, 1e-6);   // параметр одного не трогает другого
    CHECK_NEAR(b.Pose().T[0].x, 0.0f, 1e-4);
    // Копия (дублирование объекта) живёт дальше своей жизнью.
    AnimGraphInstance c = a;
    c.Update(0.5f);
    CHECK_NEAR(c.Pose().T[0].x, 7.0f, 1e-4);
    CHECK_NEAR(a.Pose().T[0].x, 2.0f, 1e-4);
}

TEST(AnimGraph_rebind_keeps_clip_time) {
    const Skeleton sk = TwoBones();
    AnimGraph g = IdleWalkGraph();
    AnimGraphInstance inst;
    inst.Bind(std::make_shared<const AnimGraph>(g), &sk, nullptr, MemoryLoader(IdleWalkFiles()));
    inst.SetFloat("Speed", 1.0f);
    inst.Update(0.3f);
    g.Find(4)->Weight = 0.9f;   // правка графа в редакторе
    inst.Bind(std::make_shared<const AnimGraph>(g), &sk, nullptr, MemoryLoader(IdleWalkFiles()));
    CHECK_NEAR(inst.ClipTime(3), 0.3f, 1e-5);
    CHECK_NEAR(inst.Pose().T[0].x, 3.0f, 1e-4);
}

TEST(AnimGraph_one_graph_on_two_skeletons) {
    // Кости адресуются именами: тот же граф на скелете с другим порядком
    // костей двигает ту же кость.
    const Skeleton a = TwoBones(false), b = TwoBones(true);
    const auto graph = std::make_shared<const AnimGraph>(IdleWalkGraph());
    AnimGraphInstance ia, ib;
    ia.Bind(graph, &a, nullptr, MemoryLoader(IdleWalkFiles()));
    ib.Bind(graph, &b, nullptr, MemoryLoader(IdleWalkFiles()));
    ia.SetFloat("Speed", 1.0f);
    ib.SetFloat("Speed", 1.0f);
    ia.Update(0.4f);
    ib.Update(0.4f);
    CHECK_NEAR(ia.Pose().T[0].x, 4.0f, 1e-4);   // Root — кость 0
    CHECK_NEAR(ib.Pose().T[1].x, 4.0f, 1e-4);   // Root — кость 1
    CHECK_EQ(ia.MissingBones(), 0);
    CHECK_EQ(ib.MissingBones(), 0);
}

TEST(AnimGraph_pose_reaches_animator) {
    const Skeleton sk = TwoBones();
    AnimGraph g = IdleWalkGraph();
    AnimGraphInstance inst;
    inst.Bind(std::make_shared<const AnimGraph>(g), &sk, nullptr, MemoryLoader(IdleWalkFiles()));
    inst.SetFloat("Speed", 1.0f);
    inst.Update(0.5f);

    Animator anim;
    anim.SetRig(&sk, nullptr);
    anim.SetExternalPose(&inst.Pose());
    anim.Update(0.0f);
    // Глобальная позиция кончика: корень сдвинут на 5 по X, кончик выше на 1.
    const glm::vec3 tip = glm::vec3(anim.GlobalMatrices()[1][3]);
    CHECK_NEAR(tip.x, 5.0f, 1e-4);
    CHECK_NEAR(tip.y, 1.0f, 1e-4);
    // Ручное переопределение кости по-прежнему побеждает позу графа.
    std::vector<JointPose> overrides(2);
    overrides[0].HasTranslation = true;
    overrides[0].Translation = glm::vec3(-1, 0, 0);
    anim.SetPoseOverride(&overrides);
    anim.RefreshPose();
    CHECK_NEAR(anim.GlobalMatrices()[0][3].x, -1.0f, 1e-4);
    // Снятие внешней позы возвращает аниматор к своим клипам (здесь — покой).
    anim.SetPoseOverride(nullptr);
    anim.SetExternalPose(nullptr);
    anim.RefreshPose();
    CHECK_NEAR(anim.GlobalMatrices()[0][3].x, 0.0f, 1e-4);
}

// --- Настоящая модель --------------------------------------------------------

TEST(AnimGraph_real_skinned_model) {
    // Модель и клипы интеграционного TestGame: тотем со скелетом и
    // два клипа, вынутые из него в .sageanim.
    namespace fs = std::filesystem;
    const fs::path models = fs::path(SAGE_TEST_DATA_DIR) / ".." / "integration" / "testgame" / "assets" / "models";
    sage::render::ModelData data;
    try {
        data = sage::render::ParseSkinnedModelFile((models / "totem.glb").string());
    } catch (const std::exception& e) {
        std::printf("       модель не разобрана: %s\n", e.what());
        CHECK_TRUE(false);
        return;
    }
    const Skeleton& sk = data.Skeleton;
    CHECK_TRUE(sk.Joints.size() >= 2);
    CHECK_TRUE(data.Clips.size() >= 2);
    if (sk.Joints.size() < 2 || data.Clips.size() < 2) return;

    auto diskLoader = [&models](const std::string& path, ClipAsset& out, std::string& err) {
        try {
            out = LoadClip((models / path).string());
            return true;
        } catch (const std::exception& e) {
            err = e.what();
            return false;
        }
    };
    AnimGraph g = NewAnimGraph();
    const int c0 = g.Add(AnimNodeKind::Clip);
    const int c1 = g.Add(AnimNodeKind::Clip);
    const int blend = g.Add(AnimNodeKind::Blend);
    const int w = g.Add(AnimNodeKind::Float);
    g.Find(c0)->ClipPath = "totem.Clip_0.sageanim";
    g.Find(c1)->ClipPath = "totem.Clip_1.sageanim";
    g.Find(w)->Param = "Mix";
    g.Params.push_back({"Mix", 0.0f, 0.0f, 1.0f});
    g.Connect(c0, blend, kBlendPinA);
    g.Connect(c1, blend, kBlendPinB);
    g.Connect(w, blend, kBlendPinWeight);
    g.Connect(blend, g.OutputNode(), 0);
    const auto graph = std::make_shared<const AnimGraph>(g);

    AnimGraphInstance inst;
    inst.Bind(graph, &sk, &data.Clips, diskLoader);
    CHECK_TRUE(inst.Problems().empty());
    CHECK_EQ(inst.MissingBones(), 0);
    const float t = std::min(data.Clips[0].Duration, data.Clips[1].Duration) * 0.37f;
    inst.Update(t);
    // Вес 0 и 1 — ровно позы клипов, сэмплированные напрямую из модели.
    LocalPose p0, p1;
    SampleClipPose(sk, &data.Clips[0], t, p0);
    SampleClipPose(sk, &data.Clips[1], t, p1);
    float maxDiff0 = 0.0f, maxDiff1 = 0.0f, clipsDiffer = 0.0f;
    for (int i = 0; i < p0.Size(); ++i) {
        maxDiff0 = std::max(maxDiff0, glm::length(inst.Pose().T[i] - p0.T[i]) +
                                          (1.0f - std::abs(glm::dot(inst.Pose().R[i], p0.R[i]))));
        clipsDiffer = std::max(clipsDiffer, 1.0f - std::abs(glm::dot(p0.R[i], p1.R[i])) +
                                                glm::length(p0.T[i] - p1.T[i]));
    }
    CHECK_TRUE(maxDiff0 < 1e-3f);
    CHECK_TRUE(clipsDiffer > 1e-4f);   // клипы правда разные — иначе смешивание не проверено
    inst.SetFloat("Mix", 1.0f);
    inst.Update(0.0f);
    for (int i = 0; i < p1.Size(); ++i)
        maxDiff1 = std::max(maxDiff1, glm::length(inst.Pose().T[i] - p1.T[i]) +
                                          (1.0f - std::abs(glm::dot(inst.Pose().R[i], p1.R[i]))));
    CHECK_TRUE(maxDiff1 < 1e-3f);
    // Середина — ровно BlendPoses двух поз.
    inst.SetFloat("Mix", 0.5f);
    inst.Update(0.0f);
    LocalPose mid;
    BlendPoses(p0, p1, 0.5f, mid);
    float maxMid = 0.0f;
    for (int i = 0; i < mid.Size(); ++i)
        maxMid = std::max(maxMid, glm::length(inst.Pose().T[i] - mid.T[i]) +
                                      (1.0f - std::abs(glm::dot(inst.Pose().R[i], mid.R[i]))));
    CHECK_TRUE(maxMid < 1e-3f);

    // Клип модели по имени — тот же результат, что и вынутый файл.
    AnimGraph byName = g;
    byName.Find(c0)->ClipPath.clear();
    byName.Find(c0)->ClipName = data.Clips[0].Name;
    AnimGraphInstance named;
    named.Bind(std::make_shared<const AnimGraph>(byName), &sk, &data.Clips, diskLoader);
    CHECK_TRUE(named.Problems().empty());
    named.Update(t);
    float maxNamed = 0.0f;
    for (int i = 0; i < p0.Size(); ++i)
        maxNamed = std::max(maxNamed, glm::length(named.Pose().T[i] - p0.T[i]));
    CHECK_TRUE(maxNamed < 1e-3f);
}
