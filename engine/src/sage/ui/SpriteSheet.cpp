#include "sage/ui/SpriteSheet.h"
#include <cstring>
#include <cmath>
#include <cctype>

#include <algorithm>
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

namespace {
unsigned char AlphaAt(const unsigned char* rgba, int w, int h, bool bottomUp, int x, int y) {
    const int row = bottomUp ? (h - 1 - y) : y;
    return rgba[((size_t)row * w + x) * 4 + 3];
}
} // namespace

int SpriteSheet::SliceGrid(int width, int height, const SpriteGrid& g, const unsigned char* rgba,
                           bool rowsBottomUp) {
    if (g.CellW <= 0 || g.CellH <= 0 || width <= 0 || height <= 0) return 0;
    int made = 0, index = 0;
    for (int y = g.OffY; y + g.CellH <= height; y += g.CellH + g.PadY) {
        for (int x = g.OffX; x + g.CellW <= width; x += g.CellW + g.PadX) {
            bool empty = false;
            if (g.SkipEmpty && rgba) {
                empty = true;
                for (int yy = y; yy < y + g.CellH && empty; ++yy)
                    for (int xx = x; xx < x + g.CellW; ++xx)
                        if (AlphaAt(rgba, width, height, rowsBottomUp, xx, yy) > 0) { empty = false; break; }
            }
            if (!empty) {
                Set({g.Prefix + "_" + std::to_string(index), {x, y, g.CellW, g.CellH}, glm::vec4(0.0f)});
                ++made;
            }
            ++index;
        }
    }
    return made;
}

int SpriteSheet::SliceAuto(const unsigned char* rgba, int width, int height, bool rowsBottomUp,
                           float alphaCutoff, int minSize, const std::string& prefix) {
    if (!rgba || width <= 0 || height <= 0) return 0;
    const unsigned char cut = (unsigned char)std::clamp(alphaCutoff * 255.0f, 1.0f, 255.0f);
    std::vector<unsigned char> seen((size_t)width * height, 0);
    std::vector<glm::ivec4> boxes;
    std::vector<int> stack;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t i = (size_t)y * width + x;
            if (seen[i] || AlphaAt(rgba, width, height, rowsBottomUp, x, y) < cut) continue;
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
                    if (seen[j] || AlphaAt(rgba, width, height, rowsBottomUp, nx[k], ny[k]) < cut) continue;
                    seen[j] = 1;
                    stack.push_back((int)j);
                }
            }
            const int w = x1 - x0 + 1, h = y1 - y0 + 1;
            if (w < minSize && h < minSize) continue;
            boxes.push_back({x0, y0, w, h});
        }
    }
    // Сверху вниз, слева направо.
    std::sort(boxes.begin(), boxes.end(), [](const glm::ivec4& a, const glm::ivec4& b) {
        return a.y != b.y ? a.y < b.y : a.x < b.x;
    });
    int index = 0;
    for (const glm::ivec4& b : boxes) {
        std::string name;
        do { name = prefix + "_" + std::to_string(index++); } while (Find(name));
        Regions.push_back({name, b, glm::vec4(0.0f)});
    }
    return (int)boxes.size();
}

std::string SpriteSheet::ToJsonString() const {
    nlohmann::json j;
    j["sprites"] = nlohmann::json::array();
    for (const SpriteRegion& r : Regions) {
        nlohmann::json e = {{"name", r.Name}, {"rect", {r.Rect.x, r.Rect.y, r.Rect.z, r.Rect.w}}};
        if (r.Slice != glm::vec4(0.0f)) e["slice"] = {r.Slice.x, r.Slice.y, r.Slice.z, r.Slice.w};
        if (r.Pivot != glm::vec2(0.5f)) e["pivot"] = {r.Pivot.x, r.Pivot.y};
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
        const nlohmann::json pivot = e.value("pivot", nlohmann::json::array());
        if (pivot.is_array() && pivot.size() == 2 && pivot[0].is_number() && pivot[1].is_number())
            r.Pivot = {pivot[0].get<float>(), pivot[1].get<float>()};
        out.Set(r);
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
    std::ofstream file(sage::PathFromUtf8(path), std::ios::binary);
    if (!file) {
        err = "не удалось открыть на запись: " + path;
        return false;
    }
    file << ToJsonString();
    return true;
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
