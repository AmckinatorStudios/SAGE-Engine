#pragma once
#include <algorithm>
#include <cmath>

#include "sage/ui/UIAnchor.h"   // UIRect

// ---------------------------------------------------------------------------
// КУДА ЛОЖИТСЯ КАРТИНКА, СОХРАНЯЮЩАЯ ПРОПОРЦИИ.
//
// Отдельной функцией, а не десятком строк внутри отрисовки, по двум причинам.
// Первая: это арифметика, и проверяется она числами, без видеокарты и кадра, —
// то есть тестом, а не глазами. Вторая: тем же правилом обязаны жить и обложки
// ассетов в редакторе, и предпросмотр — «вписать, сохраняя пропорции» не может
// означать в двух местах разное.
// ---------------------------------------------------------------------------
namespace sage::ui {

// Куда рисовать (Dst) и какой кусок исходника брать (Src, в пикселях
// исходника). При Fit кусок берётся целиком, при Cover — вырезается по центру.
struct ImagePlacement {
    UIRect Dst{0.0f, 0.0f, 0.0f, 0.0f};
    float SrcX = 0.0f, SrcY = 0.0f, SrcW = 0.0f, SrcH = 0.0f;
};

// rect — прямоугольник элемента; srcX/srcY/srcW/srcH — кусок исходника
// (весь файл: 0,0,ширина,высота). cover=false — вписать целиком (поля по
// краям), cover=true — заполнить без полей (лишнее по длинной стороне
// обрезается). snapPixels округляет масштаб вниз до целого: дробный растягивает
// одни пиксели исходника на два экранных, а соседние на один.
inline ImagePlacement PlaceImage(const UIRect& rect, float srcX, float srcY, float srcW,
                                 float srcH, bool cover, bool snapPixels) {
    ImagePlacement out;
    out.Dst = rect;
    out.SrcX = srcX;
    out.SrcY = srcY;
    out.SrcW = srcW;
    out.SrcH = srcH;
    if (srcW <= 0.0f || srcH <= 0.0f || rect.w <= 0.0f || rect.h <= 0.0f) return out;

    if (!cover) {
        float k = std::min(rect.w / srcW, rect.h / srcH);
        if (snapPixels) k = std::max(1.0f, std::floor(k));
        out.Dst.w = srcW * k;
        out.Dst.h = srcH * k;
        out.Dst.x = rect.x + std::floor((rect.w - out.Dst.w) * 0.5f);
        out.Dst.y = rect.y + std::floor((rect.h - out.Dst.h) * 0.5f);
        return out;
    }

    // Заполнение: масштаб берётся по БОЛЬШЕЙ нужде, а лишнее отрезается от
    // ИСХОДНИКА, а не рисуется за границами элемента — обрезки у элемента может
    // не быть, и картинка вылезла бы на соседей.
    const float k = std::max(rect.w / srcW, rect.h / srcH);
    const float needW = std::min(srcW, rect.w / k);
    const float needH = std::min(srcH, rect.h / k);
    out.SrcX = srcX + (srcW - needW) * 0.5f;
    out.SrcY = srcY + (srcH - needH) * 0.5f;
    out.SrcW = needW;
    out.SrcH = needH;
    return out;
}

} // namespace sage::ui
