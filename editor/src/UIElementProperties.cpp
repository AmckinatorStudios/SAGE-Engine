// ---------------------------------------------------------------------------
// Свойства элемента интерфейса — см. UIElementProperties.h.
//
// Раньше это была часть инспектора (InspectorPanel_Ui.cpp). Переехало сюда,
// когда у полей появился второй читатель — редактор интерфейса: два окна
// правят один и тот же набор компонентов, и держать для этого два списка
// полей значило бы разойтись на первой же новой галке.
// ---------------------------------------------------------------------------
#include "UIElementProperties.h"

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
#include "EditorHost.h"
#include "EditorIcons.h"
#include "FileBrowser.h"
#include "Localization.h"
#include "Project.h"
#include "sage/core/Log.h"
#include "sage/render/ResourceManager.h"
#include "sage/scene/Components.h"
#include "sage/ui/UI.h"
#include "sage/ui/UIPart.h"
#include "sage/ui/UIIcons.h"
#include "sage/ui/UIPresets.h"

namespace fs = std::filesystem;

namespace sage::editor {

namespace {

// Серое пояснение, КОТОРОЕ ПЕРЕНОСИТСЯ: панель узкая, а TextDisabled не
// переносит — пояснения обрезались по краю прямо посередине слова.
void HintWrapped(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrappedV(fmt, args);
    ImGui::PopStyleColor();
    va_end(args);
}

} // namespace

// Якорь — сеткой 3x3, а не списком из девяти строк.
//
// Якорь ЕСТЬ положение на экране: «сверху слева», «по центру», «снизу справа».
// Выпадающий список заставлял читать девять названий и держать в голове, какое
// из них соответствует нужному углу; сетка показывает это буквально — кнопка
// стоит там же, где встанет элемент.
bool DrawAnchorPicker(UIAnchor& anchor) {
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
// Одно поле части: виджет по типу, а особый — по просьбе самого поля.
//
// Отдельной функцией, потому что её зовут в цикле по реестру и она обязана
// оставаться единственным местом, где «тип поля» превращается в «элемент
// интерфейса редактора». Второе такое место — и части снова начнут
// расходиться с тем, чем их правят.
void DrawPartField(EditorHost& host, GameObject obj, const UIPropsContext& ctx,
                   const ui::PartType& part, const ui::PartField& f, void* data) {
    using K = ui::PartField::Kind;
    using W = ui::PartField::Widget;
    entt::registry& reg = host.CurrentScene().Registry();
    const entt::entity e = obj.Entity();
    bool changed = false;
    // Подписи и подсказки приходят из ТАБЛИЦЫ ДВИЖКА английскими ключами —
    // ровно как все остальные строки редактора, — и переводятся здесь.
    const char* label = T(f.Label);

    ImGui::PushID(f.Key);
    switch (f.Type) {
        case K::Bool:
            changed = ImGui::Checkbox(label, &ui::FieldAs<bool>(data, f));
            if (changed) host.PushUndoSnapshot();
            break;

        case K::Int: {
            int& v = ui::FieldAs<int>(data, f);
            ImGui::DragInt(label, &v, 1.0f, (int)f.Min, (int)f.Max);
            host.TrackLastImGuiItem();
            break;
        }

        case K::Float: {
            float& v = ui::FieldAs<float>(data, f);
            // Ползунок там, где границы осмысленны, и поле с перетаскиванием
            // там, где их нет: тащить «размер 0..4096» ползунком невозможно.
            if (f.Max > f.Min && f.Max - f.Min <= 64.0f) ImGui::SliderFloat(label, &v, f.Min, f.Max);
            else ImGui::DragFloat(label, &v, 0.5f, f.Min, f.Max);
            host.TrackLastImGuiItem();
            break;
        }

        case K::String: {
            std::string& v = ui::FieldAs<std::string>(data, f);
            if (f.Editor == W::Texture) {
                // Слот ассета: обложка, приём броска из панели Assets, «где
                // лежит», очистка. Печатать путь руками — то, ради чего слоты и
                // заведены.
                const assetslot::Result r =
                    assetslot::Draw(host, f.Key, assetslot::Kind::Texture, v, ctx.Preview,
                                    f.Tooltip ? T(f.Tooltip) : label);
                if (r.Changed) {
                    host.PushUndoSnapshot();
                    v = r.Path;
                    changed = true;
                }
                if (r.BrowseRequested && ctx.Browser) {
                    FileBrowser::Config c;
                    c.Title = T("Choose an image");
                    c.Filters = assetslot::Extensions(assetslot::Kind::Texture);
                    c.FilterLabel = T("Images");
                    c.StartDir = host.CurrentProject().AssetsDir();
                    ctx.Browser->Open(c);
                    if (ctx.BrowseTarget) *ctx.BrowseTarget = &v;
                }
            } else if (f.Editor == W::IconName) {
                // Значки выбираются ГЛАЗАМИ: список имён без картинок — это
                // угадывание, как выглядит «drop» и чем он отличается от «wire».
                if (ImGui::BeginCombo(label, v.empty() ? T("(none)") : v.c_str())) {
                    if (ImGui::Selectable(T("(none)"), v.empty())) {
                        host.PushUndoSnapshot();
                        v.clear();
                        changed = true;
                    }
                    for (const std::string& name : sage::ui::IconNames()) {
                        ImGui::PushID(name.c_str());
                        const bool sel = v == name;
                        EditorIcons::Inline("info");
                        ImGui::SameLine();
                        if (ImGui::Selectable(name.c_str(), sel)) {
                            host.PushUndoSnapshot();
                            v = name;
                            changed = true;
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
            } else {
                char buf[512];
                std::snprintf(buf, sizeof(buf), "%s", v.c_str());
                const bool multi = f.Editor == W::Multiline;
                if (multi ? ImGui::InputTextMultiline(label, buf, sizeof(buf),
                                                      ImVec2(0, ImGui::GetFrameHeight() * 2.2f))
                          : ImGui::InputText(label, buf, sizeof(buf))) {
                    v = buf;
                    changed = true;
                }
                host.TrackLastImGuiItem();
            }
            break;
        }

        case K::Color:
            ImGui::ColorEdit4(label, &ui::FieldAs<glm::vec4>(data, f).x);
            host.TrackLastImGuiItem();
            break;

        case K::Vec2:
            ImGui::DragFloat2(label, &ui::FieldAs<glm::vec2>(data, f).x, 1.0f, f.Min, f.Max);
            host.TrackLastImGuiItem();
            break;

        case K::Vec4:
            ImGui::DragFloat4(label, &ui::FieldAs<glm::vec4>(data, f).x, 1.0f, f.Min, f.Max);
            host.TrackLastImGuiItem();
            break;

        case K::Enum: {
            int& v = ui::FieldAs<int>(data, f);
            const char* preview =
                (f.EnumNames && v >= 0 && v < f.EnumCount) ? T(f.EnumNames[v]) : "?";
            if (ImGui::BeginCombo(label, preview)) {
                for (int i = 0; i < f.EnumCount; ++i) {
                    if (ImGui::Selectable(T(f.EnumNames[i]), i == v)) {
                        host.PushUndoSnapshot();
                        v = i;
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            break;
        }
    }
    if (f.Tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T(f.Tooltip));
    ImGui::PopID();

    // Картинке после смены пути нужен новый рантайм-указатель: иначе слот
    // показывает новый файл, а элемент рисует старый.
    if (changed && part.Id && std::string(part.Id) == "image") {
        if (ui::Image* im = reg.try_get<ui::Image>(e)) {
            im->Tex = im->Path.empty()
                          ? nullptr
                          : (im->PixelArt ? ResourceManager::Instance().GetTexture(
                                                im->Path, TextureFilter::Nearest, false)
                                          : ResourceManager::Instance().GetTexture(im->Path));
        }
    }
}

void DrawUIElementProperties(EditorHost& host, GameObject obj,
                             const UIPropsContext& ctx) {
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
                ui::ApplyPreset(host.CurrentScene(), e, name);
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
    // СПИСОК ЧАСТЕЙ — ИЗ РЕЕСТРА ДВИЖКА, а не из этого файла.
    //
    // Здесь стоял свой список из тринадцати галок. Он был четвёртым по счёту
    // списком одних и тех же частей (отрисовка, запись, чтение, редактор), и
    // добавить свою часть значило не забыть ни один. Теперь редактор не знает
    // ни одной части поимённо: он перебирает sage::ui::Parts(). Часть, которую
    // игра зарегистрировала у себя, появляется здесь сама.
    {
        int column = 0;
        for (const ui::PartType& p : ui::Parts()) {
            if (!p.Fields) continue; // служебные записи без полей человеку не нужны
            if (column++ % 3 != 0) ImGui::SameLine();
            bool on = p.Has(reg, e);
            if (ImGui::Checkbox(T(p.Title), &on)) {
                host.PushUndoSnapshot();
                if (on) p.Add(reg, e);
                else p.Remove(reg, e);
            }
            if (p.Hint && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T(p.Hint));
        }
        ImGui::NewLine();
    }

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

    // --- ПОЛЯ ЧАСТЕЙ — ПО ТАБЛИЦАМ, а не двести пятьдесят строк вручную ------
    //
    // Здесь были расписаны все части поимённо: у подложки семь полей, у текста
    // восемь, у картинки восемь, и так тринадцать раз. Это был ТРЕТИЙ список
    // тех же полей — после записи в файл и чтения из файла, — и разъезжались
    // они регулярно: поле есть в формате, а покрутить его нечем.
    //
    // Теперь поля берутся из таблицы, объявленной рядом с самой частью
    // (sage/ui/UIParts.cpp). Виджет выбирается по типу поля; там, где обычного
    // мало, поле само говорит какой нужен (PartField::Widget) — путь к
    // картинке просит слот ассета, имя значка просит список значков.
    for (const ui::PartType& p : ui::Parts()) {
        if (!p.Fields || !p.Has(reg, e)) continue;
        void* data = p.GetMutable(reg, e);
        if (!data) continue;

        ImGui::SeparatorText(T(p.Title));
        ImGui::PushID(p.Id);
        for (const ui::PartField& f : *p.Fields) DrawPartField(host, obj, ctx, p, f, data);
        ImGui::PopID();
    }

    ImGui::Separator();
    if (ImGui::Button(T("Remove UI Element"))) {
        host.PushUndoSnapshot();
        // Снимаем ВСЕ зарегистрированные части, а не список из этого файла:
        // часть, добавленная игрой, тоже должна уходить вместе с элементом.
        for (const ui::PartType& p : ui::Parts())
            if (p.Has(reg, e)) p.Remove(reg, e);
        reg.remove<ui::Transform>(e);
    }
}

} // namespace sage::editor
