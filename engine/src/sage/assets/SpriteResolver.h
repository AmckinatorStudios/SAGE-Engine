#pragma once

#include "sage/assets/SpriteSheet.h"

#include <memory>
#include <unordered_map>

namespace sage::assets {

struct SpriteImage {
    uint64_t Generation = 0;
    int Width = 0, Height = 0;
    std::vector<unsigned char> Pixels; // RGBA, top row first
};

struct SpriteGeneration {
    uint64_t Number = 0;
    SpriteSheet Sheet;
    std::shared_ptr<const SpriteImage> Image;
    glm::ivec2 Grid{1}; // Compatibility atlas for ordered standalone frame files.
};

struct ResolvedSprite {
    std::shared_ptr<const SpriteGeneration> Resource;
    SpriteRef Ref;
    std::string Diagnostic; // May accompany a last known good generation.
    const SpriteSequence* Sequence() const;
    const SpriteRegion* Region(float time = 0) const;
    glm::vec4 UVFor(const SpriteRegion& region) const;
    glm::vec4 UV(float time = 0) const; // left, top, right, bottom; top-left origin
    explicit operator bool() const;
};

// Owned by the resource context. Poll and resolve on the owner thread.
class SpriteResolver {
public:
    ResolvedSprite Resolve(AssetDatabase& database, SpriteRef ref, AssetGuid dependent = {}, bool refresh = true);
    ResolvedSprite ResolveSheet(AssetDatabase& database, AssetGuid guid, bool allowMetadataOnly = false, bool refresh = true);
    ResolvedSprite ResolveImage(AssetDatabase& database, const std::string& image, bool allowMissingSheet = false);
    ResolvedSprite ResolveFrames(AssetDatabase& database, const std::vector<std::string>& frames, int maxSide);
    ResolvedSprite Previous(SpriteRef ref) const;
    size_t Poll(AssetDatabase& database, size_t budget);
    bool Empty() const { return m_entries.empty(); }
    void Clear();
    void NotifyChanged(AssetGuid guid);
    static bool ValidateSource(const SpriteSheet& sheet, const std::string& source, std::string& err);

private:
    struct Entry {
        AssetGuid Guid;
        std::string Source;
        bool HasDocument = false;
        std::vector<std::string> Frames, FrameSignatures;
        size_t FrameCursor = 0;
        int MaxSide = 0;
        std::string Signature, ImagePath, ImageSignature, Diagnostic;
        std::shared_ptr<const SpriteGeneration> Good, Previous, Metadata;
        uint64_t Tick = 0;
    };
    bool Refresh(AssetDatabase&, Entry&);
    bool RefreshFrames(AssetDatabase&, Entry&);
    void Trim();
    std::unordered_map<std::string, Entry> m_entries;
    uint64_t m_tick = 0, m_generation = 0;
    size_t m_cursor = 0;
};

} // namespace sage::assets
