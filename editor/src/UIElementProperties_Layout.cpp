// ---------------------------------------------------------------------------
// Свойства элемента интерфейса — см. UIElementProperties.h.
//
// Раньше это была часть инспектора (InspectorPanel_Ui.cpp). Переехало сюда,
// когда у полей появился второй читатель — редактор интерфейса: два окна
// правят один и тот же набор компонентов, и держать для этого два списка
// полей значило бы разойтись на первой же новой галке.
// ---------------------------------------------------------------------------
#include "UIElementProperties.h"
#include "EditorTheme.h"

#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "imgui.h"

#include "AssetPreview.h"
#include "AssetSlot.h"
#include "SignalLinksEditor.h"
#include "VarsEditor.h"
#include "EditorHost.h"
#include "EditorIcons.h"
#include "FileBrowser.h"
#include "Localization.h"
#include "Project.h"
#include "sage/core/Log.h"
#include "sage/render/ResourceManager.h"
#include "sage/scene/Components.h"
#include "sage/ui/UI.h"
#include "sage/ui/UISceneSystem.h"
#include "sage/ui/UIPart.h"
#include "sage/ui/UIIcons.h"
#include "sage/ui/UIPresets.h"
#include "sage/ui/UISerialize.h"
#include "sage/ui/UIStyle.h"
#include "sage/core/Paths.h"
#include "ui/ColorPicker.h"

namespace fs = std::filesystem;

namespace sage::editor {
namespace {

void HintWrapped(const char* fmt, ...) {
    // ПОЯСНЕНИЕ — ВСПЛЫВАЮЩЕЕ, А НЕ ТЕКСТОМ В ПАНЕЛИ. Серые абзацы под каждым
    // полем превращали инспектор в справочник: нужное поле терялось среди
    // объяснений. Теперь пояснение появляется при наведении на тот элемент,
    // к которому относится (стоящий перед вызовом).
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    EditorTheme::Hint(buf);
}

} // namespace

namespace uiprops_detail {

void DrawGroupSettings(EditorHost& host, entt::entity e);

// Стиль из файла, раскладка контейнера, скины и группы элемента интерфейса.
// --- Стиль из файла -------------------------------------------------------------
//
// Слот файла стиля и два действия: записать нынешний вид в файл (новый или
// тот, что выбран) и отвязать. Правка файла доходит до всех элементов,
// которые на него ссылаются (sage::ui::ApplyStyles), — прямо в открытой сцене.
void DrawStyleRow(EditorHost& host, GameObject obj, const UIPropsContext& ctx, ui::Element& xf) {
    const assetslot::Result r =
        assetslot::Draw(host, "##ui_style", assetslot::Kind::UiStyle, xf.Style, ctx.Preview,
                        T("Style file: the look shared by many elements"));
    if (r.Changed) {
        host.PushUndoSnapshot();
        xf.Style = r.Path;
        xf.StyleVersion = -1;   // положить на следующем кадре
    }
    entt::registry& reg = host.CurrentScene().Registry();
    auto save = [&](const std::string& rel) {
        const fs::path root = assetslot::ProjectRoot(host);
        const std::string real = (root / sage::PathFromUtf8(rel)).string();
        std::error_code ec;
        fs::create_directories(fs::path(real).parent_path(), ec);
        std::string err;
        if (ui::SaveStyleFile(real, ui::CaptureStyle(reg, obj.Entity()), &err)) {
            host.SetStatusMessage(T("Style saved: ") + rel);
            return true;
        }
        host.SetStatusMessage(T("Could not save the style: ") + err);
        return false;
    };
    if (xf.Style.empty()) {
        if (ImGui::Button(T("Save this look as a style"))) {
            // Имя — по объекту, в папке styles проекта; занято — с номером.
            const fs::path root = assetslot::ProjectRoot(host);
            std::string base = obj.Name().empty() ? std::string("style") : obj.Name();
            for (char& c : base)
                if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
                    c == '>' || c == '|')
                    c = '_';
            std::string rel = "styles/" + base + ui::kStyleExtension;
            for (int n = 2; fs::exists(root / sage::PathFromUtf8(rel)) && n < 1000; ++n)
                rel = "styles/" + base + " " + std::to_string(n) + ui::kStyleExtension;
            if (save(rel)) {
                host.PushUndoSnapshot();
                xf.Style = rel;
                xf.StyleVersion = -1;
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", T("Writes the look of this element (not its text or value) to a\n"
                                      "file. Give the same file to other elements — editing it\n"
                                      "changes them all."));
        return;
    }
    if (ImGui::Button(T("Save changes to the style"))) {
        if (save(xf.Style)) xf.StyleVersion = -1;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("Every element with this style takes the new look."));
    ImGui::SameLine();
    if (ImGui::Button(T("Detach"))) {
        host.PushUndoSnapshot();
        xf.Style.clear();
        xf.StyleVersion = -1;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("Keeps the current look, but no longer follows the file."));
    HintWrapped("%s", T("The look comes from the style file: changes made here are replaced when the file changes."));
}

// --- Раздел «Где стоит» ------------------------------------------------------
void DrawLayoutSection(EditorHost& host, entt::entity e, ui::Element* xf, int selected) {
    entt::registry& reg = host.CurrentScene().Registry();
    (void)selected;

    // В КОНТЕЙНЕРЕ место считает контейнер: якорь и положение не действуют, и
    // показывать их значило бы обещать то, чего не происходит.
    const entt::entity parent = host.CurrentScene().ParentOf(e);
    const bool inStack = parent != entt::null && reg.all_of<ui::Stack>(parent);
    if (inStack) {
        ImGui::SeparatorText(T("In the container"));
        if (ImGui::Checkbox(T("Place by own anchor"), &xf->IgnoreLayout)) host.PushUndoSnapshot();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", T("Stand where the anchor puts it, outside the row: a badge in the\n"
                                      "corner of a list card."));
        if (!xf->IgnoreLayout) {
            ImGui::DragFloat(T("Grow"), &xf->Grow, 0.05f, 0.0f, 16.0f, "%.2f");
            host.TrackLastImGuiItem();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", T("A share of the room left in the row or column. 0 — keep own size;\n"
                                          "two children with 1 and 2 split the rest 1:2."));
            ImGui::DragFloat2(T("Size"), &xf->Size.x, 1.0f, 0.0f, 4096.0f);
            host.TrackLastImGuiItem();
            ImGui::Checkbox(T("Visible"), &xf->Visible);
            if (ImGui::TreeNodeEx(T("More settings"), ImGuiTreeNodeFlags_SpanAvailWidth)) {
                ImGui::DragInt(T("Order"), &xf->Order, 1);
                host.TrackLastImGuiItem();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Place in the row: lower comes first"));
                ImGui::DragFloat(T("Rotation"), &xf->Rotation, 1.0f, -180.0f, 180.0f, "%.1f\xc2\xb0");
                host.TrackLastImGuiItem();
                DrawGroupSettings(host, e);
                ImGui::TreePop();
            }
            return;
        }
        ImGui::SeparatorText(T("Position"));
    }

    if (DrawAnchorPicker(xf->Anchor)) host.PushUndoSnapshot();
    ImGui::DragFloat2(T("Position"), &xf->Position.x, 1.0f); host.TrackLastImGuiItem();

    const char* stretchNames[] = {T("None"), T("Horizontal"), T("Vertical"), T("Both")};
    int stretch = (int)xf->Mode;
    if (ImGui::Combo(T("Stretch"), &stretch, stretchNames, IM_ARRAYSIZE(stretchNames))) {
        host.PushUndoSnapshot();
        xf->Mode = (ui::Element::Stretch)stretch;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", T("Follows the parent size instead of a fixed one"));
    }
    const bool autoWidth = reg.all_of<ui::Label>(e) && reg.get<ui::Label>(e).AutoWidth;
    ImGui::BeginDisabled(autoWidth || xf->Mode == ui::Element::Stretch::Both);
    ImGui::DragFloat2(T("Size"), &xf->Size.x, 1.0f, 0.0f, 4096.0f); host.TrackLastImGuiItem();
    ImGui::EndDisabled();
    if (xf->Mode != ui::Element::Stretch::None) {
        ImGui::DragFloat4(T("Margin l,t,r,b"), &xf->Margin.x, 1.0f); host.TrackLastImGuiItem();
    }
    ImGui::Checkbox(T("Visible"), &xf->Visible);

    // Точка привязки, поворот, порядок, прозрачность — нужны реже, чем «где и
    // какого размера», и уходят под «Ещё».
    if (ImGui::TreeNodeEx(T("More settings"), ImGuiTreeNodeFlags_SpanAvailWidth)) {
        ImGui::DragFloat2(T("Pivot"), &xf->Pivot.x, 0.01f, 0.0f, 1.0f); host.TrackLastImGuiItem();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", T("Which point of the element lands on the anchor"));
        }
        // УГОЛ — ЧИСЛОМ, а не только мышью: «ровно 90» набирают здесь.
        ImGui::DragFloat(T("Rotation"), &xf->Rotation, 1.0f, -180.0f, 180.0f, "%.1f\xc2\xb0");
        host.TrackLastImGuiItem();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", T("Around the element centre. The handle above the top edge\n"
                                      "does the same with the mouse."));
        ImGui::DragInt(T("Order"), &xf->Order, 1); host.TrackLastImGuiItem();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Higher draws on top of its siblings"));
        DrawGroupSettings(host, e);
        ImGui::TreePop();
    }
}

// --- Оформление: движка или своё ---------------------------------------------------
//
// Две кнопки во всю ширину, а не список: выбор из двух, и видно оба сразу.
// Переход к своему оформлению ЗАПЕКАЕТ вид движка в поля (BakeEngineSkin):
// элемент на экране не меняется, а скругление, рамка и тень становятся теми
// числами, от которых удобно отталкиваться.
void DrawSkinSwitch(EditorHost& host, entt::entity e, ui::Element& xf) {
    entt::registry& reg = host.CurrentScene().Registry();
    const bool engine = xf.Skin == ui::Element::SkinMode::Engine;
    const float w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    auto choice = [&](const char* label, bool active) {
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::Color(EditorTheme::Role::Accent));
        const bool pressed = ImGui::Button(label, ImVec2(w, 0.0f));
        if (active) ImGui::PopStyleColor();
        return pressed && !active;
    };
    if (choice(T("Engine style"), engine)) {
        host.PushUndoSnapshot();
        xf.Skin = ui::Element::SkinMode::Engine;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("The engine draws the shape, border, shadow and states itself.\n"
                                  "You choose only colours — nothing else to tune."));
    ImGui::SameLine();
    if (choice(T("Custom"), !engine)) {
        host.PushUndoSnapshot();
        ui::BakeEngineSkin(reg, e);
        xf.Skin = ui::Element::SkinMode::Custom;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("Your own pictures for every part and state, 9-slice, every parameter.\n"
                                  "Starts from the engine look, so nothing jumps."));
}

// ПРОЗРАЧНОСТЬ И ВВОД ВСЕГО ПОДДЕРЕВА — общая настройка любого элемента, а не
// добавка, которую надо включить: плавно спрятать панель со всем содержимым —
// одно число.
void DrawGroupSettings(EditorHost& host, entt::entity e) {
    entt::registry& reg = host.CurrentScene().Registry();
    ui::Group* group = reg.try_get<ui::Group>(e);
    float alpha = group ? group->Alpha : 1.0f;
    if (ImGui::SliderFloat(T("Opacity"), &alpha, 0.0f, 1.0f, "%.2f")) {
        if (!group) group = &reg.emplace<ui::Group>(e);
        group->Alpha = alpha;
    }
    host.TrackLastImGuiItem();
    bool input = group ? group->Interactable : true;
    if (ImGui::Checkbox(T("Catches the mouse"), &input)) {
        host.PushUndoSnapshot();
        if (!group) group = &reg.emplace<ui::Group>(e);
        group->Interactable = input;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("Off — this element and everything inside ignore the mouse."));
}

} // namespace uiprops_detail
} // namespace sage::editor
