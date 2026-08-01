#include "Font.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

#include "stb_truetype.h"

#include "sage/core/Log.h"
#include "sage/rhi/GraphicsDevice.h"

// ---------------------------------------------------------------------------
// UTF-8 → codepoint. Классическая ветвистая раскодировка; при некорректной
// последовательности возвращает U+FFFD и продвигается на 1 байт, чтобы не
// зациклиться на битых данных.
// ---------------------------------------------------------------------------
uint32_t Utf8Next(const std::string& s, size_t& i) {
    if (i >= s.size()) return 0;
    unsigned char c = (unsigned char)s[i];
    auto cont = [&](size_t k) -> bool {
        return k < s.size() && ((unsigned char)s[k] & 0xC0) == 0x80;
    };
    if (c < 0x80) { ++i; return c; }
    if ((c & 0xE0) == 0xC0 && cont(i + 1)) {
        uint32_t cp = ((c & 0x1F) << 6) | ((unsigned char)s[i + 1] & 0x3F);
        i += 2; return cp;
    }
    if ((c & 0xF0) == 0xE0 && cont(i + 1) && cont(i + 2)) {
        uint32_t cp = ((c & 0x0F) << 12) | (((unsigned char)s[i + 1] & 0x3F) << 6) |
                      ((unsigned char)s[i + 2] & 0x3F);
        i += 3; return cp;
    }
    if ((c & 0xF8) == 0xF0 && cont(i + 1) && cont(i + 2) && cont(i + 3)) {
        uint32_t cp = ((c & 0x07) << 18) | (((unsigned char)s[i + 1] & 0x3F) << 12) |
                      (((unsigned char)s[i + 2] & 0x3F) << 6) | ((unsigned char)s[i + 3] & 0x3F);
        i += 4; return cp;
    }
    ++i;
    return 0xFFFD;
}

namespace {

// Диапазоны Unicode, которые печём. Basic Latin + Latin-1 (символы/акценты) и
// вся кириллица — этого достаточно для английского и русского текста.
struct Range { uint32_t first; int count; };
constexpr Range kRanges[] = {
    {0x0020, 0x00FF - 0x0020 + 1}, // Basic Latin + Latin-1 Supplement
    {0x0400, 0x04FF - 0x0400 + 1}, // Cyrillic
    {0x2010, 0x2027 - 0x2010 + 1}, // General Punctuation: тире, кавычки, «…», •
};

constexpr int kAtlasW = 1024;
constexpr int kAtlasH = 1024;

// Свободное поле вокруг каждого глифа в атласе. Нужно мип-уровням: на уровне N
// один тексель усредняет 2^N исходных, и без запаса в атлас-соседа «затекает»
// краска чужой буквы. 8 текселей хватает на три уровня — ровно тот диапазон
// уменьшения, в котором живёт интерфейс.
constexpr int kGlyphPadding = 8;

} // namespace

std::unique_ptr<Font> Font::Load(const std::string& path, float pixelHeight, bool pixelArt) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Font: не удалось открыть файл шрифта: " + path);
    }
    std::vector<unsigned char> ttf((std::istreambuf_iterator<char>(file)),
                                    std::istreambuf_iterator<char>());
    if (ttf.empty()) {
        throw std::runtime_error("Font: пустой файл шрифта: " + path);
    }

    stbtt_fontinfo info;
    int offset = stbtt_GetFontOffsetForIndex(ttf.data(), 0);
    if (offset < 0 || !stbtt_InitFont(&info, ttf.data(), offset)) {
        throw std::runtime_error("Font: не удалось разобрать шрифт: " + path);
    }

    auto font = std::unique_ptr<Font>(new Font());
    font->m_pixelHeight = pixelHeight;
    font->m_atlasW = (float)kAtlasW;
    font->m_atlasH = (float)kAtlasH;

    // Вертикальные метрики (в пикселях базового размера).
    float vscale = stbtt_ScaleForPixelHeight(&info, pixelHeight);
    int ascent, descent, lineGap;
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &lineGap);
    font->m_ascent = ascent * vscale;
    font->m_lineHeight = (ascent - descent + lineGap) * vscale;

    // Пакуем глифы всех диапазонов в один атлас.
    //
    // Отступ между глифами — kGlyphPadding, а не 1. Атлас печётся в один размер
    // (48px), а интерфейс рисует текст мельче — вплоть до 1/5 базовой высоты.
    // Такое уменьшение без мип-уровней означает точечную выборку через каждые
    // несколько текселей: тонкие вертикальные штрихи просто не попадают в
    // выборку, и буквы теряют стойки — «П» выглядит как «Г», «К» без левой
    // палки. Мип-уровни это чинят (усреднение вместо пропуска), но требуют
    // запаса вокруг глифа, иначе на дальних уровнях соседние глифы перетекают
    // друг в друга.
    std::vector<unsigned char> atlas(kAtlasW * kAtlasH, 0);
    stbtt_pack_context pc;
    if (!stbtt_PackBegin(&pc, atlas.data(), kAtlasW, kAtlasH, 0, kGlyphPadding, nullptr)) {
        throw std::runtime_error("Font: stbtt_PackBegin failed");
    }
    stbtt_PackSetOversampling(&pc, 1, 1);

    std::vector<std::vector<stbtt_packedchar>> packed(std::size(kRanges));
    std::vector<stbtt_pack_range> ranges(std::size(kRanges));
    for (size_t r = 0; r < std::size(kRanges); ++r) {
        packed[r].resize(kRanges[r].count);
        ranges[r].font_size = pixelHeight;
        ranges[r].first_unicode_codepoint_in_range = (int)kRanges[r].first;
        ranges[r].array_of_unicode_codepoints = nullptr;
        ranges[r].num_chars = kRanges[r].count;
        ranges[r].chardata_for_range = packed[r].data();
    }
    stbtt_PackFontRanges(&pc, ttf.data(), 0, ranges.data(), (int)ranges.size());
    stbtt_PackEnd(&pc);

    // Переносим packedchar → наши Glyph (метрики + UV).
    for (size_t r = 0; r < std::size(kRanges); ++r) {
        for (int k = 0; k < kRanges[r].count; ++k) {
            const uint32_t cp = kRanges[r].first + (uint32_t)k;
            // Символа НЕТ в шрифте — пропускаем совсем. Иначе stb запекает для
            // него .notdef (в большинстве шрифтов это рамка или «?»), глиф
            // выглядит настоящим, и отличить «шрифт не знает кириллицы» от
            // «шрифт её знает» становится нечем: и тем и другим экран
            // заполняется одинаковыми значками. Пропущенный символ уходит на
            // общий фолбэк '?', а HasGlyph начинает говорить правду.
            if (stbtt_FindGlyphIndex(&info, (int)cp) == 0) continue;
            const stbtt_packedchar& pcd = packed[r][k];
            if (pcd.x1 <= pcd.x0 || pcd.y1 <= pcd.y0) continue; // пустой глиф (напр. пробел без формы)
            Glyph g;
            g.uv0 = {pcd.x0 / font->m_atlasW, pcd.y0 / font->m_atlasH};
            g.uv1 = {pcd.x1 / font->m_atlasW, pcd.y1 / font->m_atlasH};
            g.size = {(float)(pcd.x1 - pcd.x0), (float)(pcd.y1 - pcd.y0)};
            g.offset = {pcd.xoff, pcd.yoff};
            g.advance = pcd.xadvance;
            font->m_glyphs[cp] = g;
        }
        // Пробел и другие «безформенные» глифы — сохраняем только advance,
        // чтобы MeasureWidth/раскладка их учитывали.
        for (int k = 0; k < kRanges[r].count; ++k) {
            uint32_t cp = kRanges[r].first + (uint32_t)k;
            if (font->m_glyphs.count(cp)) continue;
            if (stbtt_FindGlyphIndex(&info, (int)cp) == 0) continue;
            const stbtt_packedchar& pcd = packed[r][k];
            if (pcd.xadvance <= 0.0f) continue;
            Glyph g;
            g.advance = pcd.xadvance; // без формы (uv/size = 0)
            font->m_glyphs[cp] = g;
        }
    }

    // Атлас → GPU-текстура (R8, покрытие в красном канале) С мип-уровнями:
    // интерфейс рисует текст в разы мельче базовой высоты запекания, и без
    // мипов уменьшение съедает тонкие штрихи букв (см. комментарий у
    // kGlyphPadding). Отступ между глифами разведён специально под них.
    sage::rhi::Texture2DDesc desc;
    desc.Width = kAtlasW;
    desc.Height = kAtlasH;
    desc.Channels = 1;
    // Пиксельному шрифту и то и другое во вред: сглаживание размазывает
    // однопиксельные штрихи, мип-уровни съедают их совсем.
    desc.FilterMode = pixelArt ? sage::rhi::Filter::Nearest : sage::rhi::Filter::Trilinear;
    desc.WrapMode = sage::rhi::Wrap::ClampEdge;
    desc.GenerateMipmaps = !pixelArt;
    font->m_atlas = sage::rhi::GraphicsDevice::Get().CreateTexture2D(desc, atlas.data());

    font->m_pixelArt = pixelArt;
    LOG_INFO("Font") << "Загружен шрифт " << path << (pixelArt ? " [пиксельный] (" : " (")
                     << (int)pixelHeight
                     << "px, глифов " << font->m_glyphs.size() << ")";
    return font;
}

const Font::Glyph* Font::Find(uint32_t codepoint) const {
    auto it = m_glyphs.find(codepoint);
    if (it != m_glyphs.end()) return &it->second;
    // Фолбэк на '?' для неизвестных символов (если он есть в атласе).
    auto q = m_glyphs.find('?');
    return q != m_glyphs.end() ? &q->second : nullptr;
}

float Font::BuildQuads(const std::string& utf8, float startX, float startY, float scale,
                       std::vector<PositionedGlyph>& out) const {
    float penX = startX;
    float baselineY = startY + m_ascent * scale; // startY — верх строки
    size_t i = 0;
    while (i < utf8.size()) {
        uint32_t cp = Utf8Next(utf8, i);
        if (cp == '\n') { penX = startX; baselineY += m_lineHeight * scale; continue; }
        const Glyph* g = Find(cp);
        if (!g) continue;
        if (g->size.x > 0.0f && g->size.y > 0.0f) {
            PositionedGlyph pg;
            pg.x0 = penX + g->offset.x * scale;
            pg.y0 = baselineY + g->offset.y * scale;
            pg.x1 = pg.x0 + g->size.x * scale;
            pg.y1 = pg.y0 + g->size.y * scale;
            pg.uv0 = g->uv0;
            pg.uv1 = g->uv1;
            out.push_back(pg);
        }
        penX += g->advance * scale;
    }
    return penX;
}

float Font::MeasureWidth(const std::string& utf8, float scale) const {
    float w = 0.0f, maxW = 0.0f;
    size_t i = 0;
    while (i < utf8.size()) {
        uint32_t cp = Utf8Next(utf8, i);
        if (cp == '\n') { maxW = std::max(maxW, w); w = 0.0f; continue; }
        if (const Glyph* g = Find(cp)) w += g->advance * scale;
    }
    return std::max(maxW, w);
}
