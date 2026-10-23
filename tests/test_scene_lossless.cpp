// СЦЕНА БЕЗ ПОТЕРЬ: каждое записанное поле читается обратно.
//
// Проверки «компонент пережил сохранение» (самопроверка редактора,
// test_serialization) смотрят на НАЛИЧИЕ компонента и на пару полей. Поле,
// которое сериализатор пишет, а прочитать забыл, так не ловится: файл
// выглядит правильно, сцена открывается, — а после открытия значение
// молча стало умолчанием. Узнаёт об этом человек, который настроил сустав,
// сохранил и открыл назавтра.
//
// Поэтому проверка — по КАЖДОМУ полю файла, без списка полей в тесте:
//   1) сцена со всеми компонентами сохраняется в JSON;
//   2) каждое число в нём по очереди меняется, каждый флаг — переворачивается;
//   3) сцена загружается из изменённого JSON и сохраняется снова;
//   4) изменённое значение обязано оказаться на том же месте.
// Новый компонент или новое поле попадают под проверку сами, как только их
// начинают писать.
#include "TestFramework.h"

#include <cmath>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "sage/anim/AnimationComponents.h"
#include "sage/anim/PropertyAnimator.h"
#include "sage/anim/Tween.h"
#include "sage/audio/AudioComponents.h"
#include "sage/ecs/CameraLightComponents.h"
#include "sage/ecs/RenderComponents.h"
#include "sage/gi/GIComponents.h"
#include "sage/physics/PhysicsComponents.h"
#include "sage/render/LodGroup.h"
#include "sage/render/ParticleComponents.h"
#include "sage/render/PostProcessComponent.h"
#include "sage/render/ReflectionComponents.h"
#include "sage/net/NetComponents.h"
#include "sage/scene/Prefab.h"
#include "sage/scene/Scene.h"
#include "sage/scene/SceneSerializer.h"
#include "sage/scene/ScriptComponent.h"
#include "sage/ui/UI.h"
#include "sage/ui/UIPresets.h"

using json = nlohmann::json;

namespace {

// Сцена, в которой есть всё, что сцена умеет хранить.
std::unique_ptr<Scene> EverythingScene() {
    auto scene = std::make_unique<Scene>("Everything");
    entt::registry& reg = scene->Registry();
    GameObject all = scene->CreateObject("All");
    const entt::entity e = all.Entity();
    reg.emplace_or_replace<MeshRendererComponent>(e);
    reg.emplace_or_replace<DecalComponent>(e);
    reg.emplace_or_replace<GIStaticComponent>(e);
    reg.emplace_or_replace<CameraComponent>(e);
    reg.emplace_or_replace<LightComponent>(e);
    reg.emplace_or_replace<RigidBodyComponent>(e);
    reg.emplace_or_replace<ColliderComponent>(e);
    reg.emplace_or_replace<JointComponent>(e);
    reg.emplace_or_replace<CharacterControllerComponent>(e);
    reg.emplace_or_replace<AnimationComponent>(e);
    reg.emplace_or_replace<IKComponent>(e);
    reg.emplace_or_replace<ReflectionProbeComponent>(e);
    reg.emplace_or_replace<ParticleEmitterComponent>(e);
    reg.emplace_or_replace<AudioSourceComponent>(e);
    reg.emplace_or_replace<RenderTextureComponent>(e).Target = "icon";
    reg.emplace_or_replace<sage::render::PostProcessComponent>(e);
    reg.emplace_or_replace<ScriptComponent>(e, ScriptComponent{"assets/scripts/x.lua"});
    reg.emplace_or_replace<TagComponent>(e, TagComponent{"Player"});
    reg.emplace_or_replace<DecalComponent>(e).AngleLimitDeg = 33.0f;
    reg.emplace_or_replace<PropertyAnimatorComponent>(e).ClipPath = "assets/anim/door.sageclip";
    reg.emplace_or_replace<sage::render::LodComponent>(e);
    reg.emplace_or_replace<NetReplicatedComponent>(e);
    reg.emplace_or_replace<KeepOnLoadComponent>(e);
    reg.emplace_or_replace<HiddenComponent>(e);
    {
        sage::anim::TweenComponent& tc = reg.emplace_or_replace<sage::anim::TweenComponent>(e);
        sage::anim::TweenClip clip;
        clip.Name = "Pop";
        sage::anim::TweenTrack t;
        t.Property = "object.scale";
        t.To = glm::vec4(2.0f);
        clip.Tracks.push_back(t);
        tc.Tweens.push_back(clip);
    }
    // Каждая заготовка интерфейса — своим объектом: части элементов разные.
    for (const std::string& preset : sage::ui::PresetNames()) {
        GameObject ui = scene->CreateObject("UI " + preset);
        sage::ui::ApplyPreset(*scene, ui.Entity(), preset);
    }
    return scene;
}

// Объект сцены в JSON без номеров: у копии номера свои, а всё остальное
// обязано совпасть.
json WithoutIds(json j) {
    if (j.is_object()) {
        for (const char* k : {"id", "parent", "children"}) j.erase(k);
        for (auto& [k, v] : j.items()) v = WithoutIds(v);
    } else if (j.is_array()) {
        for (auto& v : j) v = WithoutIds(v);
    }
    return j;
}

// Все «листья» JSON, которые можно изменить: путь и способ изменить.
struct Leaf {
    json::json_pointer Path;
};

void CollectLeaves(const json& j, const json::json_pointer& at, std::vector<Leaf>& out) {
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) CollectLeaves(it.value(), at / it.key(), out);
    } else if (j.is_array()) {
        for (size_t i = 0; i < j.size(); ++i) CollectLeaves(j[i], at / std::to_string(i), out);
    } else if (j.is_number() || j.is_boolean()) {
        out.push_back({at});
    }
}

// Служебные поля, менять которые бессмысленно: номер версии формата и
// номера сущностей (на них держатся ссылки — сдвиг одного ломает связи, а
// не проверяет поле).
bool Skip(const std::string& path) {
    // ambientColor — приближение для старых инструментов, читается только
    // из файлов без настоящих полей окружающего света (SceneSerializer.cpp).
    static const char* const kSkip[] = {"/sage_scene_version", "/id", "/parent", "/children", "Id", "Entity",
                                        "/target", "/other", "/owner", "/lighting/ambientColor"};
    for (const char* s : kSkip)
        if (path.find(s) != std::string::npos) return true;
    return false;
}

json Mutated(const json& v) {
    if (v.is_boolean()) return !v.get<bool>();
    if (v.is_number_integer() || v.is_number_unsigned()) {
        // Целое — тоже на шаг: перечисления (тип света, форма коллайдера)
        // так переходят в соседнее значение, которое обязано сохраниться.
        // Вниз, а не вверх: последнее значение перечисления +1 вышло бы за
        // его край, а маска «все слои» — за 32 бита.
        const long long n = v.get<long long>();
        return n > 0 ? n - 1 : 1;
    }
    const double d = v.get<double>();
    return d + 0.375;   // точно представимо во float: сравнение без допуска
}

} // namespace

TEST(Scene_every_saved_value_is_read_back) {
    auto scene = EverythingScene();
    const json base = json::parse(SceneSerializer::SaveToString(*scene));
    std::vector<Leaf> leaves;
    CollectLeaves(base, json::json_pointer(), leaves);
    CHECK_TRUE(leaves.size() > 200);   // сцена действительно «со всем»

    int lost = 0, checked = 0;
    for (const Leaf& leaf : leaves) {
        const std::string path = leaf.Path.to_string();
        if (Skip(path)) continue;
        json changed = base;
        const json want = Mutated(changed[leaf.Path]);
        changed[leaf.Path] = want;
        json again;
        try {
            again = json::parse(SceneSerializer::SaveToString(*SceneSerializer::LoadFromString(changed.dump())));
        } catch (const std::exception& ex) {
            std::printf("       %s: загрузка упала: %s\n", path.c_str(), ex.what());
            ++lost;
            continue;
        }
        ++checked;
        // Флаг, который пишется только когда включён (noMesh): «выключен» и
        // «поля нет» — одно и то же.
        bool same = again.contains(leaf.Path) || (want.is_boolean() && !want.get<bool>());
        if (same && again.contains(leaf.Path)) {
            const json& got = again[leaf.Path];
            if (want.is_number() && got.is_number())
                same = std::fabs(want.get<double>() - got.get<double>()) < 1e-4;
            else
                same = got == want;
        }
        if (!same) {
            std::printf("       потеряно: %s (записали %s, прочитали %s)\n", path.c_str(), want.dump().c_str(),
                        again.contains(leaf.Path) ? again[leaf.Path].dump().c_str() : "нет поля");
            ++lost;
        }
    }
    std::printf("       полей проверено: %d, потеряно: %d\n", checked, lost);
    CHECK_EQ(lost, 0);
}

// Повторное сохранение не меняет файл: сохранить → открыть → сохранить даёт
// тот же JSON. Иначе каждое открытие сцены медленно её «дрейфует».
TEST(Scene_save_load_save_is_stable) {
    auto scene = EverythingScene();
    const std::string a = SceneSerializer::SaveToString(*scene);
    const std::string b = SceneSerializer::SaveToString(*SceneSerializer::LoadFromString(a));
    const std::string c = SceneSerializer::SaveToString(*SceneSerializer::LoadFromString(b));
    CHECK_TRUE(json::parse(a) == json::parse(b));
    CHECK_TRUE(b == c);
}

// Каждый компонент возвращается из файла. Проверка по ТИПАМ, а не по JSON:
// компонент, который сериализатор не пишет вовсе, в JSON не виден ни до, ни
// после, и сравнение файлов его не заметит.
TEST(Scene_every_component_survives_save_and_load) {
    auto scene = EverythingScene();
    auto loaded = SceneSerializer::LoadFromString(SceneSerializer::SaveToString(*scene));
    GameObject all = loaded->FindByName("All");
    CHECK_TRUE(all.Valid());
    if (!all.Valid()) return;
    entt::registry& r = loaded->Registry();
    const entt::entity e = all.Entity();
    struct Has { const char* Name; bool Present; };
    const Has has[] = {
        {"MeshRenderer", r.all_of<MeshRendererComponent>(e)},
        {"Decal", r.all_of<DecalComponent>(e)},
        {"GIStatic", r.all_of<GIStaticComponent>(e)},
        {"Camera", r.all_of<CameraComponent>(e)},
        {"Light", r.all_of<LightComponent>(e)},
        {"RigidBody", r.all_of<RigidBodyComponent>(e)},
        {"Collider", r.all_of<ColliderComponent>(e)},
        {"Joint", r.all_of<JointComponent>(e)},
        {"CharacterController", r.all_of<CharacterControllerComponent>(e)},
        {"Animation", r.all_of<AnimationComponent>(e)},
        {"IK", r.all_of<IKComponent>(e)},
        {"ReflectionProbe", r.all_of<ReflectionProbeComponent>(e)},
        {"ParticleEmitter", r.all_of<ParticleEmitterComponent>(e)},
        {"AudioSource", r.all_of<AudioSourceComponent>(e)},
        {"RenderTexture", r.all_of<RenderTextureComponent>(e)},
        {"PostProcess", r.all_of<sage::render::PostProcessComponent>(e)},
        {"Script", r.all_of<ScriptComponent>(e)},
        {"Tween", r.all_of<sage::anim::TweenComponent>(e)},
        {"Tag", r.all_of<TagComponent>(e)},
        {"PropertyAnimator", r.all_of<PropertyAnimatorComponent>(e)},
        {"Lod", r.all_of<sage::render::LodComponent>(e)},
        {"NetReplicated", r.all_of<NetReplicatedComponent>(e)},
        {"KeepOnLoad", r.all_of<KeepOnLoadComponent>(e)},
        {"Hidden", r.all_of<HiddenComponent>(e)},
    };
    for (const Has& h : has) {
        if (!h.Present) std::printf("       не вернулся из файла: %s\n", h.Name);
        CHECK_TRUE(h.Present);
    }
    if (const auto* rt = r.try_get<RenderTextureComponent>(e)) CHECK_TRUE(rt->Target == "icon");
}

// КОПИЯ ОБЪЕКТА — ПОЛНАЯ: дублирование (Ctrl+D), префаб, вставка и перенос
// между сценами идут через CopySubtree. Его список компонентов был написан
// руками и устарел: копия теряла тег, звук, твины, наклейку, LOD,
// пост-обработку, сеть — молча, файл префаба выглядел нормальным.
TEST(Scene_copied_object_keeps_every_component) {
    auto scene = EverythingScene();
    Scene copy("Copy");
    for (entt::entity e : scene->Roots()) sage::scene::CopySubtree(*scene, e, copy, entt::null);
    const json a = json::parse(SceneSerializer::SaveToString(*scene));
    const json b = json::parse(SceneSerializer::SaveToString(copy));
    CHECK_EQ(a["objects"].size(), b["objects"].size());
    for (size_t i = 0; i < a["objects"].size() && i < b["objects"].size(); ++i) {
        const json x = WithoutIds(a["objects"][i]), y = WithoutIds(b["objects"][i]);
        if (x != y) {
            for (auto& [k, v] : x.items())
                if (!y.contains(k) || y[k] != v)
                    std::printf("       копия %s потеряла: %s\n", x.value("name", "?").c_str(), k.c_str());
        }
        CHECK_TRUE(x == y);
    }
    // И в копии ничего не проснулось само: номер звука, «уже запущен» у
    // твина — состояние ЭКЗЕМПЛЯРА, а не шаблона.
    GameObject c = copy.FindByName("All");
    if (c.Valid()) {
        if (const auto* au = copy.Registry().try_get<AudioSourceComponent>(c.Entity())) CHECK_EQ((int)au->Handle, 0);
        if (const auto* tw = copy.Registry().try_get<sage::anim::TweenComponent>(c.Entity())) CHECK_TRUE(!tw->Started);
    }
}
