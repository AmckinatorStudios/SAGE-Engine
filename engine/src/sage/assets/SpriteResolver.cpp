#include "sage/assets/SpriteResolver.h"

#include "sage/assets/Pack.h"
#include "sage/assets/GridAtlas.h"
#include "sage/core/ImageDecodeBudget.h"
#include "sage/core/Log.h"
#include "sage/core/Paths.h"

#include <algorithm>
#include <filesystem>
#include <stb_image.h>

namespace sage::assets {
namespace {
constexpr size_t kMaxImageBytes = 64u << 20;
constexpr size_t kFrameInputBytes = 16u << 20;
constexpr size_t kFrameDecoderBytes = 64u << 20;

std::string Signature(const std::string& file) {
    if (!vfs::PackedKey(file).empty()) return "pack:" + std::to_string(vfs::MountGeneration()) + ":" + file;
    std::error_code ec;
    const auto path = PathFromUtf8(file);
    const auto stamp = std::filesystem::last_write_time(path, ec);
    if (ec) return "missing:" + file;
    const auto bytes = std::filesystem::file_size(path, ec);
    return file + ":" + std::to_string(stamp.time_since_epoch().count()) + ":" + std::to_string(bytes);
}

std::shared_ptr<SpriteImage> Decode(const std::string& path, glm::ivec2 expected = {}) {
    std::vector<uint8_t> bytes;
    if (!vfs::ReadFile(path, bytes, expected.x ? kFrameInputBytes : kMaxImageBytes) || bytes.empty()) return {};
    image_memory::Scope memory(expected.x ? kFrameDecoderBytes : 4 * kMaxImageBytes);
    int w = 0, h = 0, channels = 0;
    const int size = static_cast<int>(bytes.size());
    if (!stbi_info_from_memory(bytes.data(), size, &w, &h, &channels) || w <= 0 || h <= 0 ||
        uint64_t(w) * h * 4 > kMaxImageBytes ||
        (expected.x && (w != expected.x || h != expected.y))) return {};
    stbi_set_flip_vertically_on_load_thread(0);
    std::unique_ptr<unsigned char, decltype(&stbi_image_free)> pixels(
        stbi_load_from_memory(bytes.data(), size, &w, &h, &channels, 4), stbi_image_free);
    if (!pixels || w <= 0 || h <= 0 || uint64_t(w) * h * 4 > kMaxImageBytes ||
        (expected.x && (w != expected.x || h != expected.y))) return {};
    auto result = std::make_shared<SpriteImage>();
    result->Width = w;
    result->Height = h;
    result->Pixels.assign(pixels.get(), pixels.get() + size_t(w) * h * 4);
    return result;
}
} // namespace

const SpriteSequence* ResolvedSprite::Sequence() const {
    return Resource && Ref.Type == SpriteRef::Kind::Sequence ? Resource->Sheet.FindSequence(Ref.Item) : nullptr;
}

const SpriteRegion* ResolvedSprite::Region(float time) const {
    if (!Resource) return nullptr;
    if (Ref.Type == SpriteRef::Kind::Region) return Resource->Sheet.Find(Ref.Item);
    const auto* sequence = Sequence();
    return sequence ? Resource->Sheet.Find(sequence->FrameIdAt(time)) : nullptr;
}

ResolvedSprite::operator bool() const { return Resource && (!Ref.Item.Valid() || Sequence() || Region()); }

glm::vec4 ResolvedSprite::UV(float time) const {
    const auto* region = Region(time);
    return region ? UVFor(*region) : glm::vec4(0.0f);
}

glm::vec4 ResolvedSprite::UVFor(const SpriteRegion& region) const {
    if (!Resource || !Resource->Image) return {};
    const auto rect = region.Rect;
    const float w = float(Resource->Image->Width), h = float(Resource->Image->Height);
    return {rect.x / w, rect.y / h, (rect.x + rect.z) / w, (rect.y + rect.w) / h};
}

bool SpriteResolver::ValidateSource(const SpriteSheet& sheet, const std::string& source, std::string& err) {
    const auto image = Decode(source);
    if (!image) { err = "cannot decode image: " + source; return false; }
    return sheet.Validate(err, image->Width, image->Height);
}

bool SpriteResolver::Refresh(AssetDatabase& database, Entry& entry) {
    if (!entry.Frames.empty()) return RefreshFrames(database, entry);
    const auto guid = entry.Guid;
    const auto relative = database.PathOf(guid);
    const auto document = !entry.Source.empty() ? SpriteSheet::SidecarPath(entry.Source) : relative.empty() || database.ProjectDir().empty() || !vfs::PackedKey(relative).empty()
                        ? relative : PathToUtf8(PathFromUtf8(database.ProjectDir()) / PathFromUtf8(relative));
    const std::string suffix = ".sagesheet";
    const auto base = !entry.Source.empty() ? entry.Source : document.size() > suffix.size() &&
                      document.compare(document.size() - suffix.size(), suffix.size(), suffix) == 0
                    ? document.substr(0, document.size() - suffix.size()) : std::string{};
    const auto previousSource = entry.ImagePath.empty() ? base : entry.ImagePath;
    auto stamp = [&](const std::string& image) {
        return Signature(document) + "|" + Signature(NineSlice::SidecarPath(base)) + "|" + Signature(image);
    };
    auto observed = stamp(previousSource);
    if (observed == entry.Signature) return false;
    auto fail = [&](const std::string& why) {
        entry.Signature = observed;
        entry.Diagnostic = "Sprite " + guid.ToString() + " (" + document + "): " + why;
        LOG_WARN("Sprites") << entry.Diagnostic;
        return false;
    };
    if (base.empty()) {
        entry.Metadata.reset();
        entry.Good.reset();
        entry.Previous.reset();
        return fail("sheet GUID is unregistered or is not a sprite sheet");
    }
    entry.HasDocument = vfs::Exists(document);
    if (guid.Valid() && vfs::Mounted() && vfs::PackedKey(document).empty())
        return fail("sheet is absent from the mounted package");
    entry.Metadata.reset();
    const auto documentStamp = Signature(document);
    const auto legacyStamp = Signature(NineSlice::SidecarPath(base));
    SpriteSheet sheet;
    std::string error;
    const bool loaded = guid.Valid() || entry.HasDocument ? SpriteSheet::LoadFile(document, sheet, error)
                                                          : SpriteSheet::LoadForImage(base, sheet, error);
    if (!loaded) return fail(error);
    const auto imagePath = sheet.ImagePath(base);
    if (guid.Valid()) {
        const auto relativeBase = relative.substr(0, relative.size() - suffix.size());
        database.AddDependency(guid, database.GuidOf(relativeBase));
        database.AddDependency(guid, database.GuidOf(sheet.ImagePath(relativeBase)));
    }
    observed = stamp(imagePath);
    entry.ImagePath = imagePath;
    if (vfs::Mounted() && vfs::PackedKey(imagePath).empty()) return fail("image is absent from the mounted package");
    const auto imageStamp = Signature(imagePath);
    auto image = entry.Good && entry.ImageSignature == imageStamp ? entry.Good->Image : nullptr;
    if (!image) {
        auto decoded = Decode(imagePath);
        if (!decoded) {
            if (!entry.Good) entry.Metadata = std::make_shared<SpriteGeneration>(
                SpriteGeneration{++m_generation, std::move(sheet), {}});
            return fail("cannot decode image: " + imagePath);
        }
        decoded->Generation = ++m_generation;
        image = std::move(decoded);
    }
    if (!sheet.Validate(error, image->Width, image->Height)) return fail(error);
    if (observed != stamp(imagePath) || documentStamp != Signature(document) ||
        legacyStamp != Signature(NineSlice::SidecarPath(base))) return false; // Changed during reading; retry on the next poll.
    entry.Signature = std::move(observed);
    entry.ImageSignature = imageStamp;
    if (entry.Good && entry.Good->Image != image) entry.Previous = entry.Good;
    entry.Good = std::make_shared<SpriteGeneration>(SpriteGeneration{++m_generation, std::move(sheet),
                                                                    std::move(image)});
    entry.Diagnostic.clear();
    return true;
}

ResolvedSprite SpriteResolver::Resolve(AssetDatabase& database, SpriteRef ref, AssetGuid dependent, bool refresh) {
    if (!ref.Valid()) return {{}, ref, "invalid sprite reference"};
    if (dependent.Valid()) database.AddDependency(dependent, ref.Asset);
    auto result = ResolveSheet(database, ref.Asset, false, refresh);
    result.Ref = ref;
    if (!result && result.Diagnostic.empty()) result.Diagnostic = "sprite ID is absent: " + ref.Item.ToString();
    return result;
}

ResolvedSprite SpriteResolver::ResolveSheet(AssetDatabase& database, AssetGuid guid, bool allowMetadataOnly, bool refresh) {
    if (!guid.Valid()) return {};
    auto& entry = m_entries[guid.ToString()];
    entry.Guid = guid;
    entry.Tick = ++m_tick;
    if (refresh || entry.Signature.empty()) Refresh(database, entry);
    ResolvedSprite result{entry.Good ? entry.Good : (allowMetadataOnly ? entry.Metadata : nullptr), {guid, {}, SpriteRef::Kind::Region}, entry.Diagnostic};
    Trim();
    return result;
}

ResolvedSprite SpriteResolver::ResolveImage(AssetDatabase& database, const std::string& image, bool allowMissingSheet) {
    if (image.empty() || image.rfind("rt:", 0) == 0) return {};
    auto guid = database.GuidOf(SpriteSheet::SidecarPath(image));
    const auto real = database.LocatePath(image);
    const auto path = PathFromUtf8(real).lexically_normal();
    if (!guid.Valid() && path.is_absolute() && !database.ProjectDir().empty()) {
        std::error_code ec;
        const auto root = std::filesystem::absolute(PathFromUtf8(database.ProjectDir()), ec).lexically_normal();
        if (!ec) guid = database.GuidOf(SpriteSheet::SidecarPath(PathToUtf8(path.lexically_relative(root))));
    }
    if (guid.Valid()) return ResolveSheet(database, guid, true);
    const auto packed = vfs::PackedKey(image);
    const auto source = packed.empty() ? PathToUtf8(path) : packed;
    auto& entry = m_entries["image:" + source];
    entry.Source = source;
    entry.Tick = ++m_tick;
    Refresh(database, entry);
    const bool available = entry.HasDocument || allowMissingSheet;
    ResolvedSprite result{available ? (entry.Good ? entry.Good : entry.Metadata) : nullptr, {}, entry.Diagnostic};
    Trim();
    return result;
}

ResolvedSprite SpriteResolver::ResolveFrames(AssetDatabase& database, const std::vector<std::string>& frames,
                                              int maxSide) {
    if (frames.empty() || frames.size() > kMaxGridAtlasFrames || maxSide <= 0) return {};
    std::string key = "frames:" + std::to_string(maxSide);
    for (const auto& path : frames) key += ":" + std::to_string(path.size()) + ":" + path;
    auto [it, inserted] = m_entries.try_emplace(std::move(key));
    auto& entry = it->second;
    entry.Tick = ++m_tick;
    if (inserted) {
        entry.Frames = frames;
        entry.MaxSide = maxSide;
    }
    if (entry.FrameSignatures.empty()) RefreshFrames(database, entry);
    ResolvedSprite result{entry.Good, {}, entry.Diagnostic};
    Trim();
    return result;
}

bool SpriteResolver::RefreshFrames(AssetDatabase& database, Entry& entry) {
    const auto locate = [&](const std::string& path) {
        return path.empty() ? std::string{} : database.LocatePath(path);
    };
    // A list can contain thousands of files. Normal polling checks one member.
    if (!entry.FrameSignatures.empty()) {
        const size_t i = entry.FrameCursor++ % entry.Frames.size();
        if (entry.FrameSignatures[i] == Signature(locate(entry.Frames[i]))) return false;
    }
    std::vector<std::string> paths;
    paths.reserve(entry.Frames.size());
    for (const auto& path : entry.Frames) paths.push_back(locate(path));
    const auto stamp = [&] {
        std::vector<std::string> signatures;
        signatures.reserve(paths.size());
        for (const auto& path : paths) signatures.push_back(Signature(path));
        return signatures;
    };
    const auto observed = stamp();
    const auto fail = [&](const char* message) {
        entry.FrameSignatures = observed;
        entry.Diagnostic = entry.Frames.front() + ": " + message;
        LOG_WARN("Sprites") << entry.Diagnostic;
        return false;
    };
    std::vector<glm::ivec2> sizes(paths.size(), glm::ivec2(0));
    glm::ivec2 cell(0);
    GridAtlasLayout layout;
    for (size_t i = 0; i < paths.size(); ++i) {
        std::vector<uint8_t> bytes;
        int channels = 0;
        auto& size = sizes[i];
        image_memory::Scope memory(kFrameDecoderBytes);
        if (paths[i].empty() || !vfs::ReadFile(paths[i], bytes, kFrameInputBytes) || bytes.empty() ||
            !stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &size.x, &size.y, &channels) ||
            size.x <= 0 || size.y <= 0) {
            size = {};
            continue;
        }
        cell = glm::max(cell, size);
        if (!PlanGridAtlas(paths.size(), cell.x, cell.y, entry.MaxSide, kGridAtlasBudget, layout))
            return fail("standalone frames exceed the atlas budget or dimensions");
    }
    if (!layout.Bytes) return fail("no readable standalone frames");
    auto image = std::make_shared<SpriteImage>();
    image->Width = layout.Width;
    image->Height = layout.Height;
    image->Pixels.resize(layout.Bytes, 0);
    size_t decoded = 0;
    for (size_t i = 0; i < paths.size(); ++i) {
        if (!sizes[i].x) continue;
        const auto frame = Decode(paths[i], sizes[i]);
        if (!frame) continue;
        const int x = (static_cast<int>(i) % layout.Columns) * cell.x;
        const int y = (static_cast<int>(i) / layout.Columns) * cell.y + cell.y - frame->Height;
        SpriteSheet::CopyAtlasPixels(image->Pixels, image->Width, {x, y}, frame->Pixels.data(),
                                     frame->Width, frame->Height);
        ++decoded;
    }
    if (!decoded) return fail("cannot decode standalone frames");
    if (stamp() != observed) return false;
    image->Generation = ++m_generation;
    entry.Good = std::make_shared<SpriteGeneration>(SpriteGeneration{
        ++m_generation, {}, std::move(image), {layout.Columns, layout.Rows}});
    entry.FrameSignatures = observed;
    entry.Diagnostic.clear();
    if (decoded != paths.size()) {
        entry.Diagnostic = entry.Frames.front() + ": " + std::to_string(paths.size() - decoded) +
                           " unreadable standalone frames; their cells are transparent";
        LOG_WARN("Sprites") << entry.Diagnostic;
    }
    return true;
}

ResolvedSprite SpriteResolver::Previous(SpriteRef ref) const {
    const auto entry = m_entries.find(ref.Asset.ToString());
    return {entry == m_entries.end() ? nullptr : entry->second.Previous, ref, {}};
}

size_t SpriteResolver::Poll(AssetDatabase& database, size_t budget) {
    if (m_entries.empty()) return 0;
    const size_t size = m_entries.size(), count = budget ? std::min(budget, size) : size;
    size_t i = 0, changed = 0;
    for (auto& pair : m_entries)
        if ((i++ + size - m_cursor % size) % size < count && Refresh(database, pair.second)) ++changed;
    m_cursor = (m_cursor + count) % size;
    Trim();
    return changed;
}

void SpriteResolver::Trim() {
    const auto sizeOf = [](const Entry& entry) {
        const size_t current = entry.Good ? entry.Good->Image->Pixels.size() : 0;
        return current + (entry.Previous ? entry.Previous->Image->Pixels.size() : 0);
    };
    size_t bytes = 0;
    for (const auto& pair : m_entries) bytes += sizeOf(pair.second);
    while (m_entries.size() > 64 || bytes > 2 * kMaxImageBytes) {
        const auto oldest = std::min_element(m_entries.begin(), m_entries.end(), [](const auto& a, const auto& b) {
            return a.second.Tick < b.second.Tick;
        });
        bytes -= sizeOf(oldest->second);
        m_entries.erase(oldest);
    }
}

void SpriteResolver::NotifyChanged(AssetGuid guid) {
    if (auto it = m_entries.find(guid.ToString()); it != m_entries.end()) it->second.Signature.clear();
}

void SpriteResolver::Clear() {
    m_entries.clear();
    m_cursor = 0;
}

} // namespace sage::assets
