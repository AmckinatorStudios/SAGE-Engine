#include "sage/ui/core/UITypes.h"

#include <cstdio>

namespace sage::ui {
namespace {
int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
} // namespace

UIColor UIColorFromHex(const std::string& hex, const UIColor& fallback) {
    // Пустая строка и мусор дают запасной цвет, а не чёрный: «цвет не разобрался»
    // и «цвет чёрный» — разные вещи, и первое обязано быть заметно (§134).
    size_t i = 0;
    if (i < hex.size() && hex[i] == '#') ++i;
    const size_t digits = hex.size() - i;
    if (digits != 6 && digits != 8 && digits != 3) return fallback;

    int v[8] = {0};
    for (size_t k = 0; k < digits; ++k) {
        const int d = HexDigit(hex[i + k]);
        if (d < 0) return fallback;
        v[k] = d;
    }
    if (digits == 3) {
        // Короткая форма #RGB: каждый разряд удваивается.
        return UIColor(v[0] * 17 / 255.0f, v[1] * 17 / 255.0f, v[2] * 17 / 255.0f, 1.0f);
    }
    const float r = (v[0] * 16 + v[1]) / 255.0f;
    const float g = (v[2] * 16 + v[3]) / 255.0f;
    const float b = (v[4] * 16 + v[5]) / 255.0f;
    const float a = digits == 8 ? (v[6] * 16 + v[7]) / 255.0f : 1.0f;
    return UIColor(r, g, b, a);
}

std::string UIColorToHex(const UIColor& c) {
    auto byte = [](float v) {
        int i = (int)(UIClamp01(v) * 255.0f + 0.5f);
        return i < 0 ? 0 : (i > 255 ? 255 : i);
    };
    char buf[10];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X%02X", byte(c.r), byte(c.g), byte(c.b),
                  byte(c.a));
    return buf;
}

const char* const* UIBlendModeNames() {
    // Имена в файле и в редакторе — английские, как везде в движке; перевод
    // подписи делает редактор.
    static const char* names[] = {"Normal", "Add",    "Multiply", "Screen",
                                  "Overlay", "Darken", "Lighten"};
    return names;
}
int UIBlendModeCount() { return 7; }

const char* const* UIUnitNames() {
    static const char* names[] = {"Pixels", "Percent", "Content", "Stretch"};
    return names;
}
int UIUnitCount() { return 4; }

} // namespace sage::ui
