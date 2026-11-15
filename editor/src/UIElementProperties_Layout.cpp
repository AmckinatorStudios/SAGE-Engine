// ---------------------------------------------------------------------------
// Свойства элемента интерфейса — см. UIElementProperties.h.
//
// Раньше это была часть инспектора (InspectorPanel_Ui.cpp). Переехало сюда,
// когда у полей появился второй читатель — редактор интерфейса: два окна
// правят один и тот же набор компонентов, и держать для этого два списка
// полей значило бы разойтись на первой же новой галке.
// ---------------------------------------------------------------------------
#include "UIElementProperties.h"
#include "InspectorControls.h"
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
#include <cstdint>
#include <unordered_map>

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

namespace ictl = sage::editor::ictl;

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
// --- APPEARANCE SOURCE: откуда у элемента вид ------------------------------
//
// Одно поле вместо двух больших кнопок и отдельного слота стиля:
//   Engine Default — вид движка (свои значения хранятся, но не правятся);
//   Saved Style    — файл стиля (компактный слот появляется ниже);
//   Custom         — свои настройки элемента.
// Переход в Custom впервые запекает вид движка в поля (элемент на экране не
// меняется); дальше — возвращает свои значения, а не затирает их.
namespace {
// Выбран Saved Style, а файл ещё не назначен: режим держится до выбора.
std::unordered_map<std::uint32_t, bool>& PendingStyle() {
    static std::unordered_map<std::uint32_t, bool> s;
    return s;
}
} // namespace

void DrawStyleRow(EditorHost& host, GameObject obj, const UIPropsContext& ctx, ui::Element& xf);

void DrawAppearanceSource(EditorHost& host, GameObject obj, const UIPropsContext& ctx, ui::Element& xf) {
    entt::registry& reg = host.CurrentScene().Registry();
    const std::uint32_t key = (std::uint32_t)obj.Entity();
    enum { kEngine, kStyle, kCustom };
    int mode = !xf.Style.empty() || PendingStyle()[key] ? kStyle
               : xf.Skin == ui::Element::SkinMode::Engine ? kEngine
                                                           : kCustom;
    const char* kModes[] = {T("Engine Default"), T("Saved Style"), T("Custom")};
    ictl::Label(T("Source"));
    if (ictl::Combo("##appsrc", &mode, kModes, 3)) {
        host.PushUndoSnapshot();
        PendingStyle()[key] = mode == kStyle && xf.Style.empty();
        ui::SetAppearanceSource(reg, obj.Entity(), mode == kEngine  ? ui::AppearanceSource::EngineDefault
                                                   : mode == kStyle ? ui::AppearanceSource::SavedStyle
                                                                    : ui::AppearanceSource::Custom);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", mode == kEngine ? T("The standard SAGE look: only colours are yours")
                                : mode == kStyle ? T("A saved style file shared by many elements")
                                                 : T("Settings of this element only"));
    if (mode == kStyle) {
        DrawStyleRow(host, obj, ctx, xf);
        if (!xf.Style.empty()) PendingStyle().erase(key);
    } else if (mode == kCustom) {
        DrawStyleRow(host, obj, ctx, xf);   // «Save as Style…» — небольшой командой
    }
}

void DrawStyleRow(EditorHost& host, GameObject obj, const UIPropsContext& ctx, ui::Element& xf) {
    const bool styled = xf.Skin == ui::Element::SkinMode::Custom &&
                        (!xf.Style.empty() || PendingStyle()[(std::uint32_t)obj.Entity()]);
    if (styled) {
        const assetslot::Result r =
            assetslot::Draw(host, "##ui_style", assetslot::Kind::UiStyle, xf.Style, ctx.Preview,
                            T("Style file: the look shared by many elements"));
        if (r.Changed) {
            host.PushUndoSnapshot();
            xf.Style = r.Path;
            xf.StyleVersion = -1;   // положить на следующем кадре
        }
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
        if (ImGui::Button(T("Save as Style..."))) {
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
    // Два действия над связью со стилем — одной группой kit.
    ictl::GroupItem acts[2];
    acts[0].Icon = "save";
    acts[0].Text = T("Save changes to the style");
    acts[0].Tooltip = T("Every element with this style takes the new look.");
    acts[1].Icon = "unlock";
    acts[1].Text = T("Detach");
    acts[1].Tooltip = T("Keeps the current look, but no longer follows the file.");
    const int act = ictl::ButtonGroup("##style_acts", acts, 2, ictl::ButtonLook::Compact);
    if (act == 0 && save(xf.Style)) xf.StyleVersion = -1;
    if (act == 1) {
        host.PushUndoSnapshot();
        xf.Style.clear();
        xf.StyleVersion = -1;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("The look comes from the style file: changes made here are replaced when the file changes."));
}

// --- PLACEMENT: где стоит и какого размера --------------------------------
//
// Одна из главных групп — поэтому высоко и открыта. Поля — едиными
// контролами (ictl): X/Y подписаны, размер — раздельно или «Ш × В» со связью
// пропорций, поля от краёв — по сторонам со связью.
void DrawLayoutSection(EditorHost& host, entt::entity e, ui::Element* xf, int selected) {
    entt::registry& reg = host.CurrentScene().Registry();
    (void)selected;
    const ictl::NumOpts px = [] {
        ictl::NumOpts o;
        o.HasReset = true;
        return o;
    }();
    ictl::NumOpts size = px;
    size.Min = 0.0f;
    size.Max = 8192.0f;
    size.HasReset = false;

    // В КОНТЕЙНЕРЕ место считает контейнер: якорь и положение не действуют, и
    // показывать их значило бы обещать то, чего не происходит.
    const entt::entity parent = host.CurrentScene().ParentOf(e);
    const ui::Arrangement* parentArr = parent != entt::null ? reg.try_get<ui::Arrangement>(parent) : nullptr;
    const bool inStack = parentArr && parentArr->Arranges();
    if (inStack) {
        ictl::Switch(host, T("Place by own anchor"), &xf->IgnoreLayout);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", T("Stand where the anchor puts it, outside the row: a badge in the\n"
                                      "corner of a list card."));
        if (!xf->IgnoreLayout) {
            ictl::NumOpts grow = ictl::OptsForRange(0.0f, 16.0f);
            grow.HasReset = true;
            ictl::Number(host, T("Grow"), &xf->Grow, grow);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", T("A share of the room left in the row or column. 0 — keep own size;\n"
                                          "two children with 1 and 2 split the rest 1:2."));
            ictl::Size(host, T("Size"), &xf->Size, size);
            ictl::Switch(host, T("Visible"), &xf->Visible);
            return;
        }
    }

    if (DrawAnchorPicker(xf->Anchor)) host.PushUndoSnapshot();
    const char* stretchNames[] = {T("None"), T("Horizontal"), T("Vertical"), T("Both")};
    int stretch = (int)xf->Mode;
    ictl::Label(T("Stretch"));
    if (ictl::Combo("##Stretch", &stretch, stretchNames, IM_ARRAYSIZE(stretchNames))) {
        host.PushUndoSnapshot();
        xf->Mode = (ui::Element::Stretch)stretch;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Follows the parent size instead of a fixed one"));
    ictl::Position(host, T("Position"), &xf->Position, px);
    // Размер по растянутой оси задают поля от краёв — тогда поле размера не
    // нужно вовсе (скрыто, а не серое).
    const bool autoWidth = reg.all_of<ui::Label>(e) && reg.get<ui::Label>(e).AutoWidth;
    if (!autoWidth && xf->Mode != ui::Element::Stretch::Both) ictl::Size(host, T("Size"), &xf->Size, size);
    if (xf->Mode != ui::Element::Stretch::None) ictl::Sides(host, T("Margin"), &xf->Margin, px);
    ictl::Switch(host, T("Visible"), &xf->Visible);
}

// --- ADVANCED элемента: редкое -------------------------------------------
//
// Точка привязки, поворот, порядок, слой, прозрачность и ввод поддерева,
// наложение и адаптивные пределы — внизу, в закрытом по умолчанию разделе.
void DrawElementAdvanced(EditorHost& host, entt::entity e, ui::Element* xf) {
    ictl::NumOpts pivot = ictl::OptsForRange(0.0f, 1.0f);
    pivot.HasReset = true;
    pivot.Reset = 0.5f;
    ictl::GroupTitle(T("Pivot"));
    static const char* const kXY[2] = {"X", "Y"};
    ictl::Axes(host, "pivot", &xf->Pivot.x, 2, kXY, pivot);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Which point of the element lands on the anchor"));
    ictl::NumOpts deg = ictl::OptsForRange(-180.0f, 180.0f);
    deg.HasReset = true;
    deg.Format = "%.1f\xc2\xb0";
    ictl::Number(host, T("Rotation"), &xf->Rotation, deg);
    ictl::Integer(host, T("Order"), &xf->Order, -100000, 100000, true, 0);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Higher draws on top of its siblings"));
    ictl::Integer(host, T("Draw layer"), &xf->DrawLayer, -1000, 1000, true, 0);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("Above the whole interface, not only the siblings: a higher layer is\n"
                                  "drawn over everything with a lower one. 0 — as the parent."));
    DrawGroupSettings(host, e);

    // АДАПТИВНОЕ РАЗМЕЩЕНИЕ: пределы размера, пропорции и безопасная область.
    ictl::NumOpts lim;
    lim.Min = 0.0f;
    lim.Max = 8192.0f;
    lim.HasReset = true;
    ictl::Size(host, T("Min size"), &xf->MinSize, lim);
    ictl::Size(host, T("Max size"), &xf->MaxSize, lim);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("Keeps a stretched element within sensible limits on any screen. 0 — no limit."));
    ictl::NumOpts aspect = ictl::OptsForRange(0.0f, 16.0f);
    aspect.Step = 0.01f;
    aspect.Format = "%.3f";
    aspect.HasReset = true;
    ictl::Number(host, T("Aspect ratio"), &xf->AspectRatio, aspect);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("Width divided by height: the element fits its place keeping this shape. 0 — free."));
    ictl::Switch(host, T("Inside the safe area"), &xf->SafeArea);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("Anchors and stretching count from the safe area of the screen, not its edges:\n"
                                  "a notch or a TV frame does not cover the element."));
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
    // Группа kit из двух сегментов во всю ширину: выбранный — подсвечен.
    ictl::GroupItem items[2];
    items[0].Text = T("Engine style");
    items[0].Active = engine;
    items[0].Tooltip = T("The engine draws the shape, border, shadow and states itself.\n"
                         "You choose only colours — nothing else to tune.");
    items[1].Text = T("Custom");
    items[1].Active = !engine;
    items[1].Tooltip = T("Your own pictures for every part and state, 9-slice, every parameter.\n"
                         "Starts from the engine look, so nothing jumps.");
    const int hit = ictl::ButtonGroup("##skin", items, 2, ictl::ButtonLook::Standard, 0,
                                      ImGui::GetContentRegionAvail().x);
    if (hit == 0 && !engine) {
        host.PushUndoSnapshot();
        xf.Skin = ui::Element::SkinMode::Engine;
    } else if (hit == 1 && engine) {
        host.PushUndoSnapshot();
        ui::BakeEngineSkin(reg, e);
        xf.Skin = ui::Element::SkinMode::Custom;
    }
}

// ПРОЗРАЧНОСТЬ И ВВОД ВСЕГО ПОДДЕРЕВА — общая настройка любого элемента, а не
// добавка, которую надо включить: плавно спрятать панель со всем содержимым —
// одно число.
void DrawGroupSettings(EditorHost& host, entt::entity e) {
    entt::registry& reg = host.CurrentScene().Registry();
    ui::Group* group = reg.try_get<ui::Group>(e);
    float alpha = group ? group->Alpha : 1.0f;
    if (ictl::SliderNumber(host, T("Opacity"), &alpha, 0.0f, 1.0f, "%.2f", true, 1.0f)) {
        if (!group) group = &reg.emplace<ui::Group>(e);
        group->Alpha = alpha;
    }
    bool input = group ? group->Interactable : true;
    if (ictl::Switch(host, T("Catches the mouse"), &input)) {
        if (!group) group = &reg.emplace<ui::Group>(e);
        group->Interactable = input;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("Off — this element and everything inside ignore the mouse."));

    // Как элемент ложится на фон (UIBlend.h): инверсия — прицел, видный на
    // любом фоне; умножение — затемнение; экран — осветление.
    if (ui::Element* xf = reg.try_get<ui::Element>(e)) {
        const UIBlend modes[] = {UIBlend::Normal, UIBlend::Additive, UIBlend::Multiply, UIBlend::Screen,
                                 UIBlend::Invert};
        const char* labels[] = {T("Normal"), T("Add (glow)"), T("Multiply"), T("Screen"), T("Invert background")};
        int cur = 0;
        for (int i = 0; i < 5; ++i)
            if (modes[i] == xf->Blend) cur = i;
        ictl::Label(T("Blend"));
        if (ictl::BeginDropdown("##Blend", labels[cur])) {
            for (int i = 0; i < 5; ++i) {
                if (ImGui::Selectable(labels[i], i == cur) && i != cur) {
                    host.PushUndoSnapshot();
                    xf->Blend = modes[i];
                }
            }
            ictl::EndDropdown();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", T("How the element lays over what is behind it.\n"
                                      "Invert background — like a crosshair that stays visible on any surface."));
    }
}

} // namespace uiprops_detail
} // namespace sage::editor
