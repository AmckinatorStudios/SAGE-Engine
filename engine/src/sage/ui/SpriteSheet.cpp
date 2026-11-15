#include "sage/ui/SpriteSheet.h"

#include "sage/core/AtomicFile.h"
#include <cstring>
#include <cmath>
#include <cctype>

#include <algorithm>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "sage/assets/Pack.h"
#include "sage/core/Paths.h"

namespace sage::ui {

const SpriteRegion* SpriteSheet::Find(const std::string& name) const {
    for (const SpriteRegion& r : Regions)
        if (r.Name == name) return &r;
    return nullptr;
}

SpriteRegion* SpriteSheet::Find(const std::string& name) {
    for (SpriteRegion& r : Regions)
        if (r.Name == name) return &r;
    return nullptr;
}

void SpriteSheet::Set(const SpriteRegion& region) {
    if (SpriteRegion* r = Find(region.Name)) *r = region;
    else Regions.push_back(region);
}

bool SpriteSheet::Remove(const std::string& name) {
    auto it = std::find_if(Regions.begin(), Regions.end(), [&](const SpriteRegion& r) { return r.Name == name; });
    if (it == Regions.end()) return false;
    Regions.erase(it);
    return true;
}

bool SpriteSheet::Rename(const std::string& from, const std::string& to) {
    // Имя — ключ: два куска с одним именем сделали бы «взять по имени»
    // неоднозначным.
    if (to.empty() || Find(to)) return false;
    SpriteRegion* r = Find(from);
    if (!r) return false;
    r->Name = to;
    return true;
}


std::vector<GridCell> GridCells(int width, int height, const SpriteGrid& g, const unsigned char* rgba) {
    std::vector<GridCell> out;
    if (g.CellW <= 0 || g.CellH <= 0 || width <= 0 || height <= 0) return out;
    // Область нарезки — картинка за вычетом полей; сдвиг двигает сетку в ней.
    const int x0 = g.Margin.x + g.OffX, y0 = g.Margin.y + g.OffY;
    const int x1 = width - g.Margin.z, y1 = height - g.Margin.w;
    const int stepX = g.CellW + std::max(0, g.PadX), stepY = g.CellH + std::max(0, g.PadY);
    // Предел на число клеток: сетка 1x1 по картинке 4096 — шестнадцать
    // миллионов рамок, и редактор встал бы.
    for (int y = y0; y < y1 && out.size() < 20000; y += stepY) {
        for (int x = x0; x < x1 && out.size() < 20000; x += stepX) {
            GridCell c;
            c.Rect = {x, y, g.CellW, g.CellH};
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
    if (!rgba || width <= 0 || height <= 0) return boxes;
    const unsigned char cut = (unsigned char)std::clamp(p.AlphaCutoff * 255.0f, 1.0f, 255.0f);
    std::vector<unsigned char> seen((size_t)width * height, 0);
    std::vector<int> stack;
    auto alpha = [&](int x, int y) { return rgba[((size_t)y * width + x) * 4 + 3]; };
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t i = (size_t)y * width + x;
            if (seen[i] || alpha(x, y) < cut) continue;
            // Заливка острова: стек, а не рекурсия — остров бывает во весь лист.
            int x0 = x, y0 = y, x1 = x, y1 = y;
            seen[i] = 1;
            stack.assign(1, (int)i);
            while (!stack.empty()) {
                const int c = stack.back();
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
                    stack.push_back((int)j);
                }
            }
            const int w = x1 - x0 + 1, h = y1 - y0 + 1;
            if (w < p.MinW || h < p.MinH) continue;
            const int pad = std::max(0, p.Padding);
            const int bx = std::max(0, x0 - pad), by = std::max(0, y0 - pad);
            boxes.push_back({bx, by, std::min(width, x1 + 1 + pad) - bx, std::min(height, y1 + 1 + pad) - by});
        }
    }
    // Сверху вниз, слева направо.
    std::sort(boxes.begin(), boxes.end(), [](const glm::ivec4& a, const glm::ivec4& b) {
        return a.y != b.y ? a.y < b.y : a.x < b.x;
    });
    return boxes;
}

std::string SpriteSheet::FreeName(const std::string& prefix) const {
    for (int i = (int)Regions.size();; ++i) {
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
        Regions.push_back(r);
        ++added;
    }
    return added;
}

namespace {
const char* FillName(SliceFill f) { return f == SliceFill::Tile ? "tile" : "stretch"; }
SliceFill FillFrom(const nlohmann::json& j, const char* key) {
    return j.value(key, std::string("stretch")) == "tile" ? SliceFill::Tile : SliceFill::Stretch;
}
const char* const kModes[] = {"grid", "auto", "manual"};
} // namespace

// ВЕРСИЯ 2: у куска — точка опоры и девятина целиком; у листа — чем резали.
std::string SpriteSheet::ToJsonString() const {
    nlohmann::json j;
    j["version"] = 2;
    const SpriteGrid& g = Slicing.Grid;
    const SpriteAuto& a = Slicing.Auto;
    j["slicing"] = {{"mode", kModes[(int)Slicing.Method]},
                    {"cellSize", {g.CellW, g.CellH}},
                    {"offset", {g.OffX, g.OffY}},
                    {"spacing", {g.PadX, g.PadY}},
                    {"margin", {g.Margin.x, g.Margin.y, g.Margin.z, g.Margin.w}},
                    {"prefix", g.Prefix},
                    {"skipEmpty", g.SkipEmpty},
                    {"auto", {{"alpha", a.AlphaCutoff}, {"minSize", {a.MinW, a.MinH}}, {"padding", a.Padding},
                              {"prefix", a.Prefix}}}};
    j["sprites"] = nlohmann::json::array();
    for (const SpriteRegion& r : Regions) {
        nlohmann::json e = {{"name", r.Name}, {"rect", {r.Rect.x, r.Rect.y, r.Rect.z, r.Rect.w}}};
        e["pivot"] = {r.Pivot.x, r.Pivot.y};
        e["nineSlice"] = {{"border", {r.Slice.x, r.Slice.y, r.Slice.z, r.Slice.w}},
                          {"edgeFill", FillName(r.EdgeFill)},
                          {"centerFill", FillName(r.CenterFill)},
                          {"drawCenter", r.DrawCenter}};
        j["sprites"].push_back(e);
    }
    if (!Sequences.empty()) {
        j["sequences"] = nlohmann::json::array();
        for (const SpriteSequence& q : Sequences)
            j["sequences"].push_back({{"name", q.Name}, {"frames", q.Frames}, {"fps", q.Fps}, {"loop", q.Loop}});
    }
    return j.dump(2);
}

bool SpriteSheet::FromJsonString(const std::string& text, SpriteSheet& out, std::string& err) {
    nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        err = "лист спрайтов: не JSON";
        return false;
    }
    out.Regions.clear();
    const nlohmann::json list = j.value("sprites", nlohmann::json::array());
    if (!list.is_array()) {
        err = "лист спрайтов: поле sprites — не список";
        return false;
    }
    for (const nlohmann::json& e : list) {
        if (!e.is_object()) continue;
        SpriteRegion r;
        r.Name = e.value("name", std::string());
        const nlohmann::json rect = e.value("rect", nlohmann::json::array());
        if (r.Name.empty() || !rect.is_array() || rect.size() != 4) continue;  // битая строка — пропуск
        for (int k = 0; k < 4; ++k) r.Rect[k] = rect[k].is_number() ? rect[k].get<int>() : 0;
        const nlohmann::json slice = e.value("slice", nlohmann::json::array());
        if (slice.is_array() && slice.size() == 4)
            for (int k = 0; k < 4; ++k) r.Slice[k] = slice[k].is_number() ? slice[k].get<float>() : 0.0f;
        // Версия 2: девятина целиком; версия 1 — только «slice» (миграция:
        // заливки и середина — по умолчанию).
        if (e.contains("nineSlice") && e["nineSlice"].is_object()) {
            const nlohmann::json& n = e["nineSlice"];
            const nlohmann::json b = n.value("border", nlohmann::json::array());
            if (b.is_array() && b.size() == 4)
                for (int k = 0; k < 4; ++k) r.Slice[k] = b[k].is_number() ? b[k].get<float>() : 0.0f;
            r.EdgeFill = FillFrom(n, "edgeFill");
            r.CenterFill = FillFrom(n, "centerFill");
            r.DrawCenter = n.value("drawCenter", true);
        }
        const nlohmann::json pivot = e.value("pivot", nlohmann::json::array());
        if (pivot.is_array() && pivot.size() == 2 && pivot[0].is_number() && pivot[1].is_number())
            r.Pivot = {pivot[0].get<float>(), pivot[1].get<float>()};
        out.Set(r);
    }
    out.Slicing = SpriteSlicing();
    if (j.contains("slicing") && j["slicing"].is_object()) {
        const nlohmann::json& sl = j["slicing"];
        const std::string mode = sl.value("mode", std::string("grid"));
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
    out.Sequences.clear();
    const nlohmann::json seqs = j.value("sequences", nlohmann::json::array());
    if (seqs.is_array())
        for (const nlohmann::json& e : seqs) {
            if (!e.is_object()) continue;
            SpriteSequence q;
            q.Name = e.value("name", std::string());
            if (q.Name.empty()) continue;
            const nlohmann::json frames = e.value("frames", nlohmann::json::array());
            if (frames.is_array())
                for (const nlohmann::json& f : frames)
                    if (f.is_string()) q.Frames.push_back(f.get<std::string>());
            q.Fps = e.value("fps", q.Fps);
            q.Loop = e.value("loop", q.Loop);
            out.Sequences.push_back(q);
        }
    return true;
}

bool SpriteSheet::SaveFile(const std::string& path, std::string& err) const {
    return sage::WriteFileAtomic(path, ToJsonString(), err);
}

bool SpriteSheet::LoadFile(const std::string& path, SpriteSheet& out, std::string& err) {
    std::string text;   // через vfs — описание едет в пакете рядом с картинкой
    if (!sage::assets::vfs::ReadText(path, text)) {
        err = "нет файла: " + path;
        return false;
    }
    return FromJsonString(text, out, err);
}

std::string SpriteSequence::FrameAt(float time) const {
    if (Frames.empty()) return {};
    const int n = (int)Frames.size();
    int i = (int)std::floor(std::max(time, 0.0f) * std::max(Fps, 0.001f));
    i = Loop ? i % n : std::min(i, n - 1);
    return Frames[(size_t)i];
}

const SpriteSequence* SpriteSheet::FindSequence(const std::string& name) const {
    for (const SpriteSequence& q : Sequences)
        if (q.Name == name) return &q;
    return nullptr;
}

namespace {
// Число в конце имени («run_12» → 12); без числа — -1.
long TrailingNumber(const std::string& s) {
    size_t i = s.size();
    while (i > 0 && std::isdigit((unsigned char)s[i - 1])) --i;
    return i == s.size() ? -1 : std::stol(s.substr(i));
}
} // namespace

int SpriteSheet::SequenceFromPrefix(const std::string& name, const std::string& prefix, float fps) {
    std::vector<const SpriteRegion*> found;
    for (const SpriteRegion& r : Regions)
        if (r.Name.rfind(prefix, 0) == 0) found.push_back(&r);
    std::stable_sort(found.begin(), found.end(), [](const SpriteRegion* a, const SpriteRegion* b) {
        const long na = TrailingNumber(a->Name), nb = TrailingNumber(b->Name);
        return na != nb ? na < nb : a->Name < b->Name;
    });
    SpriteSequence q;
    q.Name = name;
    q.Fps = fps;
    for (const SpriteRegion* r : found) q.Frames.push_back(r->Name);
    for (SpriteSequence& old : Sequences)
        if (old.Name == name) {
            old = q;
            return (int)q.Frames.size();
        }
    Sequences.push_back(q);
    return (int)q.Frames.size();
}

bool SpriteSheet::PackAtlas(const std::vector<AtlasInput>& images, int padding, int maxSide,
                            std::vector<unsigned char>& rgba, int& width, int& height, SpriteSheet& sheet,
                            std::string& err) {
    if (images.empty()) {
        err = "нечего упаковывать";
        return false;
    }
    // Выше — раньше: полки тогда заполняются плотнее.
    std::vector<size_t> order(images.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(),
                     [&](size_t a, size_t b) { return images[a].Height > images[b].Height; });
    std::vector<glm::ivec2> at(images.size());
    for (int side = 64; side <= maxSide; side *= 2) {
        int x = padding, y = padding, shelf = 0;
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
            at[k] = {x, y};
            x += im.Width + padding;
            shelf = std::max(shelf, im.Height);
        }
        if (!fits) continue;
        width = height = side;
        rgba.assign((size_t)side * (size_t)side * 4, 0);
        sheet = SpriteSheet();
        for (size_t k = 0; k < images.size(); ++k) {
            const AtlasInput& im = images[k];
            for (int row = 0; row < im.Height; ++row)
                std::memcpy(&rgba[(((size_t)(at[k].y + row)) * (size_t)side + (size_t)at[k].x) * 4],
                            &im.Rgba[(size_t)row * (size_t)im.Width * 4], (size_t)im.Width * 4);
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

std::string SpriteSheet::SidecarPath(const std::string& imagePath) {
    return imagePath.empty() ? std::string() : imagePath + ".sagesheet";
}

} // namespace sage::ui
