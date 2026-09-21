#include "AssetCovers.h"

#include <algorithm>
#include <cstdio>
#include <system_error>

#include "AssetSlot.h"
#include "EditorIcons.h"
#include "SceneCover.h"
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

PreviewSource SourceOf(const fs::path& full, bool isDir, bool hasPreview,
                       const fs::path& projectDir) {
    if (isDir) return PreviewSource::Icon;
    if (thumbs::IsImage(full)) return PreviewSource::Image;
    if (full.extension() == ".sage") {
        // Снимок сцены лежит РЯДОМ С ПРОЕКТОМ, а не рядом со сценой: без папки
        // проекта его не найти, и тогда честнее показать значок, чем пустоту.
        if (projectDir.empty()) return PreviewSource::Icon;
        std::error_code ec;
        const fs::path cover = scenecover::For(projectDir, full);
        if (cover.empty() || !fs::exists(cover, ec)) return PreviewSource::Icon;
        return PreviewSource::SceneCover;
    }
    if (!hasPreview) return PreviewSource::Icon;
    // Съёмка бывает только у того, что есть чем нарисовать.
    const assetslot::Kind kind = assetslot::KindOf(full);
    if (kind == assetslot::Kind::Material || kind == assetslot::Kind::Prefab ||
        kind == assetslot::Kind::Model)
        return PreviewSource::Rendered;
    return PreviewSource::Icon;
}

void DrawHoverPreview(const fs::path& full, bool isDir, const std::string& name, uintmax_t bytes,
                      AssetPreview* preview, const fs::path& projectDir) {
    if (!ImGui::BeginTooltip()) return;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    // ПЛОЩАДКА ПРЕВЬЮ — ПОСТОЯННАЯ И КВАДРАТНАЯ.
    //
    // Раньше размер превью задавала САМА картинка: её увеличивали до четырёх
    // раз и обрезали по потолку. Отсюда две беды разом. Первая: окошко прыгало
    // размером от файла к файлу — панорама открывалась широкой лентой, иконка
    // 32x32 крошечным квадратиком, и глазу приходилось заново искать, куда
    // смотреть, на каждом движении мыши. Вторая: по такому превью нельзя
    // сравнить два файла — они показаны в разном масштабе.
    //
    // Теперь площадка одна на все файлы, а картинка вписывается в неё по своим
    // пропорциям: положение и размер окна не зависят от того, что внутри.
    const float side = std::min({320.0f, vp->Size.x * 0.32f, vp->Size.y * 0.42f});
    const float maxW = side;

    const ImVec2 box0 = ImGui::GetCursorScreenPos();
    const ImVec2 box1(box0.x + side, box0.y + side);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Подложка площадки: по ней видно её границы, даже когда картинка ниже и
    // уже — то есть видно, что поля пустые, а не что превью «съехало».
    dl->AddRectFilled(box0, box1, ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.30f)), 6.0f);

    // Что показывать: картинка — сама собой, сцена — своим снимком, модель,
    // материал и префаб — снятой обложкой. Остальное (скрипт, звук, шрифт,
    // папка) обложки не имеет — там значок типа, и это тоже ответ: раньше у
    // таких файлов превью не появлялось вовсе, и выглядело это как «у одних
    // работает, у других нет».
    uint64_t cover = 0;
    int coverW = 0, coverH = 0;
    bool failed = false, loading = false;
    std::string error;

    const PreviewSource source = SourceOf(full, isDir, preview != nullptr, projectDir);
    fs::path imageFile;
    if (source == PreviewSource::Image) imageFile = full;
    else if (source == PreviewSource::SceneCover) imageFile = scenecover::For(projectDir, full);

    if (!imageFile.empty()) {
        // Крупную обложку просим отдельно, а мелкую показываем, ПОКА крупная
        // читается: пустота под курсором выглядит как «редактор задумался», а
        // мелкая — как «сейчас станет резче», и это правда.
        thumbs::Thumb t = thumbs::Get(imageFile, thumbs::Size::Large);
        if (!t.Id) {
            const thumbs::Thumb small = thumbs::Get(imageFile, thumbs::Size::Tile);
            if (small.Id) t = small;
        }
        cover = t.Id;
        coverW = t.W;
        coverH = t.H;
        failed = t.Failed;
        error = t.Error;
        loading = !cover && !failed;
    } else if (source == PreviewSource::Rendered) {
        // Материал, модель, префаб: их обложку надо СНЯТЬ, и делает это общий
        // кэш редактора (не больше одной съёмки за кадр, результат помнится).
        // Кадр съёмки квадратный, поэтому пропорции 1:1.
        cover = assetslot::Cover(preview, full, 192);
        coverW = coverH = cover ? 1 : 0;
    }

    if (cover && coverW > 0 && coverH > 0) {
        const FitRect r = Fit(box0, box1, coverW, coverH);
        DrawChecker(dl, r.A, r.B);
        dl->AddImage((ImTextureID)(std::intptr_t)cover, r.A, r.B, ImVec2(0, 1), ImVec2(1, 0));
    } else if (loading) {
        DrawSpinner(dl, ImVec2((box0.x + box1.x) * 0.5f, (box0.y + box1.y) * 0.5f), side * 0.09f,
                    ImGui::GetColorU32(ImGuiCol_TextDisabled));
    } else {
        // Значок типа во всю площадку — тот же набор, что в панели и слотах.
        const char* icon = isDir ? "folder" : assetslot::KindIcon(assetslot::KindOf(full));
        const float glyph = std::floor(side * 0.42f);
        EditorIcons::Overlay(std::floor(box0.x + (side - glyph) * 0.5f),
                             std::floor(box0.y + (side - glyph) * 0.5f), glyph, icon,
                             glm::vec3(0.62f, 0.66f, 0.74f));
    }
    // Место под площадку занимаем НЕВИДИМОЙ кнопкой: рисовали мы списком
    // отрисовки, и без неё ImGui считал бы, что в подсказке ничего нет, —
    // подпись легла бы поверх картинки.
    ImGui::Dummy(ImVec2(side, side));
    const bool drew = true;
    if (failed && !error.empty()) ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.4f, 1.0f), "%s",
                                                     error.c_str());

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
