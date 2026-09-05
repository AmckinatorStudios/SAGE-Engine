#include "sage/ui/visual/UITextLayout.h"

#include <algorithm>
#include <cmath>

namespace sage::ui {

// --- UTF-8 ------------------------------------------------------------------
//
// Свой разбор, а не std::codecvt: тот объявлен устаревшим, тянет локали и на
// разных платформах ведёт себя по-разному. Здесь нужно ровно две операции —
// «следующий символ» и «предыдущий», — и обе занимают десяток строк.

uint32_t UIUtf8Next(const std::string& s, int& i) {
    if (i < 0 || i >= (int)s.size()) { i = (int)s.size(); return 0; }
    const unsigned char c = (unsigned char)s[(size_t)i];
    uint32_t cp = c;
    int extra = 0;
    if (c >= 0xF0) { cp = c & 0x07u; extra = 3; }
    else if (c >= 0xE0) { cp = c & 0x0Fu; extra = 2; }
    else if (c >= 0xC0) { cp = c & 0x1Fu; extra = 1; }
    ++i;
    for (int k = 0; k < extra && i < (int)s.size(); ++k, ++i) {
        const unsigned char n = (unsigned char)s[(size_t)i];
        if ((n & 0xC0u) != 0x80u) break; // битая последовательность — не глотаем чужой байт
        cp = (cp << 6) | (n & 0x3Fu);
    }
    return cp;
}

int UIUtf8Prev(const std::string& s, int i) {
    if (i <= 0) return 0;
    --i;
    while (i > 0 && ((unsigned char)s[(size_t)i] & 0xC0u) == 0x80u) --i;
    return i;
}

int UIUtf8Length(const std::string& s) {
    int n = 0, i = 0;
    while (i < (int)s.size()) { UIUtf8Next(s, i); ++n; }
    return n;
}

void UIUtf8Append(std::string& s, uint32_t cp) {
    if (cp < 0x80) { s.push_back((char)cp); return; }
    if (cp < 0x800) {
        s.push_back((char)(0xC0 | (cp >> 6)));
        s.push_back((char)(0x80 | (cp & 0x3F)));
        return;
    }
    if (cp < 0x10000) {
        s.push_back((char)(0xE0 | (cp >> 12)));
        s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back((char)(0x80 | (cp & 0x3F)));
        return;
    }
    s.push_back((char)(0xF0 | (cp >> 18)));
    s.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
    s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
    s.push_back((char)(0x80 | (cp & 0x3F)));
}

namespace {

bool IsSpace(uint32_t cp) { return cp == ' ' || cp == '\t'; }
// Места, где строку можно разорвать без дефиса. Пробел, дефис и неразрывные
// знаки — минимально достаточный набор для кириллицы и латиницы.
bool IsBreakable(uint32_t cp) {
    return IsSpace(cp) || cp == '-' || cp == 0x2013 || cp == 0x2014 || cp == '/';
}

// Какой оформленный кусок покрывает этот байт (§15, rich text runs).
int RunAt(const UIText& text, int byteOffset) {
    for (size_t i = 0; i < text.Runs.size(); ++i) {
        const UITextRun& r = text.Runs[i];
        if (byteOffset >= r.Begin && byteOffset < r.End) return (int)i;
    }
    return -1;
}

struct FontPick {
    int Font = -1;
    float Scale = 1.0f;
};

// Шрифт для символа с учётом ЗАПАСНОГО (§15, fallback fonts). Без этого один
// отсутствующий символ превращает строку в ряд прямоугольников, и виноватым
// выглядит движок, а не шрифт.
FontPick PickFont(const UIContext& ctx, int primary, int fallback, uint32_t cp) {
    FontPick p;
    p.Font = primary;
    if (!ctx.Fonts) return p;
    if (primary >= 0 && ctx.Fonts->HasGlyph(primary, cp)) return p;
    if (fallback >= 0 && ctx.Fonts->HasGlyph(fallback, cp)) { p.Font = fallback; return p; }
    return p;
}

struct Measured {
    uint32_t Cp = 0;
    int Byte = 0;
    int Run = -1;
    int Font = -1;
    float Advance = 0.0f;
    float Size = 0.0f;
    bool Break = false;   // сюда можно перенести
    bool Newline = false;
};

} // namespace

UITextLayoutResult UILayoutText(const UIContext& ctx, const UIText& text, float maxWidth,
                                float maxHeight) {
    UITextLayoutResult out;
    const std::string src = text.Resolve(ctx);
    out.FontSize = text.Size;
    if (src.empty()) {
        if (ctx.Fonts) {
            const int f = text.Font.empty() ? ctx.Fonts->Fallback()
                                            : ctx.Fonts->Resolve(text.Font, text.Weight, text.Italic);
            const UIFontMetrics m = ctx.Fonts->Metrics(f >= 0 ? f : ctx.Fonts->Fallback());
            // Пустая строка всё равно занимает высоту строки: иначе поле ввода
            // без текста схлопывается, и каретке негде стоять.
            out.Size = {0.0f, m.LineHeight * text.Size * text.LineSpacing};
        }
        return out;
    }
    if (!ctx.Fonts) return out;

    const int primary = text.Font.empty()
                            ? ctx.Fonts->Fallback()
                            : ctx.Fonts->Resolve(text.Font, text.Weight, text.Italic);
    const int fallback = ctx.Fonts->Fallback();
    const int mainFont = primary >= 0 ? primary : fallback;
    if (mainFont < 0) return out;

    const float availW = maxWidth > 0.0f ? maxWidth - text.Padding.Horizontal() : 0.0f;
    const float availH = maxHeight > 0.0f ? maxHeight - text.Padding.Vertical() : 0.0f;

    // Автоподбор кегля (§15): уменьшаем, пока не поместится. Двоичным поиском,
    // а не шагом по единице: шаг по единице на кегле 96 — девяносто раскладок
    // на каждый кадр, и это видно в профайлере.
    float size = text.Size;
    if (text.AutoSize && (availW > 0.0f || availH > 0.0f)) {
        float lo = std::max(1.0f, text.MinSize);
        float hi = std::max(lo, text.MaxSize);
        UIText probe = text;
        probe.AutoSize = false;
        // Мерить надо ПОЛНЫЙ текст, а не обрезанный: при Clip/Ellipsis
        // раскладка выбрасывает то, что не влезло, ширина всегда «помещается»,
        // и подбор кегля сходится к максимуму, ничего не подобрав.
        probe.Overflow = UITextOverflow::Visible;
        for (int iter = 0; iter < 12; ++iter) {
            const float mid = (lo + hi) * 0.5f;
            probe.Size = mid;
            const UITextLayoutResult t = UILayoutText(ctx, probe, maxWidth, 0.0f);
            const bool fitsW = availW <= 0.0f || t.Size.x <= availW + 0.5f;
            const bool fitsH = availH <= 0.0f || t.Size.y <= availH + 0.5f;
            if (fitsW && fitsH) lo = mid; else hi = mid;
        }
        size = lo;
    }
    out.FontSize = size;

    const UIFontMetrics metrics = ctx.Fonts->Metrics(mainFont);
    const float lineHeight = metrics.LineHeight * size * text.LineSpacing;
    const float ascent = metrics.Ascent * size;

    // 1. Померить все символы.
    std::vector<Measured> chars;
    chars.reserve(src.size());
    int i = 0;
    while (i < (int)src.size()) {
        const int byte = i;
        const uint32_t cp = UIUtf8Next(src, i);
        Measured m;
        m.Cp = cp;
        m.Byte = byte;
        m.Run = RunAt(text, byte);
        m.Size = size * (m.Run >= 0 ? text.Runs[(size_t)m.Run].SizeScale : 1.0f);
        if (cp == '\n') {
            m.Newline = true;
            chars.push_back(m);
            continue;
        }
        if (cp == '\r') continue;
        const FontPick pick = PickFont(ctx, mainFont, fallback, cp);
        m.Font = pick.Font;
        m.Advance = ctx.Fonts->Advance(pick.Font, cp) * m.Size + text.LetterSpacing;
        m.Break = IsBreakable(cp);
        chars.push_back(m);
    }

    // 2. Разбить на строки.
    struct LineRange { int First = 0, Last = 0; float Width = 0.0f; };
    std::vector<LineRange> lines;
    const bool wrap = text.Wrap != UITextWrap::None && availW > 0.0f;

    int lineStart = 0;
    float lineWidth = 0.0f;
    int lastBreak = -1;
    float widthAtBreak = 0.0f;

    for (int k = 0; k < (int)chars.size(); ++k) {
        const Measured& m = chars[(size_t)k];
        if (m.Newline) {
            lines.push_back({lineStart, k, lineWidth});
            lineStart = k + 1;
            lineWidth = 0.0f;
            lastBreak = -1;
            continue;
        }
        const float next = lineWidth + m.Advance;
        if (wrap && next > availW && k > lineStart) {
            int breakAt = k;
            if (text.Wrap == UITextWrap::Word && lastBreak > lineStart) {
                // Перенос по СЛОВАМ. Слово, которое не влезает целиком, режется
                // по символам — иначе одно длинное слово молча уехало бы за
                // край, то есть ровно то, от чего перенос и спасает.
                breakAt = lastBreak + 1;
                lines.push_back({lineStart, lastBreak, widthAtBreak});
            } else {
                lines.push_back({lineStart, k - 1, lineWidth});
            }
            lineStart = breakAt;
            lineWidth = 0.0f;
            lastBreak = -1;
            for (int j = lineStart; j <= k; ++j) lineWidth += chars[(size_t)j].Advance;
            // Ведущие пробелы новой строки не считаем — иначе абзац «дышит»
            // слева на случайную величину.
            while (lineStart <= k && IsSpace(chars[(size_t)lineStart].Cp)) {
                lineWidth -= chars[(size_t)lineStart].Advance;
                ++lineStart;
            }
            continue;
        }
        lineWidth = next;
        if (m.Break) { lastBreak = k; widthAtBreak = lineWidth - (IsSpace(m.Cp) ? m.Advance : 0.0f); }
    }
    lines.push_back({lineStart, (int)chars.size() - 1, lineWidth});

    // 3. Ограничение по числу строк и по высоте (§15).
    int maxLines = text.MaxLines;
    if (availH > 0.0f && lineHeight > 0.0f) {
        const int byHeight = std::max(1, (int)std::floor(availH / lineHeight + 0.001f));
        maxLines = maxLines > 0 ? std::min(maxLines, byHeight) : byHeight;
    }
    if (text.Overflow == UITextOverflow::Visible) maxLines = text.MaxLines;
    if (maxLines > 0 && (int)lines.size() > maxLines) {
        lines.resize((size_t)maxLines);
        out.Truncated = true;
    }

    // 4. Выложить глифы.
    const float ellipsisAdvance =
        ctx.Fonts->Advance(mainFont, 0x2026 /* … */) * size; // многоточие одним символом
    float top = 0.0f;
    float maxLineWidth = 0.0f;

    for (size_t li = 0; li < lines.size(); ++li) {
        LineRange& lr = lines[li];
        UITextLine line;
        line.First = (int)out.Glyphs.size();
        line.Top = top;
        line.Baseline = top + ascent;
        line.Height = lineHeight;
        line.ByteBegin = lr.First < (int)chars.size() ? chars[(size_t)lr.First].Byte : (int)src.size();

        const bool lastLine = li + 1 == lines.size();
        const bool needEllipsis = text.Overflow == UITextOverflow::Ellipsis &&
                                  ((lastLine && out.Truncated) ||
                                   (availW > 0.0f && lr.Width > availW + 0.5f));

        float x = 0.0f;
        int lastByte = line.ByteBegin;
        for (int k = lr.First; k <= lr.Last && k < (int)chars.size(); ++k) {
            const Measured& m = chars[(size_t)k];
            if (m.Newline) break;
            if (availW > 0.0f && text.Overflow != UITextOverflow::Visible &&
                x + m.Advance > availW + 0.5f) {
                if (needEllipsis) {
                    // Место под многоточие освобождается СПРАВА, откусывая уже
                    // выложенные глифы: иначе многоточие само не влезает.
                    while (!out.Glyphs.empty() && x + ellipsisAdvance > availW &&
                           (int)out.Glyphs.size() > line.First) {
                        x -= out.Glyphs.back().Advance;
                        out.Glyphs.pop_back();
                    }
                    UIGlyphPlacement g;
                    g.Codepoint = 0x2026;
                    g.X = x;
                    g.Baseline = line.Baseline;
                    g.Advance = ellipsisAdvance;
                    g.Font = mainFont;
                    g.ByteOffset = m.Byte;
                    out.Glyphs.push_back(g);
                    x += ellipsisAdvance;
                    line.Ellipsized = true;
                }
                out.Truncated = true;
                break;
            }
            UIGlyphPlacement g;
            g.Codepoint = m.Cp;
            g.X = x;
            g.Baseline = line.Baseline;
            g.Advance = m.Advance;
            g.Font = m.Font;
            g.RunIndex = m.Run;
            g.ByteOffset = m.Byte;
            out.Glyphs.push_back(g);
            x += m.Advance;
            lastByte = m.Byte + 1;
        }

        line.Count = (int)out.Glyphs.size() - line.First;
        line.Width = x;
        line.ByteEnd = lastByte;
        maxLineWidth = std::max(maxLineWidth, line.Width);
        out.Lines.push_back(line);
        top += lineHeight + text.ParagraphSpacing;
    }

    // 5. Горизонтальное выравнивание (§16).
    const float boxW = availW > 0.0f ? availW : maxLineWidth;
    for (size_t li = 0; li < out.Lines.size(); ++li) {
        UITextLine& line = out.Lines[li];
        float shift = 0.0f;
        float extraPerGap = 0.0f;
        switch (text.Align) {
            case UITextAlign::Center: shift = (boxW - line.Width) * 0.5f; break;
            case UITextAlign::Right: shift = boxW - line.Width; break;
            case UITextAlign::Justify: {
                // Последняя строка абзаца по ширине не растягивается — иначе
                // абзац заканчивается строкой из трёх слов на всю ширину.
                const bool last = li + 1 == out.Lines.size();
                if (!last && line.Count > 1) {
                    int gaps = 0;
                    for (int g = 0; g < line.Count; ++g)
                        if (IsSpace(out.Glyphs[(size_t)(line.First + g)].Codepoint)) ++gaps;
                    if (gaps > 0) extraPerGap = (boxW - line.Width) / (float)gaps;
                }
                break;
            }
            default: break;
        }
        float accumulated = 0.0f;
        for (int g = 0; g < line.Count; ++g) {
            UIGlyphPlacement& gl = out.Glyphs[(size_t)(line.First + g)];
            gl.X += shift + accumulated;
            if (extraPerGap != 0.0f && IsSpace(gl.Codepoint)) accumulated += extraPerGap;
        }
        if (extraPerGap != 0.0f) line.Width = boxW;
        line.Top += 0.0f;
    }

    out.Size = {maxLineWidth, out.Lines.empty() ? 0.0f
                                                : out.Lines.back().Top + out.Lines.back().Height};

    // 6. Вертикальное выравнивание.
    if (availH > 0.0f) {
        float dy = 0.0f;
        switch (text.VAlign) {
            case UITextVAlign::Center: dy = (availH - out.Size.y) * 0.5f; break;
            case UITextVAlign::Bottom: dy = availH - out.Size.y; break;
            case UITextVAlign::Baseline:
                // По базовой линии ПЕРВОЙ строки: так подписи разного кегля в
                // одном ряду стоят на общей линии, а не «примерно рядом».
                dy = out.Lines.empty() ? 0.0f : -out.Lines.front().Top;
                break;
            default: break;
        }
        if (dy != 0.0f) {
            for (auto& g : out.Glyphs) g.Baseline += dy;
            for (auto& l : out.Lines) { l.Top += dy; l.Baseline += dy; }
        }
    }

    // Поля текста прибавляются в конце — одинаково ко всем вариантам
    // выравнивания.
    if (!text.Padding.Empty()) {
        for (auto& g : out.Glyphs) { g.X += text.Padding.L; g.Baseline += text.Padding.T; }
        for (auto& l : out.Lines) { l.Top += text.Padding.T; l.Baseline += text.Padding.T; }
        out.Size += glm::vec2(text.Padding.Horizontal(), text.Padding.Vertical());
    }
    return out;
}

int UITextIndexAt(const UITextLayoutResult& layout, glm::vec2 point) {
    if (layout.Lines.empty()) return 0;
    const UITextLine* line = &layout.Lines.front();
    for (const auto& l : layout.Lines) {
        if (point.y >= l.Top && point.y < l.Top + l.Height) { line = &l; break; }
        if (point.y >= l.Top) line = &l;
    }
    for (int g = 0; g < line->Count; ++g) {
        const UIGlyphPlacement& gl = layout.Glyphs[(size_t)(line->First + g)];
        if (point.x < gl.X + gl.Advance * 0.5f) return gl.ByteOffset;
    }
    return line->ByteEnd;
}

glm::vec2 UITextCaretPos(const UITextLayoutResult& layout, int byteOffset, float& heightOut) {
    heightOut = layout.Lines.empty() ? layout.FontSize : layout.Lines.front().Height;
    if (layout.Lines.empty()) return glm::vec2(0.0f);
    for (const auto& line : layout.Lines) {
        if (byteOffset < line.ByteBegin) break;
        if (byteOffset > line.ByteEnd && &line != &layout.Lines.back()) continue;
        heightOut = line.Height;
        for (int g = 0; g < line.Count; ++g) {
            const UIGlyphPlacement& gl = layout.Glyphs[(size_t)(line.First + g)];
            if (gl.ByteOffset >= byteOffset) return {gl.X, line.Top};
        }
        return {line.Width, line.Top};
    }
    const UITextLine& first = layout.Lines.front();
    return {0.0f, first.Top};
}

} // namespace sage::ui
