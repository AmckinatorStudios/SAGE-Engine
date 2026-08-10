// ---------------------------------------------------------------------------
// Инспектор — элементы интерфейса.
//
// Правка элемента интерфейса: якорь, растяжение, вид, поведение. Отдельно от
// свойств обычной сущности потому, что элемент UI — это НАБОР компонентов
// (раскладка, вид, поведение), и его инспектор устроен иначе, чем список
// компонентов трёхмерного объекта.
//
// Часть класса InspectorPanel: объявления остались в InspectorPanel.h, здесь
// только тела. Разбит потому, что дорос до двух тысяч строк, в которых рядом
// лежали редактор материала, якоря интерфейса и список компонентов.
// ---------------------------------------------------------------------------
#include <cstdarg>
#include "InspectorPanel.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <system_error>
#include <memory>
#include <string>
#include <vector>

#include "imgui.h"

#include "EditorHost.h"
#include "sage/core/Log.h"
#include <algorithm>

#include "AssetSlot.h"
#include "EditorIcons.h"
#include "ModelMaterialImport.h"
#include "Project.h"
#include "sage/render/ResourceManager.h"
#include "sage/assets/import/Importer.h"
#include "sage/render/ModelLoader.h"
#include "sage/render/ModelMaterial.h"
#include "sage/assets/AssetDatabase.h"
#include "sage/render/ParticlePresets.h"
#include "sage/render/SkinnedModel.h"
#include "sage/scene/Components.h"
#include "sage/ui/UI.h"
#include "sage/ui/UIIcons.h"
#include "sage/ui/UIPresets.h"
#include "../Localization.h"

namespace fs = std::filesystem;

// Кнопка «Add Component» + попап со списком компонентов, которых у сущности ещё
// нет. Так добавление унифицировано (а удаление — кнопкой Remove в самой секции).
// Серое пояснение, КОТОРОЕ ПЕРЕНОСИТСЯ. Панель инспектора узкая (её ширину
// задаёт раскладка доккинга), а TextDisabled не переносит — поэтому пояснения
// обрезались по краю панели прямо посередине слова: «габарит 1.00 x 1.00 x 1.0(»,
// «задаём вид объекта целик». Пропадал ровно тот текст, ради которого их писали.
void InspectorPanel::HintWrapped(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrappedV(fmt, args);
    ImGui::PopStyleColor();
    va_end(args);
}


// Якорь — сеткой 3x3, а не списком из девяти строк.
//
// Якорь ЕСТЬ положение на экране: «сверху слева», «по центру», «снизу справа».
// Выпадающий список заставлял читать девять названий и держать в голове, какое
// из них соответствует нужному углу; сетка показывает это буквально — кнопка
// стоит там же, где встанет элемент.
bool InspectorPanel::DrawAnchorPicker(UIAnchor& anchor) {
    bool changed = false;
    const float cell = ImGui::GetFrameHeight();
    ImGui::BeginGroup();
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            const int index = row * 3 + col;
            if (col) ImGui::SameLine(0.0f, 2.0f);
            ImGui::PushID(index);
            const bool active = (int)anchor == index;
            if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.216f, 0.322f, 0.520f, 1.0f));
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            if (ImGui::Button("##anchor", ImVec2(cell, cell))) {
                anchor = (UIAnchor)index;
                changed = true;
            }
            if (active) ImGui::PopStyleColor();
            // Точка внутри кнопки — в том углу, который эта кнопка и означает.
            const float px = p0.x + cell * (0.25f + 0.25f * (float)col);
            const float py = p0.y + cell * (0.25f + 0.25f * (float)row);
            ImGui::GetWindowDrawList()->AddCircleFilled(
                ImVec2(px, py), 2.5f,
                ImGui::GetColorU32(active ? ImGuiCol_Text : ImGuiCol_TextDisabled));
            ImGui::PopID();
        }
    }
    ImGui::EndGroup();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(T("Anchor"));
    return changed;
}

// Инспектор элемента интерфейса.
//
// РАНЬШЕ ЗДЕСЬ БЫЛА ПРОСТЫНЯ ИЗ ТРИДЦАТИ ПЯТИ ПОЛЕЙ, одна на все виды
// элементов. У надписи спрашивали скругление углов, толщину рамки, градиент,
// тень и внутренний отступ; у полосы прогресса — подсказку поля ввода; у любого
// элемента — предел длины текста и пароль. Работающими из них были три-четыре,
// а какие именно — приходилось выяснять опытом. Отсюда и ощущение «компоненты
// странные»: компонент один, а элементов интерфейса восемь, и он показывал
// объединение всех их свойств сразу.
//
// Теперь показывается только то, что этот вид элемента в самом деле читает
// (см. sage/ui/UIRenderer.h), сгруппированное по смыслу: положение, вид, текст,
// поведение. Сам компонент не тронут: он общий для всех видов и сериализуется
// как раньше — иначе пришлось бы ломать формат сцен, скриптовый API и префабы
// ради вида инспектора.
void InspectorPanel::DrawUIElement(EditorHost& host, GameObject obj) {
    entt::registry& reg = host.CurrentScene().Registry();
    const entt::entity e = obj.Entity();
    namespace ui = sage::ui;

    ui::Transform* xf = reg.try_get<ui::Transform>(e);
    if (!xf) return;

    // --- Заготовка ----------------------------------------------------------
    //
    // Собрать кнопку — значит поставить четыре компонента с нужными значениями.
    // Знать это наизусть человек не обязан: заготовка делает всё разом, и это
    // ТА ЖЕ заготовка, что у скриптов (sage/ui/UI.h).
    if (ImGui::BeginCombo(T("Preset"), T("Apply a preset..."))) {
        for (const std::string& name : ui::PresetNames()) {
            if (ImGui::Selectable(name.c_str())) {
                host.PushUndoSnapshot();
                ui::ApplyPreset(reg, e, name);
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", T("Replaces the element parts with the preset ones"));
    }

    // --- Части элемента -----------------------------------------------------
    //
    // Здесь видно, ИЗ ЧЕГО элемент сделан, и здесь же это меняется. Раньше на
    // этом месте стоял выпадающий список «вид элемента» из восьми значений, и
    // собрать шкалу с иконкой и подписью было нельзя: вид один, а нужно три.
    ImGui::SeparatorText(T("Parts"));
    auto partToggle = [&](const char* label, auto tag, const char* hint) {
        using T_ = decltype(tag);
        bool on = reg.all_of<T_>(e);
        if (ImGui::Checkbox(label, &on)) {
            host.PushUndoSnapshot();
            if (on) reg.emplace<T_>(e);
            else reg.remove<T_>(e);
        }
        if (hint && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", hint);
    };
    partToggle(T("Fill"), ui::Fill{}, T("Rounded background, border, gradient, shadow"));
    ImGui::SameLine();
    partToggle(T("Label"), ui::Label{}, T("Text on the element"));
    ImGui::SameLine();
    partToggle(T("Image"), ui::Image{}, T("Texture or a sprite from a sheet"));
    partToggle(T("Bar"), ui::Bar{}, T("Progress or health bar"));
    ImGui::SameLine();
    partToggle(T("Icon"), ui::Icon{}, T("Vector icon of the engine"));
    ImGui::SameLine();
    partToggle(T("Interactable"), ui::Interactable{}, T("Reacts to the mouse: hover, press, click"));
    partToggle(T("Text Input"), ui::TextInput{}, T("Editable text field"));
    ImGui::SameLine();
    partToggle(T("Range"), ui::Range{}, T("Slider or checkbox value"));
    ImGui::SameLine();
    partToggle(T("Mask"), ui::Mask{}, T("The subtree is masked by this rectangle"));
    partToggle(T("Layout"), ui::Layout{}, T("Lays the children out by itself"));
    ImGui::SameLine();
    partToggle(T("Canvas"), ui::Canvas{}, T("Scaling rules for this UI root"));
    ImGui::SameLine();
    partToggle(T("Group"), ui::Group{}, T("Alpha and input for the whole subtree"));

    // --- Положение ----------------------------------------------------------
    ImGui::SeparatorText(T("Layout"));
    if (DrawAnchorPicker(xf->Anchor)) host.PushUndoSnapshot();
    ImGui::DragFloat2(T("Offset"), &xf->Offset.x, 1.0f); host.TrackLastImGuiItem();

    const char* stretchNames[] = {T("None"), T("Horizontal"), T("Vertical"), T("Both")};
    int stretch = (int)xf->Mode;
    if (ImGui::Combo(T("Stretch"), &stretch, stretchNames, IM_ARRAYSIZE(stretchNames))) {
        host.PushUndoSnapshot();
        xf->Mode = (ui::Transform::Stretch)stretch;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", T("Follows the parent size instead of a fixed one"));
    }
    const bool autoWidth = reg.all_of<ui::Label>(e) && reg.get<ui::Label>(e).AutoWidth;
    ImGui::BeginDisabled(autoWidth || xf->Mode == ui::Transform::Stretch::Both);
    ImGui::DragFloat2(T("Size"), &xf->Size.x, 1.0f, 0.0f, 4096.0f); host.TrackLastImGuiItem();
    ImGui::EndDisabled();
    if (xf->Mode != ui::Transform::Stretch::None) {
        ImGui::DragFloat4(T("Margin l,t,r,b"), &xf->Margin.x, 1.0f); host.TrackLastImGuiItem();
    }
    ImGui::DragFloat2(T("Pivot"), &xf->Pivot.x, 0.01f, 0.0f, 1.0f); host.TrackLastImGuiItem();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", T("Which point of the element lands on the anchor"));
    }
    ImGui::DragInt(T("Layer"), &xf->Layer, 1); host.TrackLastImGuiItem();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Higher draws on top of its siblings"));
    ImGui::Checkbox(T("Visible"), &xf->Visible);

    // --- Подложка -----------------------------------------------------------
    if (ui::Fill* fill = reg.try_get<ui::Fill>(e)) {
        ImGui::SeparatorText(T("Fill"));
        ImGui::ColorEdit4(T("Fill Color"), &fill->Color.x); host.TrackLastImGuiItem();
        ImGui::DragFloat(T("Rounding"), &fill->Rounding, 0.5f, 0.0f, 200.0f); host.TrackLastImGuiItem();
        ImGui::DragFloat(T("Border"), &fill->BorderThickness, 0.25f, 0.0f, 50.0f); host.TrackLastImGuiItem();
        if (fill->BorderThickness > 0.0f) {
            ImGui::ColorEdit4(T("Border Color"), &fill->BorderColor.x); host.TrackLastImGuiItem();
        }
        ImGui::ColorEdit4(T("Gradient (a=0 off)"), &fill->Gradient.x); host.TrackLastImGuiItem();
        ImGui::DragFloat(T("Shadow"), &fill->ShadowSize, 0.5f, 0.0f, 64.0f); host.TrackLastImGuiItem();
    }

    // --- Надпись ------------------------------------------------------------
    if (ui::Label* label = reg.try_get<ui::Label>(e)) {
        ImGui::SeparatorText(T("Text"));
        char textBuf[256];
        std::snprintf(textBuf, sizeof(textBuf), "%s", label->Text.c_str());
        if (ImGui::InputText(T("Text"), textBuf, sizeof(textBuf))) label->Text = textBuf;
        host.TrackLastImGuiItem();
        ImGui::DragFloat(T("Text Scale"), &label->Scale, 0.05f, 0.5f, 12.0f); host.TrackLastImGuiItem();
        ImGui::ColorEdit4(T("Text Color"), &label->Color.x); host.TrackLastImGuiItem();

        // Выравнивание — по ДВУМ осям: прижать подпись к правому краю панели
        // прежней галкой «по центру» было нельзя вовсе.
        const char* aligns[] = {T("Start"), T("Center"), T("End")};
        int h = (int)label->Horizontal, v = (int)label->Vertical;
        if (ImGui::Combo(T("Horizontal"), &h, aligns, IM_ARRAYSIZE(aligns))) {
            host.PushUndoSnapshot();
            label->Horizontal = (ui::Label::Align)h;
        }
        if (ImGui::Combo(T("Vertical"), &v, aligns, IM_ARRAYSIZE(aligns))) {
            host.PushUndoSnapshot();
            label->Vertical = (ui::Label::Align)v;
        }
        ImGui::Checkbox(T("Wrap"), &label->Wrap);
        ImGui::SameLine();
        ImGui::Checkbox(T("Auto Width"), &label->AutoWidth);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Width follows the content"));
        ImGui::DragFloat(T("Padding X"), &label->PadX, 0.5f, 0.0f, 64.0f); host.TrackLastImGuiItem();
    }

    // --- Картинка -----------------------------------------------------------
    if (ui::Image* image = reg.try_get<ui::Image>(e)) {
        ImGui::SeparatorText(T("Image"));
        auto loadTexture = [&] {
            image->Tex = image->Path.empty()
                             ? nullptr
                             : image->PixelArt
                                   ? ResourceManager::Instance().GetTexture(
                                         image->Path, TextureFilter::Nearest, false)
                                   : ResourceManager::Instance().GetTexture(image->Path);
        };
        ImGui::ColorEdit4(T("Tint"), &image->Tint.x); host.TrackLastImGuiItem();
        // Тот же слот, что у мешей и материалов: перетащить картинку из Assets,
        // увидеть её тут же, узнать, где она лежит.
        const assetslot::Result r = assetslot::Draw(host, "ui_tex", assetslot::Kind::Texture,
                                                    image->Path, &m_preview);
        if (r.Changed) {
            host.PushUndoSnapshot();
            image->Path = r.Path;
            loadTexture();
        }
        if (r.BrowseRequested) {
            FileBrowser::Config c;
            c.Title = T("Choose an image");
            c.Filters = assetslot::Extensions(assetslot::Kind::Texture);
            c.FilterLabel = T("Images");
            if (host.CurrentProject().Loaded()) c.StartDir = host.CurrentProject().AssetsDir();
            m_browser.Open(c);
            m_browseTarget = &image->Path;
            m_browseIsShader = false;
            m_browseIsMesh = false;
            m_browseIsMaterial = false;
        }
        if (!image->Path.empty() && !image->Tex) {
            ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "%s", T("Texture not loaded (press Load)"));
            ImGui::SameLine();
            if (EditorIcons::Button("refresh", T("Load"))) {
                host.PushUndoSnapshot();
                loadTexture();
            }
        }
        // Пиксель-арт меняет ФИЛЬТРАЦИЮ загруженной текстуры, поэтому
        // перезагружаем сразу: иначе галка стоит, а картинка мыльная до
        // следующего Load, и это выглядит как «галка не работает».
        if (ImGui::Checkbox(T("Pixel Art (nearest, no mips)"), &image->PixelArt)) {
            host.PushUndoSnapshot();
            loadTexture();
        }
        if (image->Tex) ImGui::TextDisabled(T("Sheet: %d x %d px"), image->Tex->Width(),
                                            image->Tex->Height());

        if (ImGui::TreeNode(T("Sprite from a sheet"))) {
            ImGui::DragFloat4(T("Sprite x,y,w,h"), &image->Sprite.x, 1.0f, 0.0f, 8192.0f);
            host.TrackLastImGuiItem();
            ImGui::TextDisabled("%s", T("w or h = 0 — the whole file"));
            ImGui::DragFloat4(T("9-slice l,t,r,b"), &image->SliceBorder.x, 0.5f, 0.0f, 512.0f);
            host.TrackLastImGuiItem();
            ImGui::DragFloat(T("Pixel Scale"), &image->PixelScale, 0.25f, 0.0f, 16.0f);
            host.TrackLastImGuiItem();
            ImGui::TextDisabled("%s", T("0 — stretch to fit; >0 — source pixel size"));
            ImGui::DragFloat4(T("Hover sprite"), &image->SpriteHover.x, 1.0f, 0.0f, 8192.0f);
            host.TrackLastImGuiItem();
            ImGui::DragFloat4(T("Pressed sprite"), &image->SpritePressed.x, 1.0f, 0.0f, 8192.0f);
            host.TrackLastImGuiItem();
            ImGui::TreePop();
        }
    }

    // --- Полоса -------------------------------------------------------------
    if (ui::Bar* bar = reg.try_get<ui::Bar>(e)) {
        ImGui::SeparatorText(T("Bar"));
        ImGui::SliderFloat(T("Value"), &bar->Value, 0.0f, 1.0f); host.TrackLastImGuiItem();
        ImGui::ColorEdit4(T("Fill Color"), &bar->FillColor.x); host.TrackLastImGuiItem();
        const char* grows[] = {T("Left to right"), T("Right to left"), T("Bottom to top"),
                               T("Top to bottom")};
        int grow = (int)bar->Grow;
        if (ImGui::Combo(T("Direction"), &grow, grows, IM_ARRAYSIZE(grows))) {
            host.PushUndoSnapshot();
            bar->Grow = (ui::Bar::Direction)grow;
        }
        ImGui::DragFloat(T("Smoothing"), &bar->Smoothing, 0.1f, 0.0f, 20.0f);
        host.TrackLastImGuiItem();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", T("Units per second; 0 — jump straight to the value"));
        }
    }

    // --- Значок -------------------------------------------------------------
    if (ui::Icon* icon = reg.try_get<ui::Icon>(e)) {
        ImGui::SeparatorText(T("Icon"));
        const std::string current = icon->Name.empty() ? T("(none)") : icon->Name;
        if (ImGui::BeginCombo(T("Icon"), current.c_str())) {
            if (ImGui::Selectable(T("(none)"), icon->Name.empty())) {
                host.PushUndoSnapshot();
                icon->Name.clear();
            }
            for (const std::string& name : sage::ui::IconNames()) {
                if (ImGui::Selectable(name.c_str(), name == icon->Name)) {
                    host.PushUndoSnapshot();
                    icon->Name = name;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::ColorEdit4(T("Icon Color"), &icon->Color.x); host.TrackLastImGuiItem();
    }

    // --- Поведение ----------------------------------------------------------
    if (ui::Interactable* act = reg.try_get<ui::Interactable>(e)) {
        ImGui::SeparatorText(T("Interaction"));
        ImGui::Checkbox(T("Enabled"), &act->Enabled);
        char actionBuf[128];
        std::snprintf(actionBuf, sizeof(actionBuf), "%s", act->Action.c_str());
        if (ImGui::InputText(T("Action"), actionBuf, sizeof(actionBuf))) act->Action = actionBuf;
        host.TrackLastImGuiItem();
        if (ImGui::IsItemHovered()) {
            // Именно этого не хватало: связь кнопки с логикой держалась на
            // номере сущности, а он меняется при любой пересборке сцены.
            ImGui::SetTooltip("%s", T("Name the game asks for instead of an entity number"));
        }
        ImGui::DragFloat(T("Hover brightness"), &act->HoverBrightness, 0.01f, 0.5f, 2.0f);
        host.TrackLastImGuiItem();
        ImGui::DragFloat(T("Pressed brightness"), &act->PressedBrightness, 0.01f, 0.5f, 2.0f);
        host.TrackLastImGuiItem();
    }
    if (ui::TextInput* field = reg.try_get<ui::TextInput>(e)) {
        ImGui::SeparatorText(T("Text Input"));
        char phBuf[256];
        std::snprintf(phBuf, sizeof(phBuf), "%s", field->Placeholder.c_str());
        if (ImGui::InputText(T("Placeholder"), phBuf, sizeof(phBuf))) field->Placeholder = phBuf;
        host.TrackLastImGuiItem();
        ImGui::DragInt(T("Max Length"), &field->MaxLength, 1, 0, 4096); host.TrackLastImGuiItem();
        ImGui::Checkbox(T("Password"), &field->Password);
        ImGui::SameLine();
        ImGui::Checkbox(T("Read Only"), &field->ReadOnly);
    }
    if (ui::Range* range = reg.try_get<ui::Range>(e)) {
        ImGui::SeparatorText(T("Range"));
        ImGui::Checkbox(T("Toggle (checkbox)"), &range->Toggle);
        ImGui::DragFloat(T("Min Value"), &range->Min, 0.1f); host.TrackLastImGuiItem();
        ImGui::DragFloat(T("Max Value"), &range->Max, 0.1f); host.TrackLastImGuiItem();
        ImGui::SliderFloat(T("Value"), &range->Value, glm::min(range->Min, range->Max),
                           glm::max(range->Min, range->Max));
        host.TrackLastImGuiItem();
        ImGui::DragFloat(T("Step"), &range->Step, 0.01f, 0.0f, 100.0f); host.TrackLastImGuiItem();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("0 — smooth; >0 — snap to the step"));
    }

    // --- Контейнеры ---------------------------------------------------------
    if (ui::Mask* mask = reg.try_get<ui::Mask>(e)) {
        ImGui::SeparatorText(T("Mask"));
        ImGui::DragFloat4(T("Padding l,t,r,b"), &mask->Padding.x, 1.0f); host.TrackLastImGuiItem();
        ImGui::Checkbox(T("Show outside"), &mask->ShowOutside);
    }
    if (ui::Layout* layout = reg.try_get<ui::Layout>(e)) {
        ImGui::SeparatorText(T("Layout"));
        const char* flows[] = {T("Horizontal"), T("Vertical"), T("Grid")};
        int flow = (int)layout->Direction;
        if (ImGui::Combo(T("Direction"), &flow, flows, IM_ARRAYSIZE(flows))) {
            host.PushUndoSnapshot();
            layout->Direction = (ui::Layout::Flow)flow;
        }
        const char* justifies[] = {T("Start"), T("Center"), T("End"), T("Space between")};
        int justify = (int)layout->Justify;
        if (ImGui::Combo(T("Justify"), &justify, justifies, IM_ARRAYSIZE(justifies))) {
            host.PushUndoSnapshot();
            layout->Justify = (ui::Layout::Align)justify;
        }
        ImGui::DragFloat(T("Spacing"), &layout->Spacing, 0.5f, 0.0f, 200.0f); host.TrackLastImGuiItem();
        ImGui::DragFloat4(T("Padding l,t,r,b"), &layout->Padding.x, 1.0f); host.TrackLastImGuiItem();
        if (layout->Direction == ui::Layout::Flow::Grid) {
            ImGui::DragInt(T("Columns"), &layout->Columns, 1, 1, 64); host.TrackLastImGuiItem();
        }
        ImGui::Checkbox(T("Stretch across"), &layout->StretchCross);
        ImGui::SameLine();
        ImGui::Checkbox(T("Fit content"), &layout->FitContent);
    }
    if (ui::Canvas* canvas = reg.try_get<ui::Canvas>(e)) {
        ImGui::SeparatorText(T("Canvas"));
        const char* modes[] = {T("Pixels"), T("Scale with screen size")};
        int mode = (int)canvas->Mode;
        if (ImGui::Combo(T("Scale mode"), &mode, modes, IM_ARRAYSIZE(modes))) {
            host.PushUndoSnapshot();
            canvas->Mode = (ui::Canvas::Scale)mode;
        }
        ImGui::DragFloat2(T("Reference"), &canvas->Reference.x, 1.0f, 320.0f, 7680.0f);
        host.TrackLastImGuiItem();
        ImGui::SliderFloat(T("Match width/height"), &canvas->MatchWidthOrHeight, 0.0f, 1.0f);
        host.TrackLastImGuiItem();
        ImGui::DragInt(T("Sort order"), &canvas->SortOrder, 1); host.TrackLastImGuiItem();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("HUD under the pause menu"));
    }
    if (ui::Group* group = reg.try_get<ui::Group>(e)) {
        ImGui::SeparatorText(T("Group"));
        ImGui::SliderFloat(T("Alpha"), &group->Alpha, 0.0f, 1.0f); host.TrackLastImGuiItem();
        ImGui::Checkbox(T("Interactable"), &group->Interactable);
        ImGui::SameLine();
        ImGui::Checkbox(T("Block raycasts"), &group->BlockRaycasts);
    }

    ImGui::Separator();
    // Вёрстка правится прямо в кадре — рамками и ручками поверх картинки (см.
    // UICanvas). Кнопка здесь потому, что элемент, которого не видно, ищут
    // именно отсюда.
    ImGui::Checkbox(T("Edit layout in the viewport"), &host.UIEditMode());
    if (ImGui::Button(T("Remove UI Element"))) {
        host.PushUndoSnapshot();
        reg.remove<ui::Transform, ui::Fill, ui::Label, ui::Image, ui::Bar, ui::Icon,
                   ui::Interactable, ui::TextInput, ui::Range, ui::Mask, ui::Layout, ui::Canvas,
                   ui::Group>(e);
    }
}
