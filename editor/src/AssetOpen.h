#pragma once
#include <filesystem>
#include <string>

// ---------------------------------------------------------------------------
// ЧТО ЗНАЧИТ «ОТКРЫТЬ» ФАЙЛ ПРОЕКТА — одно правило для двойного щелчка и для
// пункта «Открыть» в меню панели Assets.
//
// Пункт «Открыть» стоит в меню ТОЛЬКО у того, что открывается: у картинки,
// модели и звука его нет (их назначают слотом и перетаскивают), — пункт,
// который ничего не делает, хуже отсутствующего.
// ---------------------------------------------------------------------------
namespace sage::editor::assetopen {

enum class OpenAs { None, Folder, Scene, Prefab, Clip, Text, AiGraph, VisualScript };

inline OpenAs Of(const std::filesystem::path& path, bool isDir) {
    if (isDir) return OpenAs::Folder;
    std::string ext = path.extension().string();
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    if (ext == ".sage") return OpenAs::Scene;
    if (ext == ".sageprefab") return OpenAs::Prefab;
    if (ext == ".sageclip" || ext == ".sageanim") return OpenAs::Clip;
    if (ext == ".aigraph") return OpenAs::AiGraph;
    if (ext == ".vgraph") return OpenAs::VisualScript;
    if (ext == ".lua" || ext == ".slua" || ext == ".vert" || ext == ".frag" || ext == ".glsl" || ext == ".txt" ||
        ext == ".md" || ext == ".json")
        return OpenAs::Text;
    return OpenAs::None;
}

} // namespace sage::editor::assetopen
