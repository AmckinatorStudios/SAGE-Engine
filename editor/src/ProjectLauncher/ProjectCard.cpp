#include "ProjectCard.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

#include "imgui_internal.h"

#include "../EditorIcons.h"
#include "../EditorTheme.h"
#include "../Localization.h"
#include "../ui/UI.h"
#include "ProjectDatabase.h"
#include "ProjectThumbnail.h"

namespace Sage::Launcher {

namespace {

using EditorTheme::Role;

// Обрезка под ширину НА ЗАДАННОМ КЕГЛЕ. Sage::UI::Truncate считает по текущему
// шрифту кадра, а подпись и путь на карточке идут мельче основного текста.
std::string TruncateAt(const std::string& text, float maxWidth, float fontSize) {
    if (text.empty() || maxWidth <= 0.0f) return {};
    ImFont* font = ImGui::GetFont();
    auto width = [&](const std::string& s) {
        return font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, s.c_str()).x;
    };
    if (width(text) <= maxWidth) return text;
    const std::string dots = "...";
    const float dotsW = width(dots);
    std::string out;
    const char* p = text.c_str();
    while (*p) {
        const char* next = p + 1;
        while ((*next & 0xC0) == 0x80) ++next;   // хвостовые байты символа UTF-8
        std::string probe = out + std::string(p, next);
        if (width(probe) + dotsW > maxWidth) break;
        out = std::move(probe);
        p = next;
    }
    return out.empty() ? dots : out + dots;
}

void TextAt(ImDrawList* dl, const ImVec2& pos, float size, ImU32 color, const std::string& text) {
    if (text.empty()) return;
    dl->AddText(ImGui::GetFont(), size, pos, color, text.c_str());
}

} // namespace

// ---------------------------------------------------------------------------
//  Время
// ---------------------------------------------------------------------------

std::string RelativeTime(long long unixTime) {
    if (unixTime <= 0) return T("Not opened yet");
    const long long now = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::system_clock::now().time_since_epoch()).count();
    const long long d = std::max(0LL, now - unixTime);
    char buf[64];
    if (d < 60) return T("Just now");
    if (d < 3600) {
        std::snprintf(buf, sizeof(buf), T("%d min ago"), (int)(d / 60));
        return buf;
    }
    if (d < 86400) {
        std::snprintf(buf, sizeof(buf), T("%d h ago"), (int)(d / 3600));
        return buf;
    }
    if (d < 2 * 86400) return T("Yesterday");
    if (d < 30LL * 86400) {
        std::snprintf(buf, sizeof(buf), T("%d days ago"), (int)(d / 86400));
        return buf;
    }
    return FormatStamp(unixTime);
}

// ---------------------------------------------------------------------------
//  Строка
// ---------------------------------------------------------------------------

float RowHeight() {
    const Sage::UI::Style& ui = Sage::UI::Get();
    // Две строки текста и поля; обложка вписывается в высоту, а не задаёт её —
    // иначе список перестал бы быть плотным.
    return std::floor(ImGui::GetTextLineHeight() * 2.0f + ui.SpacingMD * 2.0f);
}

CardEvent DrawProjectRow(const ProjectEntry& entry, bool selected, float width,
                         ProjectThumbnail& thumbs) {
    const Sage::UI::Style& ui = Sage::UI::Get();
    CardEvent ev;
    const ImVec2 size(width, RowHeight());

    ImGui::PushID(entry.Path.c_str());
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);

    ImGui::InvisibleButton("##row", size);
    const bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) ev.Selected = true;
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) ev.Activated = true;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        ev.Selected = true;
        ev.MenuRequested = true;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Выбранная строка — подложкой, без рамок и акцентной заливки: жёлтым в окне
    // светится одна кнопка («Новый проект»).
    if (selected) dl->AddRectFilled(p0, p1, EditorTheme::Color32(Role::Selection), ui.CornerRadius);
    else if (hovered) dl->AddRectFilled(p0, p1, EditorTheme::Color32(Role::Hover), ui.CornerRadius);

    const float caption = std::max(10.0f, ImGui::GetFontSize() * 0.82f);
    const float body = ImGui::GetFontSize();
    const ImU32 nameCol = EditorTheme::Color32(entry.Missing ? Role::TextFaint : Role::Text);
    const ImU32 dimCol = EditorTheme::Color32(Role::TextDim);
    const ImU32 faintCol = EditorTheme::Color32(Role::TextFaint);

    const std::string stamp =
        entry.Missing ? std::string(T("The folder is gone")) : RelativeTime(entry.Opened);

    const float inner = size.y - ui.SpacingSM * 2.0f;
    const ImVec2 shot0(p0.x + ui.SpacingSM, p0.y + ui.SpacingSM);
    const ImVec2 shot1(shot0.x + inner * 16.0f / 9.0f, shot0.y + inner);
    dl->PushClipRect(shot0, shot1, true);
    thumbs.Draw(entry, shot0, shot1, ui.CornerRadiusSmall);
    dl->PopClipRect();

    const float textX = shot1.x + ui.SpacingMD;
    const float stampW = ImGui::GetFont()->CalcTextSizeA(caption, FLT_MAX, 0.0f, stamp.c_str()).x;
    const float rightX = p1.x - ui.SpacingMD - stampW;
    const float textW = std::max(60.0f, rightX - textX - ui.SpacingMD);
    const float mid = p0.y + size.y * 0.5f;
    const float lineH = ImGui::GetTextLineHeight();

    TextAt(dl, ImVec2(textX, mid - lineH - ui.SpacingXS * 0.25f), body, nameCol,
           TruncateAt(entry.Name, textW, body));
    TextAt(dl, ImVec2(textX, mid + ui.SpacingXS * 0.25f), caption, faintCol,
           TruncateAt(entry.Path, textW, caption));
    TextAt(dl, ImVec2(rightX, mid - lineH * 0.45f), caption, dimCol, stamp);

    ImGui::PopID();
    return ev;
}

// ---------------------------------------------------------------------------
//  Контекстное меню
// ---------------------------------------------------------------------------

ProjectAction DrawProjectMenuItems(const ProjectEntry& entry) {
    ProjectAction action = ProjectAction::None;
    // Отсутствующий проект нельзя ни открыть, ни скопировать — но можно убрать
    // из списка. Выключенные пункты честнее спрятанных: человек видит, что
    // действие существует, и понимает, почему оно сейчас недоступно.
    ImGui::BeginDisabled(entry.Missing);
    if (EditorIcons::MenuItem("open", T("Open"))) action = ProjectAction::Open;
    if (EditorIcons::MenuItem("folder", T("Open Folder"))) action = ProjectAction::OpenFolder;
    ImGui::Separator();
    if (EditorIcons::MenuItem("pencil", T("Rename..."))) action = ProjectAction::Rename;
    if (EditorIcons::MenuItem("copy", T("Duplicate..."))) action = ProjectAction::Duplicate;
    ImGui::EndDisabled();
    ImGui::Separator();
    if (EditorIcons::MenuItem("eye-off", T("Remove from Launcher"))) action = ProjectAction::Forget;
    ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(Role::Danger));
    if (EditorIcons::MenuItem("trash", T("Delete Project..."))) action = ProjectAction::Delete;
    ImGui::PopStyleColor();
    Sage::UI::Tooltip(T("Deletes the project folder from disk"));
    return action;
}

} // namespace Sage::Launcher
