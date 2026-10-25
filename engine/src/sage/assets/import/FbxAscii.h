#pragma once
#include <string>

#include "sage/assets/import/FbxTree.h"

// Текстовый FBX (Blockbench, старые выгрузки) — в то же дерево записей, что
// и двоичный (см. FbxAscii.cpp). Зовётся из ReadTree, когда у файла нет
// двоичной подписи.
namespace sage::assets::fbx {

// Похоже ли начало файла на текстовый FBX.
bool LooksLikeAsciiFbx(const std::string& head);

// Разбор текста в дерево. false + err — с указанием, где разбор споткнулся.
bool ReadAsciiTree(const std::string& text, Node& root, std::string& err);

} // namespace sage::assets::fbx
