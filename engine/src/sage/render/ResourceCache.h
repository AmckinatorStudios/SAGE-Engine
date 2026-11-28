#pragma once

#include "sage/render/Texture.h"

#include <memory>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

class Mesh;
class Material;
class Shader;
class Skybox;

namespace sage::render { class SkinnedModel; }

// Хранилище записей файловых ресурсов. Оно не загружает файлы и не принимает
// решения о памяти: этим занимаются ResourceManager и VRAMBudgetManager.
class ResourceCache {
public:
    struct ShaderEntry {
        std::shared_ptr<Shader> Program;
        std::string VertPath, FragPath;
        long long VertStamp = 0, FragStamp = 0;
        long long IncludeStamp = 0;
        bool Screen = false;
    };

    struct TextureRecord {
        std::shared_ptr<Texture> Tex;
        size_t Bytes = 0;
        uint64_t Tick = 0;
        bool Pending = false;
        std::string Source;
        TextureFilter Filter = TextureFilter::Trilinear;
        bool Mipmaps = true;
        int MaxSide = 0;
        bool Generated = false;
        bool Bleed = false;
        bool Srgb = false;
        long long Stamp = 0;
    };

    std::unordered_map<std::string, std::shared_ptr<Mesh>> Models;
    std::unordered_map<std::string, std::shared_ptr<Material>> Materials;
    std::unordered_map<std::string, long long> ModelStamps;
    std::unordered_map<std::string, long long> MaterialStamps;
    std::unordered_map<std::string, long long> SkinnedStamps;
    std::unordered_map<std::string, ShaderEntry> Shaders;
    std::unordered_map<std::string, std::shared_ptr<Skybox>> Skyboxes;
    std::unordered_map<std::string, TextureRecord> Textures;
    std::unordered_map<std::string, std::shared_ptr<sage::render::SkinnedModel>> Skinned;
    size_t ReloadCursor = 0;
    uint64_t AssetsGeneration = 0;

    ResourceCache() = default;
    ~ResourceCache();
    ResourceCache(const ResourceCache&) = delete;
    ResourceCache& operator=(const ResourceCache&) = delete;
};
