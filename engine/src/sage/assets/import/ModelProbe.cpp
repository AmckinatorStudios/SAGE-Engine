#include "sage/assets/import/ModelProbe.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "sage/assets/import/FbxTree.h"
#include "sage/core/Paths.h"

namespace sage::assets {

namespace {

std::string LowerExt(const std::string& path) {
    std::string ext = sage::PathFromUtf8(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return ext;
}

// Оглавление glTF: у .gltf это весь файл, у .glb — первый кусок (JSON).
// Двоичные буферы и картинки не читаются вовсе.
bool GltfHasSkins(const std::string& path) {
    std::ifstream f(sage::PathFromUtf8(path), std::ios::binary);
    if (!f) return false;
    char magic[4] = {};
    f.read(magic, 4);
    std::string json;
    if (f.gcount() == 4 && std::memcmp(magic, "glTF", 4) == 0) {
        // Заголовок 12 байт, затем кусок: длина, тип «JSON», данные.
        uint32_t header[2] = {};
        f.read(reinterpret_cast<char*>(header), sizeof(header));
        uint32_t chunk[2] = {};
        f.read(reinterpret_cast<char*>(chunk), sizeof(chunk));
        if (!f || chunk[1] != 0x4E4F534Au || chunk[0] == 0 || chunk[0] > (256u << 20)) return false;
        json.resize(chunk[0]);
        f.read(json.data(), (std::streamsize)json.size());
        if (!f) return false;
    } else {
        f.seekg(0);
        json.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    const nlohmann::json doc = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
    if (!doc.is_object()) return false;
    auto skins = doc.find("skins");
    return skins != doc.end() && skins->is_array() && !skins->empty();
}

bool FbxHasSkin(const std::string& path) {
    fbx::Node root;
    std::string err;
    if (!fbx::ReadTree(path, root, err)) return false;
    const fbx::Node* objects = root.Find("Objects");
    if (!objects) return false;
    for (const fbx::Node& n : objects->Children) {
        if (n.Name == "Deformer" && n.Props.size() > 2 && n.Props[2].Text == "Skin") return true;
    }
    return false;
}

} // namespace

bool ModelHasSkeleton(const std::string& path) {
    const std::string ext = LowerExt(path);
    if (ext == ".gltf" || ext == ".glb") return GltfHasSkins(path);
    if (ext == ".fbx") return FbxHasSkin(path);
    return false;   // .obj, .blend, .bbmodel, .sagemesh — скелета в них нет
}

} // namespace sage::assets
