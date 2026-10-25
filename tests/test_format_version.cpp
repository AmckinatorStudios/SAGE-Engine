// ВЕРСИИ ФОРМАТОВ (sage/core/FormatVersion.h): одно правило для всех файлов.
// Нет номера — версия 1; файл новее движка — отказ, а не чтение наполовину;
// миграции — по шагу, атомарно. И каждый формат движка этому правилу следует:
// раньше раскладка управления, клипы и оглавление ассетов номер писали, но не
// читали, а у материала его не было вовсе.
#include "TestFramework.h"

#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "sage/anim/ClipFile.h"
#include "sage/anim/PropertyClip.h"
#include "sage/assets/AssetDatabase.h"
#include "sage/core/FormatVersion.h"
#include "sage/input/InputSystem.h"
#include "sage/render/Material.h"
#include "sage/ui/Interface.h"

namespace fs = std::filesystem;
using nlohmann::json;

TEST(FormatVersion_missing_number_is_version_one_and_gets_stamped) {
    sage::format::Spec spec{"v", 1, "тест", {}};
    json doc = {{"a", 1}};
    std::string err;
    CHECK_EQ(sage::format::VersionOf(doc, spec), 1);
    CHECK_TRUE(sage::format::Upgrade(doc, spec, err));
    CHECK_EQ(doc["v"].get<int>(), 1);
}

TEST(FormatVersion_newer_file_is_refused_and_left_untouched) {
    sage::format::Spec spec{"v", 2, "тест", {{1, [](json& d) { d["b"] = d["a"]; }}}};
    json doc = {{"v", 3}, {"a", 1}};
    const json before = doc;
    std::string err;
    CHECK_TRUE(!sage::format::Upgrade(doc, spec, err));
    CHECK_TRUE(err.find("более новой") != std::string::npos);
    CHECK_TRUE(doc == before);
}

TEST(FormatVersion_steps_run_in_order_and_are_idempotent) {
    sage::format::Spec spec{"v", 3, "тест",
                            {{2, [](json& d) { d["log"] = d["log"].get<std::string>() + "b"; }},
                             {1, [](json& d) { d["log"] = d.value("log", std::string()) + "a"; }}}};
    json doc = json::object();   // без номера: версия 1
    std::string err;
    CHECK_TRUE(sage::format::Upgrade(doc, spec, err));
    CHECK_TRUE(doc["log"] == "ab" && doc["v"] == 3);
    CHECK_TRUE(sage::format::Upgrade(doc, spec, err));   // второй прогон — ничего
    CHECK_TRUE(doc["log"] == "ab");
}

TEST(FormatVersion_failed_or_missing_step_leaves_the_document_whole) {
    sage::format::Spec broken{"v", 3, "тест",
                              {{1, [](json& d) { d["x"] = 1; }}, {2, [](json&) { throw std::runtime_error("бум"); }}}};
    json doc = {{"v", 1}};
    std::string err;
    CHECK_TRUE(!sage::format::Upgrade(doc, broken, err));
    CHECK_TRUE(!doc.contains("x") && doc["v"] == 1);   // первый шаг не остался наполовину
    sage::format::Spec gap{"v", 2, "тест", {}};
    CHECK_TRUE(!sage::format::Upgrade(doc, gap, err));
    CHECK_TRUE(err.find("нет миграции") != std::string::npos);
    json bad = {{"v", "один"}};
    CHECK_TRUE(!sage::format::Upgrade(bad, gap, err));
}

// --- Каждый формат движка отказывается от файла из будущего -----------------

TEST(FormatVersion_every_engine_format_refuses_a_future_file) {
    // Раскладка управления.
    {
        sage::input::InputSystem in;
        CHECK_TRUE(!in.LoadMappingFromString(R"({"version": 99, "contexts": []})"));
        CHECK_TRUE(in.LoadMappingFromString(R"({"contexts": []})"));   // старый файл без номера
    }
    // Клип свойств.
    {
        sage::anim::PropertyClip c;
        std::string err;
        CHECK_TRUE(!sage::anim::FromJsonString(R"({"sage_clip_version": 99, "name": "x"})", c, err));
        CHECK_TRUE(sage::anim::FromJsonString(R"({"name": "x"})", c, err));
    }
    // Интерфейс.
    {
        sage::ui::Interface i;
        std::string err;
        CHECK_TRUE(!sage::ui::Interface::FromJsonString(R"({"sage_interface_version": 99})", i, err));
    }
    // Оглавление ассетов.
    {
        sage::AssetDatabase db;
        CHECK_EQ(db.LoadIndex(R"({"version": 99, "assets": [{"g": "0123456789abcdef0123456789abcdef", "p": "a.png"}]})"), 0);
    }
    const fs::path dir = fs::temp_directory_path() / "sage_format_version";
    fs::create_directories(dir);
    // Скелетный клип.
    {
        const fs::path p = dir / "future.sageanim";
        std::ofstream(p) << R"({"sage_anim_version": 99, "name": "x", "channels": []})";
        bool threw = false;
        try { sage::anim::LoadClip(p.string()); } catch (const std::exception&) { threw = true; }
        CHECK_TRUE(threw);
    }
    // Материал: номер теперь пишется, будущий — отказ, старый без номера читается.
    {
        const fs::path p = dir / "m.sagemat";
        Material m;
        m.Metallic = 0.75f;
        m.SaveToFile(p.string());
        std::ifstream in(p);
        json j;
        in >> j;
        CHECK_TRUE(j.contains("sage_material_version"));
        j.erase("sage_material_version");
        std::ofstream(p) << j.dump();
        CHECK_NEAR(Material::LoadFromFile(p.string()).Metallic, 0.75f, 1e-5f);
        j["sage_material_version"] = 99;
        std::ofstream(p) << j.dump();
        bool threw = false;
        try { Material::LoadFromFile(p.string()); } catch (const std::exception&) { threw = true; }
        CHECK_TRUE(threw);
    }
    fs::remove_all(dir);
}

// --- Повороты в анимациях переводятся вместе с самим поворотом ----------------
//
// Соглашение об углах сменилось (R = Ry*Rx*Rz, Transform.h). Сцены перевела
// миграция v16 -> v17, но клип свойств и твин хранят ПОВОРОТ ЧИСЛАМИ — без
// перевода объект, повёрнутый по двум осям, играл бы не ту позу, а соседние
// ключи, переведённые независимо, давали бы проворот через полный оборот.
#include "sage/scene/Transform.h"
#include "sage/scene/SceneSerializer.h"
#include <glm/gtc/matrix_transform.hpp>

namespace {
glm::mat4 LegacyRotation(const glm::vec3& a) {
    glm::mat4 m = glm::rotate(glm::mat4(1.0f), glm::radians(a.x), glm::vec3(1, 0, 0));
    m = glm::rotate(m, glm::radians(a.y), glm::vec3(0, 1, 0));
    return glm::rotate(m, glm::radians(a.z), glm::vec3(0, 0, 1));
}
bool SameRotation(const glm::mat4& a, const glm::mat4& b) {
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            if (std::abs(a[c][r] - b[c][r]) > 1e-3f) return false;
    return true;
}
} // namespace

TEST(PropertyClip_v1_rotation_keys_keep_their_pose_and_path) {
    const char* old = R"({"sage_clip_version": 1, "name": "spin", "duration": 2,
      "tracks": [{"target": "", "property": "object.rotation", "keys": [
        {"time": 0, "value": [30, 40, 0, 0]},
        {"time": 1, "value": [35, 120, 0, 0]},
        {"time": 2, "value": [40, 200, 10, 0]}]},
       {"target": "", "property": "object.position", "keys": [{"time": 0, "value": [30, 40, 0, 0]}]}]})";
    sage::anim::PropertyClip clip;
    std::string err;
    CHECK_TRUE(sage::anim::FromJsonString(old, clip, err));
    CHECK_EQ(clip.Tracks.size(), (size_t)2);
    const glm::vec3 legacy[] = {{30, 40, 0}, {35, 120, 0}, {40, 200, 10}};
    const auto& keys = clip.Tracks[0].Keys;
    for (size_t i = 0; i < keys.size(); ++i) {
        const glm::vec3 now(keys[i].Value);
        CHECK_TRUE(SameRotation(sage::RotationMatrixFromEulerDegrees(now), LegacyRotation(legacy[i])));
        // Соседние ключи не разбегаются на полный оборот.
        if (i > 0) {
            const glm::vec3 d = glm::abs(now - glm::vec3(keys[i - 1].Value));
            CHECK_TRUE(d.x < 180.0f && d.y < 180.0f && d.z < 180.0f);
        }
    }
    // Не поворот — не трогается.
    CHECK_NEAR(clip.Tracks[1].Keys[0].Value.x, 30.0f, 1e-5f);
}

TEST(Scene_v17_rotation_tweens_aim_at_the_same_pose) {
    const std::string old = R"({"name": "T", "sage_scene_version": 17, "objects": [
      {"id": 1, "name": "Door", "position": {"x":0,"y":0,"z":0}, "rotation": {"x": 0, "y": 0, "z": 0},
       "scale": {"x":1,"y":1,"z":1}, "mesh": {"type": "cube", "path": ""},
       "tweens": [{"name": "Open", "tracks": [
          {"property": "object.rotation", "start": 0, "duration": 1,
           "to": {"x": 20, "y": 90, "z": 0, "w": 0}, "ease": "linear"}]}]}]})";
    const nlohmann::json j = nlohmann::json::parse(SceneSerializer::MigrateSceneJson(old));
    const nlohmann::json& to = j["objects"][0]["tweens"][0]["tracks"][0]["to"];
    const glm::vec3 now(to["x"].get<float>(), to["y"].get<float>(), to["z"].get<float>());
    CHECK_TRUE(SameRotation(sage::RotationMatrixFromEulerDegrees(now), LegacyRotation({20, 90, 0})));
}

TEST(Euler_equivalent_triples_are_the_same_rotation) {
    const glm::vec3 a(25.0f, 70.0f, -40.0f);
    const glm::vec3 b = sage::ClosestEquivalentEuler(a, glm::vec3(160.0f, 250.0f, 140.0f));
    CHECK_TRUE(SameRotation(sage::RotationMatrixFromEulerDegrees(a), sage::RotationMatrixFromEulerDegrees(b)));
    CHECK_TRUE(std::abs(b.x - 155.0f) < 1e-3f);   // выбрана вторая запись, ближняя к ref
}
