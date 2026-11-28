#include "sage/assets/SpriteSheet.h"

#include "sage/core/AtomicFile.h"
#include <cstring>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

#include <algorithm>

#include <nlohmann/json.hpp>

#include "sage/assets/Pack.h"
#include "sage/core/Paths.h"

namespace sage::assets {

const SpriteRegion* SpriteSheet::Find(const std::string& name) const {
    for (const SpriteRegion& r : m_regions)
        if (r.Name == name) return &r;
    return nullptr;
}

SpriteRegion* SpriteSheet::FindMutable(const std::string& name) {
    for (SpriteRegion& r : m_regions)
        if (r.Name == name) return &r;
    return nullptr;
}

void SpriteSheet::Set(const SpriteRegion& region) {
    if (SpriteRegion* r = FindMutable(region.Name)) {
        const AssetGuid id = r->Id();
        *r = region;
        r->m_id = id;
    } else {
        m_regions.push_back(region);
        m_regions.back().m_id = AssetGuid::Generate();
    }
}

const SpriteRegion* SpriteSheet::Find(AssetGuid id) const {
    for (const SpriteRegion& r : m_regions)
        if (r.Id() == id) return &r;
    return nullptr;
}

const SpriteSequence* SpriteSheet::FindSequence(AssetGuid id) const {
    for (const SpriteSequence& q : m_sequences)
        if (q.Id() == id) return &q;
    return nullptr;
}

bool SpriteSheet::Remove(const std::string& name) {
    const SpriteRegion* region = Find(name);
    if (!region) return false;
    const AssetGuid id = region->Id();
    for (SpriteSequence& q : m_sequences) {
        for (size_t i = q.m_frames.size(); i-- > 0;) {
            if (q.m_frames[i] != id) continue;
            q.m_frames.erase(q.m_frames.begin() + i);
            q.Frames.erase(q.Frames.begin() + i);
        }
    }
    m_regions.erase(m_regions.begin() + (region - m_regions.data()));
    return true;
}

bool SpriteSheet::Rename(const std::string& from, const std::string& to) {
    if (to.empty() || Find(to)) return false;
    SpriteRegion* r = FindMutable(from);
    if (!r) return false;
    for (SpriteSequence& q : m_sequences)
        for (size_t i = 0; i < q.m_frames.size(); ++i)
            if (q.m_frames[i] == r->Id()) q.Frames[i] = to;
    r->Name = to;
    return true;
}

void SpriteSheet::SetRect(size_t index, glm::ivec4 rect) { m_regions.at(index).Rect = rect; }
void SpriteSheet::SetPivot(size_t index, glm::vec2 pivot) { m_regions.at(index).Pivot = pivot; }
void SpriteSheet::SetSlice(size_t index, const NineSlice& slice) {
    SpriteRegion& r = m_regions.at(index);
    r.Slice = slice.Border();
    r.CenterFill = slice.CenterFill;
    r.EdgeFill = slice.EdgeFill;
    r.DrawCenter = slice.DrawCenter;
}
void SpriteSheet::SetPlayback(size_t index, float fps, bool loop) {
    SpriteSequence& q = m_sequences.at(index);
    q.Fps = fps;
    q.Loop = loop;
}
void SpriteSheet::RemoveSequence(size_t index) {
    if (index < m_sequences.size()) m_sequences.erase(m_sequences.begin() + index);
}

int SpriteSheet::ApplySlices(const std::vector<glm::ivec4>& rects, const std::string& prefix, bool replace) {
    if (replace) {
        for (size_t i = m_regions.size(); i-- > 0;)
            if (std::find(rects.begin(), rects.end(), m_regions[i].Rect) == rects.end())
                Remove(m_regions[i].Name);
    }
    int added = 0;
    for (const auto& rect : rects) {
        if (std::any_of(m_regions.begin(), m_regions.end(),
                        [&](const SpriteRegion& r) { return r.Rect == rect; })) continue;
        Set(SpriteRegion(FreeName(prefix.empty() ? "sprite" : prefix), rect));
        ++added;
    }
    return added;
}


std::vector<GridCell> GridCells(int width, int height, const SpriteGrid& g, const unsigned char* rgba) {
    std::vector<GridCell> out;
    if (g.CellW <= 0 || g.CellH <= 0 || width <= 0 || height <= 0) return out;
    // Область нарезки — картинка за вычетом полей; сдвиг двигает сетку в ней.
    if (glm::any(glm::lessThan(g.Margin, glm::ivec4(0)))) return out;
    const int64_t x0 = int64_t(g.Margin.x) + g.OffX, y0 = int64_t(g.Margin.y) + g.OffY;
    const int64_t x1 = int64_t(width) - g.Margin.z, y1 = int64_t(height) - g.Margin.w;
    const int64_t stepX = int64_t(g.CellW) + std::max(0, g.PadX);
    const int64_t stepY = int64_t(g.CellH) + std::max(0, g.PadY);
    if (x0 >= x1 || y0 >= y1) return out;
    // Предел на число клеток: сетка 1x1 по картинке 4096 — шестнадцать
    // миллионов рамок, и редактор встал бы.
    for (int64_t y = y0; y < y1 && out.size() < 20000; y += stepY) {
        for (int64_t x = x0; x < x1 && out.size() < 20000; x += stepX) {
            GridCell c;
            c.Rect = {static_cast<int>(x), static_cast<int>(y), g.CellW, g.CellH};
            c.Inside = x >= 0 && y >= 0 && x + g.CellW <= x1 && y + g.CellH <= y1;
            if (c.Inside && rgba) {
                c.Empty = true;
                for (int yy = y; yy < y + g.CellH && c.Empty; ++yy)
                    for (int xx = x; xx < x + g.CellW; ++xx)
                        if (rgba[((size_t)yy * width + xx) * 4 + 3] > 0) { c.Empty = false; break; }
            }
            out.push_back(c);
        }
    }
    return out;
}

std::vector<glm::ivec4> FindIslands(const unsigned char* rgba, int width, int height, const SpriteAuto& p) {
    std::vector<glm::ivec4> boxes;
    if (!rgba || width <= 0 || height <= 0 || !std::isfinite(p.AlphaCutoff)) return boxes;
    const unsigned char cut = (unsigned char)std::clamp(p.AlphaCutoff * 255.0f, 1.0f, 255.0f);
    std::vector<unsigned char> seen((size_t)width * height, 0);
    std::vector<size_t> stack;
    auto alpha = [&](int x, int y) { return rgba[((size_t)y * width + x) * 4 + 3]; };
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t i = (size_t)y * width + x;
            if (seen[i] || alpha(x, y) < cut) continue;
            // Заливка острова: стек, а не рекурсия — остров бывает во весь лист.
            int x0 = x, y0 = y, x1 = x, y1 = y;
            seen[i] = 1;
            stack.assign(1, i);
            while (!stack.empty()) {
                const size_t c = stack.back();
                stack.pop_back();
                const int cx = c % width, cy = c / width;
                x0 = std::min(x0, cx); x1 = std::max(x1, cx);
                y0 = std::min(y0, cy); y1 = std::max(y1, cy);
                const int nx[4] = {cx - 1, cx + 1, cx, cx};
                const int ny[4] = {cy, cy, cy - 1, cy + 1};
                for (int k = 0; k < 4; ++k) {
                    if (nx[k] < 0 || ny[k] < 0 || nx[k] >= width || ny[k] >= height) continue;
                    const size_t j = (size_t)ny[k] * width + nx[k];
                    if (seen[j] || alpha(nx[k], ny[k]) < cut) continue;
                    seen[j] = 1;
                    stack.push_back(j);
                }
            }
            const int w = x1 - x0 + 1, h = y1 - y0 + 1;
            if (w < p.MinW || h < p.MinH) continue;
            const int pad = std::max(0, p.Padding);
            const int bx = std::max(0, x0 - pad), by = std::max(0, y0 - pad);
            const int right = static_cast<int>(std::min<int64_t>(width, int64_t(x1) + 1 + pad));
            const int bottom = static_cast<int>(std::min<int64_t>(height, int64_t(y1) + 1 + pad));
            boxes.push_back({bx, by, right - bx, bottom - by});
        }
    }
    // Сверху вниз, слева направо.
    std::sort(boxes.begin(), boxes.end(), [](const glm::ivec4& a, const glm::ivec4& b) {
        return a.y != b.y ? a.y < b.y : a.x < b.x;
    });
    return boxes;
}

std::string SpriteSheet::FreeName(const std::string& prefix) const {
    for (int i = (int)m_regions.size();; ++i) {
        std::string name = prefix + "_" + std::to_string(i);
        if (!Find(name)) {
            // Первый свободный номер — с нуля, если он свободен.
            for (int k = 0; k < i; ++k) {
                const std::string low = prefix + "_" + std::to_string(k);
                if (!Find(low)) return low;
            }
            return name;
        }
    }
}

namespace {
// Строки снизу вверх (картинка из текстуры) — в строки сверху вниз.
std::vector<unsigned char> TopDown(const unsigned char* rgba, int w, int h, bool bottomUp) {
    std::vector<unsigned char> out(rgba, rgba + (size_t)w * h * 4);
    if (bottomUp)
        for (int y = 0; y < h; ++y)
            std::memcpy(&out[(size_t)y * w * 4], &rgba[(size_t)(h - 1 - y) * w * 4], (size_t)w * 4);
    return out;
}
} // namespace

int SpriteSheet::SliceGrid(int width, int height, const SpriteGrid& g, const unsigned char* rgba,
                           bool rowsBottomUp) {
    if (width <= 0 || height <= 0) return 0;
    std::vector<unsigned char> td;
    if (rgba) td = TopDown(rgba, width, height, rowsBottomUp);
    int made = 0, index = 0;
    for (const GridCell& c : GridCells(width, height, g, rgba ? td.data() : nullptr)) {
        if (!c.Inside) continue;
        if (!(g.SkipEmpty && c.Empty)) {
            SpriteRegion r;
            r.Name = g.Prefix + "_" + std::to_string(index);
            r.Rect = c.Rect;
            Set(r);
            ++made;
        }
        ++index;
    }
    return made;
}

int SpriteSheet::SliceAuto(const unsigned char* rgba, int width, int height, bool rowsBottomUp,
                           float alphaCutoff, int minSize, const std::string& prefix) {
    if (!rgba || width <= 0 || height <= 0) return 0;
    const std::vector<unsigned char> td = TopDown(rgba, width, height, rowsBottomUp);
    SpriteAuto p;
    p.AlphaCutoff = alphaCutoff;
    p.MinW = p.MinH = 1;
    int added = 0;
    for (const glm::ivec4& b : FindIslands(td.data(), width, height, p)) {
        if (b.z < minSize && b.w < minSize) continue;
        SpriteRegion r;
        r.Name = FreeName(prefix);
        r.Rect = b;
        Set(r);
        ++added;
    }
    return added;
}

namespace {
const char* FillName(SliceFill f) { return f == SliceFill::Tile ? "tile" : "stretch"; }
const char* const kModes[] = {"grid", "auto", "manual"};
} // namespace

// ВЕРСИЯ 2: у куска — точка опоры и девятина целиком; у листа — чем резали.
std::string SpriteSheet::ToJsonString() const {
    nlohmann::json j;
    j["version"] = 3;
    if (!m_imageFile.empty()) j["image"] = m_imageFile;
    j["textureNineSlice"] = nlohmann::json::parse(TextureSlice.ToJsonString());
    const SpriteGrid& g = Slicing.Grid;
    const SpriteAuto& a = Slicing.Auto;
    const int mode = static_cast<int>(Slicing.Method);
    j["slicing"] = {{"mode", mode >= 0 && mode < 3 ? kModes[mode] : "invalid"},
                    {"cellSize", {g.CellW, g.CellH}},
                    {"offset", {g.OffX, g.OffY}},
                    {"spacing", {g.PadX, g.PadY}},
                    {"margin", {g.Margin.x, g.Margin.y, g.Margin.z, g.Margin.w}},
                    {"prefix", g.Prefix},
                    {"skipEmpty", g.SkipEmpty},
                    {"auto", {{"alpha", a.AlphaCutoff}, {"minSize", {a.MinW, a.MinH}}, {"padding", a.Padding},
                              {"prefix", a.Prefix}}}};
    j["sprites"] = nlohmann::json::array();
    for (const SpriteRegion& r : m_regions) {
        nlohmann::json e = {{"id", r.Id().ToString()}, {"name", r.Name}, {"rect", {r.Rect.x, r.Rect.y, r.Rect.z, r.Rect.w}}};
        e["pivot"] = {r.Pivot.x, r.Pivot.y};
        e["nineSlice"] = {{"border", {r.Slice.x, r.Slice.y, r.Slice.z, r.Slice.w}},
                          {"edgeFill", FillName(r.EdgeFill)},
                          {"centerFill", FillName(r.CenterFill)},
                          {"drawCenter", r.DrawCenter}};
        j["sprites"].push_back(e);
    }
    if (!m_sequences.empty()) {
        j["sequences"] = nlohmann::json::array();
        for (const SpriteSequence& q : m_sequences) {
            auto frames = nlohmann::json::array();
            for (AssetGuid id : q.m_frames) frames.push_back(id.ToString());
            j["sequences"].push_back({{"id", q.Id().ToString()}, {"name", q.Name}, {"frames", frames},
                                      {"fps", q.Fps}, {"loop", q.Loop}});
        }
    }
    return j.dump(2);
}

bool SpriteSheet::FromJsonString(const std::string& text, SpriteSheet& result, std::string& err) {
    int version = 0;
    return ParseDocument(text, result, err, version);
}

bool SpriteSheet::ParseDocument(const std::string& text, SpriteSheet& result, std::string& err, int& version) try {
    SpriteSheet out;
    nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        err = "лист спрайтов: не JSON";
        return false;
    }
    const auto format = j.value("version", nlohmann::json(1));
    if (!format.is_number_integer() || format < 1 || format > 3)
        throw std::runtime_error("unsupported sprite sheet version");
    version = format.get<int>();
    if (version == 3 && !out.SetImageFile(j.value("image", std::string{})))
        throw std::runtime_error("invalid sprite image filename");
    if (version == 3 && !NineSlice::FromJsonString(j.at("textureNineSlice").dump(), out.TextureSlice, err))
        return false;
    const nlohmann::json list = j.value("sprites", nlohmann::json::array());
    if (!list.is_array()) {
        err = "лист спрайтов: поле sprites — не список";
        return false;
    }
    for (const nlohmann::json& e : list) {
        if (!e.is_object()) throw std::runtime_error("invalid sprite region");
        SpriteRegion r;
        r.Name = e.value("name", std::string());
        const nlohmann::json rect = e.value("rect", nlohmann::json::array());
        if (!rect.is_array() || rect.size() != 4) throw std::runtime_error("invalid sprite rectangle");
        for (int k = 0; k < 4; ++k) {
            if (!rect[k].is_number_integer() || rect[k] < 0 || rect[k] > std::numeric_limits<int>::max())
                throw std::runtime_error("invalid sprite rectangle coordinate");
            r.Rect[k] = rect[k].get<int>();
        }
        NineSlice slice;
        if (e.contains("nineSlice")) {
            if (!NineSlice::FromJsonString(e["nineSlice"].dump(), slice, err)) return false;
        } else if (e.contains("slice")) {
            const nlohmann::json legacy = {{"border", e["slice"]}};
            if (!NineSlice::FromJsonString(legacy.dump(), slice, err)) return false;
        }
        r.Slice = slice.Border();
        r.EdgeFill = slice.EdgeFill;
        r.CenterFill = slice.CenterFill;
        r.DrawCenter = slice.DrawCenter;
        if (e.contains("pivot")) {
            const auto& pivot = e["pivot"];
            if (!pivot.is_array() || pivot.size() != 2)
                throw std::runtime_error("invalid sprite pivot");
            r.Pivot = {pivot[0].get<float>(), pivot[1].get<float>()};
        }
        r.m_id = version == 3 ? AssetGuid::FromString(e.at("id").get<std::string>())
                               : AssetGuid{1, out.m_regions.size() + 1};
        out.m_regions.push_back(std::move(r));
    }
    out.Slicing = SpriteSlicing();
    if (j.contains("slicing") && j["slicing"].is_object()) {
        const nlohmann::json& sl = j["slicing"];
        const std::string mode = sl.value("mode", std::string("grid"));
        if (mode != "grid" && mode != "auto" && mode != "manual")
            throw std::runtime_error("invalid sprite slicing mode");
        out.Slicing.Method = mode == "auto" ? SpriteSlicing::Mode::Automatic
                             : mode == "manual" ? SpriteSlicing::Mode::Manual
                                                : SpriteSlicing::Mode::Grid;
        SpriteGrid& g = out.Slicing.Grid;
        auto pair = [&](const char* key, int& a, int& b) {
            const nlohmann::json v = sl.value(key, nlohmann::json::array());
            if (v.is_array() && v.size() == 2 && v[0].is_number() && v[1].is_number()) {
                a = v[0].get<int>();
                b = v[1].get<int>();
            }
        };
        pair("cellSize", g.CellW, g.CellH);
        pair("offset", g.OffX, g.OffY);
        pair("spacing", g.PadX, g.PadY);
        const nlohmann::json m = sl.value("margin", nlohmann::json::array());
        if (m.is_array() && m.size() == 4)
            for (int k = 0; k < 4; ++k) g.Margin[k] = m[k].is_number() ? m[k].get<int>() : 0;
        g.Prefix = sl.value("prefix", g.Prefix);
        g.SkipEmpty = sl.value("skipEmpty", g.SkipEmpty);
        if (sl.contains("auto") && sl["auto"].is_object()) {
            const nlohmann::json& au = sl["auto"];
            SpriteAuto& a = out.Slicing.Auto;
            a.AlphaCutoff = au.value("alpha", a.AlphaCutoff);
            const nlohmann::json ms = au.value("minSize", nlohmann::json::array());
            if (ms.is_array() && ms.size() == 2 && ms[0].is_number() && ms[1].is_number()) {
                a.MinW = ms[0].get<int>();
                a.MinH = ms[1].get<int>();
            }
            a.Padding = au.value("padding", a.Padding);
            a.Prefix = au.value("prefix", a.Prefix);
        }
    }
    const auto seqs = j.value("sequences", nlohmann::json::array());
    if (!seqs.is_array()) throw std::runtime_error("invalid sprite sequences");
    for (const auto& e : seqs) {
        SpriteSequence q;
        q.Name = e.at("name").get<std::string>();
        q.m_id = version == 3 ? AssetGuid::FromString(e.at("id").get<std::string>())
                              : AssetGuid{2, out.m_sequences.size() + 1};
        const auto frames = e.value("frames", nlohmann::json::array());
        if (!frames.is_array()) throw std::runtime_error("invalid sprite frames");
        for (const auto& f : frames) {
            const auto key = f.get<std::string>();
            const SpriteRegion* region = version == 3 ? out.Find(AssetGuid::FromString(key)) : out.Find(key);
            if (!region) throw std::runtime_error("unknown frame '" + key + "' in '" + q.Name + "'");
            q.m_frames.push_back(region->Id());
            q.Frames.push_back(region->Name);
        }
        q.Fps = e.value("fps", q.Fps);
        q.Loop = e.value("loop", q.Loop);
        out.m_sequences.push_back(std::move(q));
    }
    if (!out.Validate(err)) return false;
    result = std::move(out);
    return true;
} catch (const std::exception& e) {
    err = e.what();
    return false;
}

bool SpriteSheet::Validate(std::string& err, int width, int height) const {
    err.clear();
    auto fail = [&](std::string message) { err = std::move(message); return false; };
    auto sliceValid = [](const NineSlice& n) {
        const auto fill = [](SliceFill f) { return f == SliceFill::Stretch || f == SliceFill::Tile; };
        const glm::vec4 b = n.Border();
        for (int i = 0; i < 4; ++i)
            if (!std::isfinite(b[i]) || b[i] < 0.0f) return false;
        return fill(n.CenterFill) && fill(n.EdgeFill);
    };
    if (!sliceValid(TextureSlice)) return fail("invalid texture nine-slice");
    std::unordered_set<AssetGuid, AssetGuidHash> ids;
    std::unordered_set<std::string> names;
    for (const SpriteRegion& r : m_regions) {
        if (r.Name.empty() || !names.insert(r.Name).second || !r.Id().Valid() || !ids.insert(r.Id()).second)
            return fail("duplicate or missing sprite identity: " + r.Name);
        const auto rect = r.Rect;
        const int64_t right = int64_t(rect.x) + rect.z, bottom = int64_t(rect.y) + rect.w;
        if (rect.x < 0 || rect.y < 0 || rect.z <= 0 || rect.w <= 0 ||
            right > std::numeric_limits<int>::max() || bottom > std::numeric_limits<int>::max() ||
            (width > 0 && right > width) || (height > 0 && bottom > height))
            return fail("invalid sprite rectangle: " + r.Name);
        for (int i = 0; i < 2; ++i)
            if (!std::isfinite(r.Pivot[i]) || r.Pivot[i] < 0.0f || r.Pivot[i] > 1.0f)
                return fail("invalid sprite pivot: " + r.Name);
        if (!sliceValid(r.NineSliceOf())) return fail("invalid sprite nine-slice: " + r.Name);
    }
    names.clear();
    for (const SpriteSequence& q : m_sequences) {
        if (q.Name.empty() || !names.insert(q.Name).second || !q.Id().Valid() || !ids.insert(q.Id()).second)
            return fail("duplicate or missing sequence identity: " + q.Name);
        if (!std::isfinite(q.Fps) || q.Fps <= 0.0f || q.Frames.size() != q.m_frames.size())
            return fail("invalid sprite sequence: " + q.Name);
        for (size_t i = 0; i < q.m_frames.size(); ++i) {
            const SpriteRegion* r = Find(q.m_frames[i]);
            if (!r || r->Name != q.Frames[i]) return fail("unknown frame in sequence: " + q.Name);
        }
    }
    const auto& g = Slicing.Grid;
    const auto& a = Slicing.Auto;
    if (int(Slicing.Method) < 0 || int(Slicing.Method) > 2 || g.CellW < 1 || g.CellH < 1 ||
        g.PadX < 0 || g.PadY < 0 || glm::any(glm::lessThan(g.Margin, glm::ivec4(0))) ||
        !std::isfinite(a.AlphaCutoff) || a.AlphaCutoff < 0 || a.AlphaCutoff > 1 ||
        a.MinW < 1 || a.MinH < 1 || a.Padding < 0)
        return fail("invalid sprite slicing settings");
    return true;
}

bool SpriteSheet::SaveFile(const std::string& path, std::string& err) const {
    return Validate(err) && sage::WriteFileAtomic(path, ToJsonString(), err);
}

bool SpriteSheet::LoadLegacyTextureSlice(const std::string& image, SpriteSheet& out, std::string& err) {
    const std::string path = NineSlice::SidecarPath(image);
    if (path.empty() || !vfs::Exists(path)) return true;
    std::string text;
    if (!vfs::ReadText(path, text)) {
        err = "cannot read nine-slice: " + path;
        return false;
    }
    return NineSlice::FromJsonString(text, out.TextureSlice, err);
}

bool SpriteSheet::LoadForImage(const std::string& image, SpriteSheet& result, std::string& err) {
    const std::string path = SidecarPath(image);
    if (!path.empty() && vfs::Exists(path)) return LoadFile(path, result, err);
    SpriteSheet sheet;
    if (!LoadLegacyTextureSlice(image, sheet, err)) return false;
    result = std::move(sheet);
    err.clear();
    return true;
}

bool SpriteSheet::LoadFile(const std::string& path, SpriteSheet& result, std::string& err) {
    std::string text;
    if (!vfs::ReadText(path, text)) {
        err = "cannot read sprite sheet: " + path;
        return false;
    }
    SpriteSheet sheet;
    int version = 0;
    if (!ParseDocument(text, sheet, err, version)) return false;
    constexpr std::string_view suffix = ".sagesheet";
    if (version < 3 && path.size() > suffix.size() &&
        path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0 &&
        !LoadLegacyTextureSlice(path.substr(0, path.size() - suffix.size()), sheet, err))
        return false;
    result = std::move(sheet);
    return true;
}

size_t SpriteSequence::FrameIndex(float time) const {
    if (Frames.empty() || !std::isfinite(time) || time <= 0 || !std::isfinite(Fps) || Fps <= 0) return 0;
    const double frame = std::floor(double(time) * Fps);
    const double count = double(Frames.size());
    return static_cast<size_t>(Loop ? std::fmod(frame, count) : std::min(frame, count - 1));
}

std::string SpriteSequence::FrameAt(float time) const {
    return Frames.empty() ? std::string{} : Frames[FrameIndex(time)];
}

AssetGuid SpriteSequence::FrameIdAt(float time) const {
    return m_frames.empty() ? AssetGuid{} : m_frames[FrameIndex(time)];
}

const SpriteSequence* SpriteSheet::FindSequence(const std::string& name) const {
    for (const SpriteSequence& q : m_sequences)
        if (q.Name == name) return &q;
    return nullptr;
}

namespace {
// Цифры в конце имени без ведущих нулей. Пустая строка сортируется первой.
std::string_view TrailingNumber(std::string_view name) {
    const size_t last = name.find_last_not_of("0123456789");
    auto suffix = name.substr(last == std::string_view::npos ? 0 : last + 1);
    while (suffix.size() > 1 && suffix.front() == '0') suffix.remove_prefix(1);
    return suffix;
}
} // namespace

int SpriteSheet::SequenceFromPrefix(const std::string& name, const std::string& prefix, float fps) {
    if (name.empty() || !std::isfinite(fps) || fps <= 0) return 0;
    std::vector<const SpriteRegion*> found;
    for (const SpriteRegion& r : m_regions)
        if (r.Name.rfind(prefix, 0) == 0) found.push_back(&r);
    std::stable_sort(found.begin(), found.end(), [](const SpriteRegion* a, const SpriteRegion* b) {
        const auto na = TrailingNumber(a->Name), nb = TrailingNumber(b->Name);
        if (na.size() != nb.size()) return na.size() < nb.size();
        return na != nb ? na < nb : a->Name < b->Name;
    });
    SpriteSequence q;
    q.Name = name;
    q.Fps = fps;
    for (const SpriteRegion* r : found) {
        q.Frames.push_back(r->Name);
        q.m_frames.push_back(r->Id());
    }
    for (SpriteSequence& old : m_sequences)
        if (old.Name == name) {
            q.m_id = old.Id();
            q.Loop = old.Loop;
            old = q;
            return (int)q.Frames.size();
        }
    q.m_id = AssetGuid::Generate();
    m_sequences.push_back(q);
    return (int)q.Frames.size();
}

void SpriteSheet::CopyAtlasPixels(std::vector<unsigned char>& atlas, int stride, glm::ivec2 position,
                                  const unsigned char* rgba, int width, int height) {
    for (int row = 0; row < height; ++row)
        std::memcpy(&atlas[(size_t(position.y + row) * stride + position.x) * 4],
                    &rgba[size_t(row) * width * 4], size_t(width) * 4);
}

bool SpriteSheet::PackAtlas(const std::vector<AtlasInput>& images, int padding, int maxSide,
                            std::vector<unsigned char>& rgba, int& width, int& height, SpriteSheet& sheet,
                            std::string& err) {
    if (images.empty()) {
        err = "нечего упаковывать";
        return false;
    }
    std::unordered_set<std::string> names;
    if (padding < 0 || maxSide < 64) {
        err = "invalid atlas dimensions or padding";
        return false;
    }
    for (const AtlasInput& image : images) {
        if (!image.Rgba || image.Name.empty() || !names.insert(image.Name).second ||
            image.Width <= 0 || image.Height <= 0 ||
            int64_t(image.Width) + int64_t(padding) * 2 > maxSide ||
            int64_t(image.Height) + int64_t(padding) * 2 > maxSide) {
            err = "invalid atlas image: " + image.Name;
            return false;
        }
    }
    // Выше — раньше: полки тогда заполняются плотнее.
    std::vector<size_t> order(images.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(),
                     [&](size_t a, size_t b) { return images[a].Height > images[b].Height; });
    std::vector<glm::ivec2> at(images.size());
    for (int64_t candidate = 64; candidate <= maxSide; candidate *= 2) {
        const int side = static_cast<int>(candidate);
        int64_t x = padding, y = padding;
        int shelf = 0;
        bool fits = true;
        for (size_t k : order) {
            const AtlasInput& im = images[k];
            if (im.Width + 2 * padding > side) { fits = false; break; }
            if (x + im.Width + padding > side) {
                x = padding;
                y += shelf + padding;
                shelf = 0;
            }
            if (y + im.Height + padding > side) { fits = false; break; }
            at[k] = {static_cast<int>(x), static_cast<int>(y)};
            x += im.Width + padding;
            shelf = std::max(shelf, im.Height);
        }
        if (!fits) continue;
        width = height = side;
        rgba.assign((size_t)side * (size_t)side * 4, 0);
        sheet = SpriteSheet();
        for (size_t k = 0; k < images.size(); ++k) {
            const AtlasInput& im = images[k];
            CopyAtlasPixels(rgba, side, at[k], im.Rgba, im.Width, im.Height);
            SpriteRegion r;
            r.Name = im.Name;
            r.Rect = {at[k].x, at[k].y, im.Width, im.Height};
            sheet.Set(r);
        }
        return true;
    }
    err = "картинки не помещаются в атлас " + std::to_string(maxSide) + "x" + std::to_string(maxSide);
    return false;
}

bool SpriteSheet::SetImageFile(std::string name) {
    if (name == "." || name == ".." || name.find_first_of("/\\:\0", 0, 4) != std::string::npos) return false;
    m_imageFile = std::move(name);
    return true;
}

std::string SpriteSheet::ImagePath(const std::string& baseImage) const {
    if (m_imageFile.empty()) return baseImage;
    return PathToUtf8(PathFromUtf8(baseImage).parent_path() / PathFromUtf8(m_imageFile));
}

std::string SpriteSheet::SidecarPath(const std::string& imagePath) {
    return imagePath.empty() ? std::string() : imagePath + ".sagesheet";
}

} // namespace sage::assets
