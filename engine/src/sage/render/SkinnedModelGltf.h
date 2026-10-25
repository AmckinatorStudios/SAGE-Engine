#pragma once
#include <string>

#include "sage/render/ModelData.h"

// Разбор glTF/GLB со скелетом и анимациями в ModelData (SkinnedModel_Gltf.cpp).
//
// Отдельным файлом, потому что разбор — шестьсот строк, которые не трогают
// видеокарту, а SkinnedModel.cpp — про то, как модель рисуется и двигается:
// правка одного не должна пересобирать и путать другое.
namespace sage::render {

ModelData ParseGltfSkinned(const std::string& path);

} // namespace sage::render
