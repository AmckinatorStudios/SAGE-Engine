#include "sage/ui/NineSlice.h"

#include <algorithm>
#include <cmath>
#include <fstream>

#include <nlohmann/json.hpp>
#include <stb_image.h>

#include "sage/core/Paths.h"

namespace sage::ui {
namespace {

const char* FillName(SliceFill f) { return f == SliceFill::Tile ? "tile" : "stretch"; }
SliceFill FillFromName(const std::string& s) {
    return s == "tile" ? SliceFill::Tile : SliceFill::Stretch;
}

// Режет отрезок [0, total] на три части: неподвижное начало, тянущуюся
// середину и неподвижный конец. Возвращает четыре границы.
//
// Ужимание — здесь, в одном месте на обе оси. Раньше оно было двумя парами
// строк подряд, и ошибиться в одной из них, не тронув вторую, ничего не мешало.
void Split(float origin, float total, float head, float tail, float out[4]) {
    const float sum = head + tail;
    if (sum > total && sum > 0.0f) {
        const float k = total / sum;
        head *= k;
        tail *= k;
    }
    out[0] = origin;
    out[1] = origin + head;
    out[2] = origin + total - tail;
    out[3] = origin + total;
    // Ужатые поля могут сойтись в одну точку — середина обязана остаться
    // невырожденной по порядку, иначе кусок получит отрицательную ширину.
    out[2] = std::max(out[1], out[2]);
}

// Раскладывает ОДНУ ось тянущегося куска: либо один растянутый отрезок, либо
// цепочка повторов исходного размера с обрезанным хвостом.
//
// Обрезанный хвост — не мелочь: без него последний повтор вылезал бы за
// пределы элемента, и рамка «подтекала» бы наружу ровно настолько, насколько
// размер не кратен куску. Заметно это только на некоторых размерах — то есть
// ровно тот вид поломки, который считают случайным.
struct Span { float DstA, DstB, SrcA, SrcB; };

void LayOutAxis(SliceFill fill, float dstA, float dstB, float srcA, float srcB, float scale,
                std::vector<Span>& out) {
    const float dstLen = dstB - dstA;
    const float srcLen = srcB - srcA;
    if (dstLen <= 0.0f || srcLen <= 0.0f) return;

    const float step = srcLen * scale;
    if (fill == SliceFill::Stretch || step <= 0.0f) {
        out.push_back({dstA, dstB, srcA, srcB});
        return;
    }
    // Потолок на число повторов. Кусок в один пиксель исходника на панели
    // шириной в экран дал бы тысячи квадов — при том, что глазу такой повтор
    // неотличим от растяжения. Лучше нарисовать грубее, чем встать колом.
    constexpr int kMaxRepeats = 512;
    const int count = (int)std::ceil(dstLen / step);
    if (count > kMaxRepeats) {
        out.push_back({dstA, dstB, srcA, srcB});
        return;
    }
    for (int i = 0; i < count; ++i) {
        const float a = dstA + (float)i * step;
        const float b = std::min(a + step, dstB);
        // Последний повтор обрезан — вместе с ним обрезается и кусок исходника,
        // иначе хвост показал бы не ту часть картинки.
        const float frac = (b - a) / step;
        out.push_back({a, b, srcA, srcA + srcLen * frac});
    }
}

} // namespace

std::vector<SliceQuad> Solve(const NineSlice& slice, const SliceRequest& req) {
    std::vector<SliceQuad> quads;
    if (req.SrcW <= 0.0f || req.SrcH <= 0.0f || req.DstW <= 0.0f || req.DstH <= 0.0f) return quads;

    const float scale = req.Scale > 0.0f ? req.Scale : 1.0f;

    // Поля не могут быть больше самого исходника: рамка в 40 пикселей на
    // картинке в 32 означала бы, что левый край начинается правее правого.
    const float l = std::clamp(slice.Left, 0.0f, req.SrcW);
    const float r = std::clamp(slice.Right, 0.0f, req.SrcW - l);
    const float t = std::clamp(slice.Top, 0.0f, req.SrcH);
    const float b = std::clamp(slice.Bottom, 0.0f, req.SrcH - t);

    float dx[4], dy[4];
    Split(req.DstX, req.DstW, l * scale, r * scale, dx);
    Split(req.DstY, req.DstH, t * scale, b * scale, dy);

    const float sx[4] = {req.SrcX, req.SrcX + l, req.SrcX + req.SrcW - r, req.SrcX + req.SrcW};
    const float sy[4] = {req.SrcY, req.SrcY + t, req.SrcY + req.SrcH - b, req.SrcY + req.SrcH};

    std::vector<Span> cols, rows;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            const bool center = (row == 1 && col == 1);
            if (center && !slice.DrawCenter) continue;

            const float qw = dx[col + 1] - dx[col];
            const float qh = dy[row + 1] - dy[row];
            if (qw <= 0.0f || qh <= 0.0f) continue;

            // Тянется ли кусок по каждой оси: углы не тянутся вовсе, края — по
            // одной, середина — по обеим.
            const SliceFill fillX =
                col == 1 ? (center ? slice.CenterFill : slice.EdgeFill) : SliceFill::Stretch;
            const SliceFill fillY =
                row == 1 ? (center ? slice.CenterFill : slice.EdgeFill) : SliceFill::Stretch;

            cols.clear();
            rows.clear();
            LayOutAxis(fillX, dx[col], dx[col + 1], sx[col], sx[col + 1], scale, cols);
            LayOutAxis(fillY, dy[row], dy[row + 1], sy[row], sy[row + 1], scale, rows);
            for (const Span& cy : rows) {
                for (const Span& cxs : cols) {
                    quads.push_back({cxs.DstA, cy.DstA, cxs.DstB - cxs.DstA, cy.DstB - cy.DstA,
                                     cxs.SrcA, cy.SrcA, cxs.SrcB - cxs.SrcA, cy.SrcB - cy.SrcA});
                }
            }
        }
    }
    return quads;
}

glm::vec2 MinimumSize(const NineSlice& slice, float scale) {
    if (scale <= 0.0f) scale = 1.0f;
    return {(slice.Left + slice.Right) * scale, (slice.Top + slice.Bottom) * scale};
}

// --- Файл --------------------------------------------------------------------

std::string NineSlice::ToJsonString() const {
    nlohmann::json j;
    j["border"] = {Left, Top, Right, Bottom};
    j["centerFill"] = FillName(CenterFill);
    j["edgeFill"] = FillName(EdgeFill);
    j["drawCenter"] = DrawCenter;
    return j.dump(2);
}

bool NineSlice::FromJsonString(const std::string& text, NineSlice& out, std::string& err) {
    try {
        const nlohmann::json j = nlohmann::json::parse(text);
        if (j.contains("border") && j["border"].is_array() && j["border"].size() == 4) {
            out.Left = j["border"][0].get<float>();
            out.Top = j["border"][1].get<float>();
            out.Right = j["border"][2].get<float>();
            out.Bottom = j["border"][3].get<float>();
        }
        if (j.contains("centerFill")) out.CenterFill = FillFromName(j["centerFill"].get<std::string>());
        if (j.contains("edgeFill")) out.EdgeFill = FillFromName(j["edgeFill"].get<std::string>());
        if (j.contains("drawCenter")) out.DrawCenter = j["drawCenter"].get<bool>();
        return true;
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
}

bool NineSlice::SaveFile(const std::string& path, std::string& err) const {
    std::ofstream file(sage::PathFromUtf8(path), std::ios::binary);
    if (!file) {
        err = "не удалось открыть на запись: " + path;
        return false;
    }
    file << ToJsonString();
    return true;
}

bool NineSlice::LoadFile(const std::string& path, NineSlice& out, std::string& err) {
    std::ifstream file(sage::PathFromUtf8(path), std::ios::binary);
    if (!file) {
        err = "нет файла: " + path;
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return FromJsonString(text, out, err);
}

std::string NineSlice::SidecarPath(const std::string& imagePath) {
    return imagePath.empty() ? std::string() : imagePath + ".sage9";
}

// --- Догадка -----------------------------------------------------------------

bool GuessBorder(const unsigned char* rgba, int width, int height, NineSlice& out) {
    if (!rgba || width < 3 || height < 3) return false;

    auto columnsEqual = [&](int x0, int x1) {
        for (int y = 0; y < height; ++y) {
            const unsigned char* a = rgba + ((size_t)y * width + x0) * 4;
            const unsigned char* b = rgba + ((size_t)y * width + x1) * 4;
            for (int c = 0; c < 4; ++c)
                if (a[c] != b[c]) return false;
        }
        return true;
    };
    auto rowsEqual = [&](int y0, int y1) {
        const unsigned char* a = rgba + (size_t)y0 * width * 4;
        const unsigned char* b = rgba + (size_t)y1 * width * 4;
        for (int i = 0; i < width * 4; ++i)
            if (a[i] != b[i]) return false;
        return true;
    };

    // ГДЕ КОНЧАЕТСЯ УГОЛ. Не там, где соседние столбцы начинают совпадать —
    // у обычной рамки совпадают И столбцы каймы между собой, И столбцы
    // середины между собой, так что этот признак ничего не различает. Угол
    // кончается там, где столбец перестаёт повторять КРАЙНИЙ: кайма — это
    // полоса, одинаковая от края внутрь, а середина от неё отличается.
    //
    // Потолок в половину размера: уголок больше половины картинки означал бы,
    // что тянуть нечего.
    const int maxX = width / 2, maxY = height / 2;

    int left = 1;
    while (left < maxX && columnsEqual(left, 0)) ++left;
    int right = 1;
    while (right < maxX && columnsEqual(width - 1 - right, width - 1)) ++right;
    int top = 1;
    while (top < maxY && rowsEqual(top, 0)) ++top;
    int bottom = 1;
    while (bottom < maxY && rowsEqual(height - 1 - bottom, height - 1)) ++bottom;

    // Однородная картинка: ни один столбец (строка) не отличается от крайнего,
    // и счётчики упёрлись в потолок. Резать нечего — предлагать выдуманную
    // рамку честнее не будет.
    if (left >= maxX && right >= maxX && top >= maxY && bottom >= maxY) return false;

    out.Left = (float)left;
    out.Right = (float)right;
    out.Top = (float)top;
    out.Bottom = (float)bottom;
    return true;
}

bool GuessBorderFromFile(const std::string& imagePath, NineSlice& out) {
    if (imagePath.empty()) return false;
    // БЕЗ переворота по вертикали: догадка симметрична по осям, но поля
    // «сверху» и «снизу» — нет, и перевёрнутая картинка поменяла бы их местами.
    // Загрузчик текстур переворачивает (GPU ждёт (0,0) внизу), поэтому флаг
    // выставляется явно, а не берётся тем, каким его оставил кто-то другой.
    stbi_set_flip_vertically_on_load_thread(false);
    int w = 0, h = 0, channels = 0;
    unsigned char* px = stbi_load(imagePath.c_str(), &w, &h, &channels, 4);
    if (!px) return false;
    const bool ok = GuessBorder(px, w, h, out);
    stbi_image_free(px);
    return ok;
}

} // namespace sage::ui
