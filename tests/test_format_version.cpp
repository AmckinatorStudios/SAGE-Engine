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
