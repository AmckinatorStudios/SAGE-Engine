#include "SpriteEditModel.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

#include "sage/assets/Pack.h"
#include "sage/core/Paths.h"

namespace sage::editor::sprite {

using sage::ui::NineSlice;
using sage::ui::SpriteRegion;
using sage::ui::SpriteSheet;

std::string Fingerprint(const SpriteDoc& doc) {
    return doc.Sheet.ToJsonString() + "\n" + doc.Texture.ToJsonString();
}

// --- История ---------------------------------------------------------------

namespace {
// Без предела история редактора съела бы память на большом листе: снимок —
// весь документ. Сто шагов — с запасом для работы руками.
constexpr size_t kHistoryLimit = 100;
} // namespace

void SpriteHistory::Push(const SpriteDoc& before) {
    m_undo.push_back(before);
    if (m_undo.size() > kHistoryLimit) m_undo.erase(m_undo.begin());
    m_redo.clear();
}

bool SpriteHistory::Undo(SpriteDoc& doc) {
    if (m_undo.empty()) return false;
    m_redo.push_back(doc);
    doc = std::move(m_undo.back());
    m_undo.pop_back();
    return true;
}

bool SpriteHistory::Redo(SpriteDoc& doc) {
    if (m_redo.empty()) return false;
    m_undo.push_back(doc);
    doc = std::move(m_redo.back());
    m_redo.pop_back();
    return true;
}

void SpriteHistory::Clear() {
    m_undo.clear();
    m_redo.clear();
}

// --- Нарезка ---------------------------------------------------------------

std::vector<glm::ivec4> GridRects(int width, int height, const sage::ui::SpriteGrid& grid,
                                  const unsigned char* rgba) {
    std::vector<glm::ivec4> out;
    for (const sage::ui::GridCell& c : sage::ui::GridCells(width, height, grid, rgba)) {
        if (!c.Inside) continue;
        if (grid.SkipEmpty && rgba && c.Empty) continue;
        out.push_back(c.Rect);
    }
    return out;
}

int ApplySlices(SpriteSheet& sheet, const std::vector<glm::ivec4>& rects, const std::string& prefix,
                SliceApply how) {
    if (how == SliceApply::Replace) sheet.Regions.clear();
    const std::string base = prefix.empty() ? std::string("sprite") : prefix;
    int added = 0;
    for (const glm::ivec4& r : rects) {
        // Добавление поверх готовой нарезки не плодит двойников: кусок с тем
        // же прямоугольником уже есть — у него своё имя, точка опоры и
        // девятина, и затирать их второй копией незачем.
        const bool twin = std::any_of(sheet.Regions.begin(), sheet.Regions.end(),
                                      [&](const SpriteRegion& e) { return e.Rect == r; });
        if (twin) continue;
        SpriteRegion reg;
        reg.Name = sheet.FreeName(base);
        reg.Rect = r;
        sheet.Regions.push_back(reg);
        ++added;
    }
    return added;
}

// --- Попадание и ручки --------------------------------------------------------

int HitTest(const SpriteSheet& sheet, glm::vec2 px) {
    // Сверху — последний заведённый, но из вложенных выигрывает меньший: иначе
    // кусок внутри крупной рамки не выбрать щелчком вовсе.
    int best = -1;
    long bestArea = 0;
    for (int i = (int)sheet.Regions.size() - 1; i >= 0; --i) {
        const glm::ivec4 r = sheet.Regions[(size_t)i].Rect;
        if (px.x < (float)r.x || px.y < (float)r.y || px.x >= (float)(r.x + r.z) || px.y >= (float)(r.y + r.w))
            continue;
        const long area = (long)r.z * (long)r.w;
        if (best < 0 || area < bestArea) {
            best = i;
            bestArea = area;
        }
    }
    return best;
}

std::vector<int> HitRect(const SpriteSheet& sheet, glm::ivec4 rect) {
    std::vector<int> out;
    for (int i = 0; i < (int)sheet.Regions.size(); ++i) {
        const glm::ivec4 r = sheet.Regions[(size_t)i].Rect;
        if (r.x < rect.x + rect.z && rect.x < r.x + r.z && r.y < rect.y + rect.w && rect.y < r.y + r.w)
            out.push_back(i);
    }
    return out;
}

glm::ivec4 RectFromCorners(glm::ivec2 a, glm::ivec2 b, int width, int height) {
    const glm::ivec2 hi0(std::max(0, width), std::max(0, height));
    a = glm::clamp(a, glm::ivec2(0), hi0);
    b = glm::clamp(b, glm::ivec2(0), hi0);
    const glm::ivec2 lo = glm::min(a, b), hi = glm::max(a, b);
    return {lo.x, lo.y, hi.x - lo.x, hi.y - lo.y};
}

glm::ivec4 ResizeRect(glm::ivec4 r, int handle, glm::ivec2 d, int width, int height) {
    int x0 = r.x, y0 = r.y, x1 = r.x + r.z, y1 = r.y + r.w;
    const bool left = handle == HTopLeft || handle == HLeft || handle == HBottomLeft;
    const bool right = handle == HTopRight || handle == HRight || handle == HBottomRight;
    const bool top = handle == HTopLeft || handle == HTop || handle == HTopRight;
    const bool bottom = handle == HBottomLeft || handle == HBottom || handle == HBottomRight;
    if (left) x0 = std::clamp(x0 + d.x, 0, x1 - 1);
    if (right) x1 = std::clamp(x1 + d.x, x0 + 1, std::max(x0 + 1, width));
    if (top) y0 = std::clamp(y0 + d.y, 0, y1 - 1);
    if (bottom) y1 = std::clamp(y1 + d.y, y0 + 1, std::max(y0 + 1, height));
    return {x0, y0, x1 - x0, y1 - y0};
}

glm::ivec4 MoveRect(glm::ivec4 r, glm::ivec2 d, int width, int height) {
    r.x = std::clamp(r.x + d.x, 0, std::max(0, width - r.z));
    r.y = std::clamp(r.y + d.y, 0, std::max(0, height - r.w));
    return r;
}

// --- Точка опоры ---------------------------------------------------------------

glm::vec2 PivotPreset(int index) {
    index = std::clamp(index, 0, 8);
    return {(float)(index % 3) * 0.5f, (float)(index / 3) * 0.5f};
}

int PivotPresetOf(glm::vec2 p) {
    for (int i = 0; i < 9; ++i) {
        const glm::vec2 q = PivotPreset(i);
        if (std::fabs(q.x - p.x) < 1e-4f && std::fabs(q.y - p.y) < 1e-4f) return i;
    }
    return kPivotCustom;
}

bool NameFree(const SpriteSheet& sheet, const std::string& name, int self) {
    if (name.empty()) return false;
    for (int i = 0; i < (int)sheet.Regions.size(); ++i)
        if (i != self && sheet.Regions[(size_t)i].Name == name) return false;
    return true;
}

glm::vec4 ClampBorder(glm::vec4 b, float w, float h) {
    b = glm::max(b, glm::vec4(0.0f));
    b.x = std::min(b.x, std::max(0.0f, w));
    b.z = std::min(b.z, std::max(0.0f, w - b.x));
    b.y = std::min(b.y, std::max(0.0f, h));
    b.w = std::min(b.w, std::max(0.0f, h - b.y));
    return b;
}

// --- Файлы -------------------------------------------------------------------

bool SaveDoc(const std::string& image, const SpriteDoc& doc, std::string& err) {
    if (image.empty()) {
        err = "нет картинки";
        return false;
    }
    namespace fs = std::filesystem;
    std::error_code ec;
    const std::string sheetPath = SpriteSheet::SidecarPath(image);
    const std::string ninePath = NineSlice::SidecarPath(image);
    // Лист пишется, если в нём что-то есть или он уже лежал рядом (его
    // опустошили — значит, пустым и сохранить, а не оставить старые куски).
    const bool sheetHasData = !doc.Sheet.Regions.empty() || !doc.Sheet.Sequences.empty();
    if (sheetHasData || fs::exists(sage::PathFromUtf8(sheetPath), ec))
        if (!doc.Sheet.SaveFile(sheetPath, err)) return false;
    if (!doc.Texture.Empty() || fs::exists(sage::PathFromUtf8(ninePath), ec))
        if (!doc.Texture.SaveFile(ninePath, err)) return false;
    return true;
}

bool LoadDoc(const std::string& image, SpriteDoc& doc, std::string& err) {
    doc = SpriteDoc();
    if (image.empty()) return true;
    const std::string sheetPath = SpriteSheet::SidecarPath(image);
    std::string text;
    if (sage::assets::vfs::ReadText(sheetPath, text) && !SpriteSheet::FromJsonString(text, doc.Sheet, err))
        return false;
    text.clear();
    const std::string ninePath = NineSlice::SidecarPath(image);
    if (sage::assets::vfs::ReadText(ninePath, text) && !NineSlice::FromJsonString(text, doc.Texture, err))
        return false;
    return true;
}

} // namespace sage::editor::sprite
