#pragma once

#include "sage/assets/SpriteSheet.h"

class EditorHost;

namespace sage::editor {

// The caller supplies its own ImGui ID scope.
bool SpritePicker(EditorHost& host, const char* label, assets::SpriteRef& value);

} // namespace sage::editor
