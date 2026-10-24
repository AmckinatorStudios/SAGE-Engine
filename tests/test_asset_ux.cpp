// Правила панели Assets и окна «Создать…»: что открывается и какой файл
// получится (editor/src/AssetOpen.h, editor/src/CreateAssetDialog.h).
#include "TestFramework.h"

#include "../editor/src/AssetOpen.h"
#include "../editor/src/CreateAssetDialog.h"

using sage::editor::assetopen::OpenAs;
namespace ca = sage::editor::createasset;

TEST(AssetOpen_only_openable_files_get_open) {
    CHECK_TRUE(sage::editor::assetopen::Of("assets/Models", true) == OpenAs::Folder);
    CHECK_TRUE(sage::editor::assetopen::Of("assets/Scenes/main.sage", false) == OpenAs::Scene);
    CHECK_TRUE(sage::editor::assetopen::Of("assets/scripts/hero.SLUA", false) == OpenAs::Text);
    CHECK_TRUE(sage::editor::assetopen::Of("assets/door.sageprefab", false) == OpenAs::Prefab);
    // Картинка, модель, звук — назначаются слотом, «Открыть» у них нет.
    CHECK_TRUE(sage::editor::assetopen::Of("assets/wood.png", false) == OpenAs::None);
    CHECK_TRUE(sage::editor::assetopen::Of("assets/tree.gltf", false) == OpenAs::None);
    CHECK_TRUE(sage::editor::assetopen::Of("assets/step.wav", false) == OpenAs::None);
}

TEST(CreateAsset_one_script_item_two_languages) {
    CHECK_EQ(ca::FileName(ca::What::Script, ca::Language::Lua, "hero"), std::string("hero.lua"));
    CHECK_EQ(ca::FileName(ca::What::Script, ca::Language::SageLua, "hero"), std::string("hero.slua"));
    CHECK_EQ(ca::FileName(ca::What::Folder, ca::Language::Lua, "Props"), std::string("Props"));
    CHECK_TRUE(ca::DefaultDir("assets", ca::What::Scene) == std::filesystem::path("assets/Scenes"));
    CHECK_TRUE(ca::DefaultDir("assets", ca::What::Material) == std::filesystem::path("assets/materials"));
}
