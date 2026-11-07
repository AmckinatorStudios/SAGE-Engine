#include "Font.h"

#include <algorithm>
#include <stdexcept>
#include <vector>

#include <stb_image.h>   // реализация — в render/Texture.cpp

#include "sage/assets/Pack.h"
#include "sage/core/Log.h"
#include "sage/rhi/GraphicsDevice.h"

// ---------------------------------------------------------------------------
// Шрифт-картинка: глифы нарисованы в PNG сеткой 16×16. Так устроены шрифты
// пиксельных игр — у них нет TrueType-файла, есть лист символов, и ширина
// каждого символа читается по его пикселям. Растеризовать такой шрифт через
// TrueType-копию значило бы получить «почти тот же» шрифт: другие ширины,
// другие кромки — ровно то, что видно глазом.
//
// Метрики — в пикселях ячейки (k = ячейка / 8 — пиксель «восьмипиксельной»
// сетки): шаг символа = round(ширина рисунка · 8 / ячейка) + 1 единиц k, где
// ширина рисунка — номер последнего непустого столбца + 1; пробел — 4 единицы.
// Рисуется символ шириной (шаг − 1)·k и высотой в ячейку: последний столбец
// шага — пустой промежуток между буквами.
// ---------------------------------------------------------------------------

namespace {

// Порядок символов по ячейкам: файл «<путь>.chars» (UTF-8) или Latin-1.
std::vector<uint32_t> CellCodepoints(const std::string& path) {
    std::vector<uint32_t> cps(256);
    for (uint32_t i = 0; i < 256; ++i) cps[i] = i;
    std::vector<unsigned char> bytes;
    if (!sage::assets::vfs::ReadFile(path + ".chars", bytes)) {
        // Без расширения картинки: «ascii.chars» рядом с «ascii.png».
        std::string alt = path.substr(0, path.size() - 4) + ".chars";
        if (!sage::assets::vfs::ReadFile(alt, bytes)) return cps;
    }
    std::string text(bytes.begin(), bytes.end());
    // Перевод строки в файле — для удобства (по 16 символов в строке), не символ.
    size_t i = 0, cell = 0;
    while (i < text.size() && cell < 256) {
        uint32_t cp = Utf8Next(text, i);
        if (cp == '\n' || cp == '\r') continue;
        cps[cell++] = cp;
    }
    return cps;
}

} // namespace

std::vector<int> ImageFontUnits(const unsigned char* rgba, int w, int h) {
    std::vector<int> units(256, 0);
    if (!rgba || w < 16 || h < 16) return units;
    const int cw = w / 16, ch = h / 16;
    for (int cell = 0; cell < 256; ++cell) {
        const int cx = (cell % 16) * cw, cy = (cell / 16) * ch;
        int last = -1;
        for (int x = cw - 1; x >= 0 && last < 0; --x)
            for (int y = 0; y < ch; ++y)
                if (rgba[((size_t)(cy + y) * w + cx + x) * 4 + 3] != 0) { last = x; break; }
        if (cell == 32) units[cell] = 4;
        else if (last >= 0) units[cell] = (int)(0.5f + (float)(last + 1) * 8.0f / (float)cw) + 1;
    }
    return units;
}

std::unique_ptr<Font> Font::LoadImageSheet(const std::string& path) {
    std::vector<unsigned char> file;
    if (!sage::assets::vfs::ReadFile(path, file) || file.empty())
        throw std::runtime_error("Font: не удалось открыть картинку шрифта: " + path);
    int w = 0, h = 0, comp = 0;
    stbi_set_flip_vertically_on_load_thread(false);
    unsigned char* px = stbi_load_from_memory(file.data(), (int)file.size(), &w, &h, &comp, 4);
    if (!px) throw std::runtime_error("Font: картинка шрифта не читается: " + path);
    std::vector<unsigned char> rgba(px, px + (size_t)w * h * 4);
    stbi_image_free(px);
    if (w < 16 || h < 16 || w % 16 != 0 || h % 16 != 0)
        throw std::runtime_error("Font: картинка шрифта должна делиться на сетку 16×16: " + path);

    const int cw = w / 16, ch = h / 16;
    const float k = (float)ch / 8.0f;   // пиксель «восьмипиксельной» сетки — в пикселях картинки
    auto font = std::unique_ptr<Font>(new Font());
    font->m_pixelHeight = (float)ch;
    font->m_atlasW = (float)w;
    font->m_atlasH = (float)h;
    font->m_lineHeight = 9.0f * k;
    font->m_ascent = 7.0f * k;
    font->m_nearest = true;
    font->m_gridUnit = k;

    // Покрытие — альфа, умноженная на яркость: серые пиксели рисунка — это
    // полутона буквы, а не прозрачность.
    std::vector<unsigned char> cover((size_t)w * h);
    for (size_t i = 0; i < cover.size(); ++i) {
        const unsigned char* p = &rgba[i * 4];
        const int lum = std::max(p[0], std::max(p[1], p[2]));
        cover[i] = (unsigned char)(p[3] * lum / 255);
    }

    const std::vector<uint32_t> cps = CellCodepoints(path);
    const std::vector<int> units = ImageFontUnits(rgba.data(), w, h);
    for (int cell = 0; cell < 256; ++cell) {
        const uint32_t cp = cps[(size_t)cell];
        // Пробел — по символу, а не по ячейке: у своего порядка он может стоять где угодно.
        const int u = cp == ' ' ? 4 : units[(size_t)cell];
        if (cp == 0 || u == 0) continue;   // пустая ячейка — символа нет
        Glyph g;
        g.advance = u * k;
        if (cp != ' ') {
            const int cx = (cell % 16) * cw, cy = (cell / 16) * ch;
            const float drawW = (float)(u - 1) * k;
            g.size = {drawW, (float)ch};
            g.offset = {0.0f, -font->m_ascent};
            g.uv0 = {(float)cx / w, (float)cy / h};
            g.uv1 = {((float)cx + drawW) / w, (float)(cy + ch) / h};
        }
        font->m_glyphs[cp] = g;
    }

    sage::rhi::Texture2DDesc desc;
    desc.Width = w;
    desc.Height = h;
    desc.Channels = 1;
    desc.FilterMode = sage::rhi::Filter::Nearest;
    desc.WrapMode = sage::rhi::Wrap::ClampEdge;
    desc.GenerateMipmaps = false;
    font->m_atlas = sage::rhi::GraphicsDevice::Get().CreateTexture2D(desc, cover.data());
    LOG_INFO("Font") << "Загружен шрифт-картинка " << path << " (ячейка " << cw << "x" << ch << ", символов "
                     << font->m_glyphs.size() << ")";
    return font;
}
