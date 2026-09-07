#include "ProjectCard.h"

#include <algorithm>
#include <cmath>
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

// Значок типа проекта — тот же набор, что во всём редакторе.
const char* KindIcon(ProjectKind kind) {
    switch (kind) {
        case ProjectKind::Scene: return "scene";
        case ProjectKind::Sample: return "material";
        case ProjectKind::Template: return "copy";
        default: return "cube";
    }
}

// Метка типа поверх обложки: тёмная подложка и мелкий текст. Рисунком, а не
// компонентом Badge: она лежит ПОВЕРХ картинки, то есть в абсолютных
// координатах, и элементом ImGui быть не должна — иначе перехватит клик у
// карточки.
void DrawKindTag(ImDrawList* dl, const ImVec2& pos, ProjectKind kind, float fontSize) {
    const char* label = ProjectKindLabel(kind);
    const ImVec2 size = ImGui::GetFont()->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, label);
    const Sage::UI::Style& ui = Sage::UI::Get();
    const ImVec2 pad(ui.SpacingSM, ui.SpacingXS * 0.75f);
    const ImVec2 max(pos.x + size.x + pad.x * 2.0f, pos.y + size.y + pad.y * 2.0f);
    dl->AddRectFilled(pos, max, EditorTheme::Color32Alpha(Role::Bg, 0.72f), ui.CornerRadiusSmall);
    TextAt(dl, ImVec2(pos.x + pad.x, pos.y + pad.y), fontSize,
           EditorTheme::Color32(Role::TextDim), label);
}

// «•••» — три точки в правом верхнем углу. Одинаково в сетке и в списке.
bool DotsButton(const ImVec2& center, float box) {
    ImGui::SetCursorScreenPos(ImVec2(center.x - box * 0.5f, center.y - box * 0.5f));
    const bool pressed = ImGui::InvisibleButton("##menu", ImVec2(box, box));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hovered) {
        dl->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                          EditorTheme::Color32(Role::Hover), Sage::UI::Get().CornerRadiusSmall);
    }
    const ImU32 col = EditorTheme::Color32(hovered ? Role::Text : Role::TextDim);
    const float step = std::max(3.0f, box * 0.18f);
    for (int i = -1; i <= 1; ++i)
        dl->AddCircleFilled(ImVec2(center.x + (float)i * step, center.y), 1.5f, col, 6);
    return pressed;
}

} // namespace

// ---------------------------------------------------------------------------
//  Размеры
// ---------------------------------------------------------------------------

int CardColumns(float availWidth, float targetWidth, float gap) {
    if (availWidth <= 0.0f || targetWidth <= 0.0f) return 1;
    const int columns = (int)std::floor((availWidth + gap) / (targetWidth + gap));
    return std::max(1, columns);
}

float CardWidth(float availWidth, int columns, float gap) {
    columns = std::max(1, columns);
    return std::floor((availWidth - gap * (float)(columns - 1)) / (float)columns);
}

float CardHeight(CardLayout layout, float width) {
    const Sage::UI::Style& ui = Sage::UI::Get();
    const float line = ImGui::GetTextLineHeight();
    if (layout == CardLayout::List) {
        // Строка: две строки текста и поля. Обложка вписывается в эту высоту,
        // а не задаёт её — иначе список перестал бы быть плотным.
        return std::floor(line * 2.0f + ui.SpacingMD * 2.0f);
    }
    // Сетка: обложка 16:9 плюс подпись из трёх строк.
    return std::floor(width * 9.0f / 16.0f + line * 3.0f + ui.SpacingSM * 2.0f + ui.SpacingMD);
}

// ---------------------------------------------------------------------------
//  Отрисовка
// ---------------------------------------------------------------------------

CardEvent DrawProjectCard(CardLayout layout, const ProjectEntry& entry, bool selected,
                          const ImVec2& size, ProjectThumbnail& thumbs) {
    const Sage::UI::Style& ui = Sage::UI::Get();
    CardEvent ev;

    ImGui::PushID(entry.Path.c_str());
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);

    // Вся карточка — один невидимый элемент. AllowOverlap нужен, чтобы кнопка
    // «•••», поданная ПОСЛЕ неё и поверх неё, получала свои нажатия: иначе
    // меню открывалось бы через раз, а карточка «моргала» выбором под ним.
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##card", size);
    const bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) ev.Selected = true;
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) ev.Activated = true;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        ev.Selected = true;
        ev.MenuRequested = true;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float radius = ui.CornerRadius;
    const ImU32 bg = EditorTheme::Color32(hovered ? Role::Elevated : Role::SurfaceAlt);
    dl->AddRectFilled(p0, p1, bg, radius);
    // Рамка — ТОНКАЯ и всегда: по ней читается граница карточки на тёмном фоне.
    // Выбранная получает акцент, а не заливку: жёлтым в этом окне светится
    // ровно одна кнопка — «Открыть проект».
    dl->AddRect(p0, p1, selected ? EditorTheme::Color32(Role::Accent)
                                 : EditorTheme::Color32(Role::Line),
                radius, 0, selected ? 1.6f : 1.0f);

    const float caption = std::max(10.0f, ImGui::GetFontSize() * 0.82f);
    const float body = ImGui::GetFontSize();
    const ImU32 nameCol = EditorTheme::Color32(entry.Missing ? Role::TextFaint : Role::Text);
    const ImU32 dimCol = EditorTheme::Color32(Role::TextDim);
    const ImU32 faintCol = EditorTheme::Color32(Role::TextFaint);

    const std::string modified =
        entry.Missing ? std::string(T("The folder is gone"))
                      : std::string(T("Last modified: ")) + HumanStamp(entry.Modified);

    if (layout == CardLayout::Grid) {
        const float shotH = std::floor(size.x * 9.0f / 16.0f);
        const ImVec2 shot1(p1.x, p0.y + shotH);
        dl->PushClipRect(p0, shot1, true);
        thumbs.Draw(entry, p0, shot1, radius);
        dl->PopClipRect();
        // Линия под обложкой: без неё картинка и подпись сливаются в одно пятно.
        dl->AddLine(ImVec2(p0.x, shot1.y), ImVec2(p1.x, shot1.y),
                    EditorTheme::Color32(Role::Line), 1.0f);
        DrawKindTag(dl, ImVec2(p0.x + ui.SpacingSM, p0.y + ui.SpacingSM), entry.Kind, caption);

        float y = shot1.y + ui.SpacingSM;
        const float textX = p0.x + ui.SpacingMD;
        const float textW = size.x - ui.SpacingMD * 2.0f - ui.IconSize;
        EditorIcons::Overlay(textX, y + (body - ui.IconSize) * 0.5f, ui.IconSize,
                             KindIcon(entry.Kind),
                             glm::vec3(EditorTheme::Color(Role::TextDim).x,
                                       EditorTheme::Color(Role::TextDim).y,
                                       EditorTheme::Color(Role::TextDim).z));
        TextAt(dl, ImVec2(textX + ui.IconSize + ui.SpacingXS, y), body, nameCol,
               TruncateAt(entry.Name, textW, body));
        y += ImGui::GetTextLineHeight() + ui.SpacingXS;
        TextAt(dl, ImVec2(textX, y), caption, faintCol,
               TruncateAt(entry.Path, size.x - ui.SpacingMD * 2.0f, caption));
        y += ImGui::GetTextLineHeight() * 0.9f + ui.SpacingXS;
        TextAt(dl, ImVec2(textX, y), caption, dimCol,
               TruncateAt(modified, size.x - ui.SpacingMD * 2.0f, caption));
    } else {
        // Список: та же информация в одну строку. Обложка маленькая — она здесь
        // примета, а не картинка.
        const float inner = size.y - ui.SpacingSM * 2.0f;
        const ImVec2 shot0(p0.x + ui.SpacingSM, p0.y + ui.SpacingSM);
        const ImVec2 shot1(shot0.x + inner * 16.0f / 9.0f, shot0.y + inner);
        dl->PushClipRect(shot0, shot1, true);
        thumbs.Draw(entry, shot0, shot1, ui.CornerRadiusSmall);
        dl->PopClipRect();

        const float textX = shot1.x + ui.SpacingMD;
        // Справа — время правки и метка типа; имя и путь занимают остаток.
        const float tagW = ImGui::GetFont()->CalcTextSizeA(caption, FLT_MAX, 0.0f,
                                                           ProjectKindLabel(entry.Kind)).x +
                           ui.SpacingMD;
        const float stampW = ImGui::GetFont()->CalcTextSizeA(caption, FLT_MAX, 0.0f,
                                                             modified.c_str()).x;
        // Место под кнопку «•••» вычитается ЗДЕСЬ, а не «на глаз»: без этого
        // дата упиралась в три точки и читалась как «21:44•••».
        const float dotsRoom = ImGui::GetFrameHeight() * 0.8f + ui.SpacingMD;
        const float rightX = p1.x - dotsRoom - ui.SpacingSM - stampW;
        const float textW = std::max(60.0f, rightX - textX - tagW - ui.SpacingMD);

        EditorIcons::Overlay(textX, p0.y + size.y * 0.5f - ui.IconSize * 0.5f, ui.IconSize,
                             KindIcon(entry.Kind),
                             glm::vec3(EditorTheme::Color(Role::TextDim).x,
                                       EditorTheme::Color(Role::TextDim).y,
                                       EditorTheme::Color(Role::TextDim).z));
        const float nameX = textX + ui.IconSize + ui.SpacingXS;
        TextAt(dl, ImVec2(nameX, p0.y + ui.SpacingSM), body, nameCol,
               TruncateAt(entry.Name, textW, body));
        TextAt(dl, ImVec2(nameX, p0.y + ui.SpacingSM + ImGui::GetTextLineHeight()), caption,
               faintCol, TruncateAt(entry.Path, textW, caption));
        TextAt(dl, ImVec2(rightX, p0.y + size.y * 0.5f - ImGui::GetTextLineHeight() * 0.45f),
               caption, dimCol, modified);
        DrawKindTag(dl, ImVec2(rightX - tagW - ui.SpacingMD,
                               p0.y + size.y * 0.5f - ImGui::GetTextLineHeight() * 0.55f),
                    entry.Kind, caption);
    }

    // Кнопка меню — последней, поверх карточки.
    const float dots = ImGui::GetFrameHeight() * 0.8f;
    const ImVec2 after = ImGui::GetCursorScreenPos();
    if (DotsButton(ImVec2(p1.x - dots * 0.75f - ui.SpacingXS, p0.y + dots * 0.75f + ui.SpacingXS),
                   dots)) {
        ev.Selected = true;
        ev.MenuRequested = true;
    }
    Sage::UI::Tooltip(T("Project actions"));
    ImGui::SetCursorScreenPos(after);
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
    if (ImGui::MenuItem(T("Open"))) action = ProjectAction::Open;
    if (ImGui::MenuItem(T("Open Folder"))) action = ProjectAction::OpenFolder;
    ImGui::Separator();
    if (ImGui::MenuItem(T("Rename..."))) action = ProjectAction::Rename;
    if (ImGui::MenuItem(T("Duplicate..."))) action = ProjectAction::Duplicate;
    ImGui::EndDisabled();
    ImGui::Separator();
    if (ImGui::MenuItem(T("Remove from Launcher"))) action = ProjectAction::Forget;
    ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(Role::Danger));
    if (ImGui::MenuItem(T("Delete Project..."))) action = ProjectAction::Delete;
    ImGui::PopStyleColor();
    Sage::UI::Tooltip(T("Deletes the project folder from disk"));
    return action;
}

} // namespace Sage::Launcher
