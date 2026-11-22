#pragma once
#include "sage/assets/SkinnedModelData.h"

// Модель в памяти до видеокарты — данные ресурсов (sage/assets/SkinnedModelData.h).
// Прежние имена рендера оставлены псевдонимами: ими пользуются разбор glTF,
// SkinnedModel и тесты. Скелет и клипы здесь — данные импорта; в
// anim::Skeleton и anim::AnimationClip их превращает SkinnedModel при сборке.
namespace sage::render {

using ModelImage = sage::assets::ModelImage;
using ModelSubMeshMaterial = sage::assets::ModelSubMeshMaterial;
using ModelSubMeshData = sage::assets::ModelSubMeshData;
using ModelData = sage::assets::SkinnedModelData;

} // namespace sage::render
