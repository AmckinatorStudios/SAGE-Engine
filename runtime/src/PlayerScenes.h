#pragma once
#include <string>
#include <filesystem>
#include <vector>

#include "sage/assets/Pack.h"

// Где собранная игра ищет свои сцены — общее для запуска (PlayerLayer.cpp)
// и переходов между сценами (PlayerLayer_Scene.cpp).
namespace player {
// ГДЕ ЛЕЖАТ СЦЕНЫ. Нынешнее место — assets/Scenes (там их создаёт редактор);
// прежнее — scenes/ в корне проекта: собранные раньше игры и проекты, которые
// не открывали в новом редакторе, обязаны запускаться как прежде.
inline constexpr const char* kSceneDirs[] = {"assets/Scenes", "scenes"};

inline std::string SceneFile(const std::string& name) {
    for (const char* dir : kSceneDirs) {
        const std::string path = (std::filesystem::path(dir) / (name + ".sage")).generic_string();
        if (sage::assets::vfs::Exists(path)) return path;
    }
    return {};
}

inline std::vector<std::string> SceneFiles() {
    std::vector<std::string> out;
    for (const char* dir : kSceneDirs)
        for (const std::string& f : sage::assets::vfs::ListFiles(dir, ".sage")) out.push_back(f);
    return out;
}
} // namespace player
