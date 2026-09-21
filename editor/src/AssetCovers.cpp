#include "AssetCovers.h"

#include <algorithm>
#include <cstdio>
#include <system_error>

#include "AssetSlot.h"
#include "Localization.h"
#include "Thumbnails.h"
#include "sage/ui/ImageFit.h"

namespace fs = std::filesystem;

namespace sage::editor::covers {

FitRect Fit(const ImVec2& a, const ImVec2& b, int w, int h) {
    const float boxW = b.x - a.x;
    const float boxH = b.y - a.y;
    if (w <= 0 || h <= 0 || boxW <= 0.0f || boxH <= 0.0f) return {a, b};
    // ТА ЖЕ арифметика, что у картинки интерфейса в режиме «сохранять
    // пропорции» (sage/ui/ImageFit.h): «вписать, сохраняя пропорции» не может
    // означать в обложке одно, а в элементе другое.
    const sage::ui::ImagePlacement p =
        sage::ui::PlaceImage(sage::ui::UIRect{a.x, a.y, boxW, boxH}, 0.0f, 0.0f, (float)w, (float)h,
                             /*cover=*/false, /*pixelArt=*/false);
    return {ImVec2(p.Dst.x, p.Dst.y), ImVec2(p.Dst.x + p.Dst.w, p.Dst.y + p.Dst.h)};
}

void DrawChecker(ImDrawList* dl, const ImVec2& a, const ImVec2& b) {
    const float step = 8.0f;
    dl->PushClipRect(a, b, true);
    for (float y = a.y; y < b.y; y += step) {
        for (float x = a.x; x < b.x; x += step) {
            const bool odd = ((int)((x - a.x) / step) + (int)((y - a.y) / step)) % 2;
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + step, y + step),
                              odd ? IM_COL32(68, 68, 74, 255) : IM_COL32(50, 50, 56, 255));
        }
    }
    dl->PopClipRect();
}

void DrawSpinner(ImDrawList* dl, const ImVec2& center, float radius, ImU32 color) {
    const float t = (float)ImGui::GetTime() * 3.2f;
    dl->PathClear();
    dl->PathArcTo(center, radius, t, t + 4.2f, 20);
    dl->PathStroke(color, 0, 2.0f);
}

std::string HumanSize(uintmax_t bytes) {
    char buf[64];
    if (bytes < 1024) std::snprintf(buf, sizeof(buf), T("%llu B"), (unsigned long long)bytes);
    else if (bytes < 1024 * 1024) std::snprintf(buf, sizeof(buf), T("%.1f KiB"), bytes / 1024.0);
    else if (bytes < 1024ull * 1024 * 1024)
        std::snprintf(buf, sizeof(buf), T("%.1f MiB"), bytes / (1024.0 * 1024.0));
    else std::snprintf(buf, sizeof(buf), T("%.1f GiB"), bytes / (1024.0 * 1024.0 * 1024.0));
    return buf;
}

void DrawHoverPreview(const fs::path& full, bool isDir, const std::string& name, uintmax_t bytes,
                      AssetPreview* preview) {
    if (!ImGui::BeginTooltip()) return;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    // Больше половины окна превью быть не должно: оно всплывает под курсором и,
    // разросшись, закрывает собой тот самый список, по которому человек ведёт
    // мышь.
    const float maxW = std::min(560.0f, vp->Size.x * 0.42f);
    const float maxH = std::min(560.0f, vp->Size.y * 0.55f);

    bool drew = false;
    if (!isDir && thumbs::IsImage(full)) {
        // Крупную обложку просим отдельно, а мелкую показываем, ПОКА крупная
        // читается: пустота под курсором выглядит как «редактор задумался», а
        // растянутая мелкая — как «сейчас станет резче», и это правда.
        thumbs::Thumb t = thumbs::Get(full, thumbs::Size::Large);
        if (!t.Id) {
            const thumbs::Thumb small = thumbs::Get(full, thumbs::Size::Tile);
            if (small.Id) t = small;
        }
        if (t.Id && t.W > 0 && t.H > 0) {
            // Маленькую картинку НЕ РАСТЯГИВАЕМ до размеров превью: спрайт
            // 32x32, раздутый до полуэкрана, показывает не спрайт, а свои
            // пиксели. Увеличиваем не больше чем вчетверо.
            const float k = std::min({maxW / (float)t.W, maxH / (float)t.H, 4.0f});
            const ImVec2 size(std::max(16.0f, (float)t.W * k), std::max(16.0f, (float)t.H * k));
            const ImVec2 p = ImGui::GetCursorScreenPos();
            DrawChecker(ImGui::GetWindowDrawList(), p, ImVec2(p.x + size.x, p.y + size.y));
            ImGui::Image((ImTextureID)(std::intptr_t)t.Id, size, ImVec2(0, 1), ImVec2(1, 0));
            drew = true;
        } else if (t.Failed) {
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.4f, 1.0f), "%s", t.Error.c_str());
            drew = true;
        } else {
            ImGui::TextDisabled("%s", T("Reading..."));
            drew = true;
        }
    } else if (!isDir && preview) {
        // Материал, модель, префаб: их обложку надо СНЯТЬ, и делает это общий
        // кэш редактора (не больше одной съёмки за кадр, результат помнится).
        const uint64_t cover = assetslot::Cover(preview, full, 192);
        if (cover) {
            const float side = std::min(maxW, maxH) * 0.6f;
            ImGui::Image((ImTextureID)(std::intptr_t)cover, ImVec2(side, side), ImVec2(0, 1),
                         ImVec2(1, 0));
            drew = true;
        }
    }

    if (drew) ImGui::Spacing();
    ImGui::PushTextWrapPos(maxW);
    ImGui::TextUnformatted(name.c_str());
    ImGui::PopTextWrapPos();

    if (isDir) {
        ImGui::TextDisabled("%s", T("Folder"));
    } else {
        const thumbs::Thumb t =
            thumbs::IsImage(full) ? thumbs::Get(full, thumbs::Size::Tile) : thumbs::Thumb{};
        if (t.W > 0 && t.H > 0)
            ImGui::TextDisabled(T("%d x %d, %s"), t.W, t.H, HumanSize(bytes).c_str());
        else
            ImGui::TextDisabled("%s", HumanSize(bytes).c_str());
    }
    ImGui::EndTooltip();
}

} // namespace sage::editor::covers
