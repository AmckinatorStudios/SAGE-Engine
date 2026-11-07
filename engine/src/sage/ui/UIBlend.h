#pragma once
#include <string>

// Как элемент интерфейса ложится на то, что под ним (Element::Blend,
// UIRenderer::SetBlend). Invert — инверсия фона под рисунком: прицел, видный
// на любом фоне; Multiply — затемнение умножением; Screen — осветление;
// Additive — свечение. Normal — обычная прозрачность.
enum class UIBlend { Normal, Additive, Multiply, Screen, Invert };

// Имя для файла сцены и скриптов: "normal", "add", "multiply", "screen",
// "invert". Неизвестное имя -> false (вызывающий решает, ошибка ли это).
inline const char* UIBlendName(UIBlend b) {
    switch (b) {
        case UIBlend::Additive: return "add";
        case UIBlend::Multiply: return "multiply";
        case UIBlend::Screen:   return "screen";
        case UIBlend::Invert:   return "invert";
        default:                return "normal";
    }
}
inline bool UIBlendFromName(const std::string& name, UIBlend& out) {
    if (name == "normal") out = UIBlend::Normal;
    else if (name == "add" || name == "additive") out = UIBlend::Additive;
    else if (name == "multiply") out = UIBlend::Multiply;
    else if (name == "screen") out = UIBlend::Screen;
    else if (name == "invert") out = UIBlend::Invert;
    else return false;
    return true;
}
