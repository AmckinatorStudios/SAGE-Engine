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
    // Порядок — тот же, что у UIAnchor (строка * 3 + столбец), и это не
    // случайность: сетка кнопок и есть перечисление, разложенное по экрану.
    static const char* const kAnchorIcons[9] = {
        "anchor-tl", "anchor-tc", "anchor-tr",
        "anchor-cl", "anchor-cc", "anchor-cr",
        "anchor-bl", "anchor-bc", "anchor-br",
    };
    bool changed = false;
    const float cell = ImGui::GetFrameHeight();
    ImGui::BeginGroup();
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            const int index = row * 3 + col;
            if (col) ImGui::SameLine(0.0f, 2.0f);
            ImGui::PushID(index);
            const bool active = (int)anchor == index;
            if (active) ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::Color(EditorTheme::Role::Accent));
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            if (ImGui::Button("##anchor", ImVec2(cell, cell))) {
                anchor = (UIAnchor)index;
                changed = true;
            }
            if (active) ImGui::PopStyleColor();
            // Рисунок внутри кнопки — ИКОНКА НАБОРА: прямоугольник и плашка,
            // прижатая к тому краю, который эта кнопка означает. Точка в углу,
            // стоявшая здесь раньше, называла МЕСТО, но не говорила главного —
            // что элемент к этому месту прижмётся, а не просто окажется рядом.
            EditorIcons::Overlay(p0.x, p0.y, cell, kAnchorIcons[index],
                                 EditorIcons::kThemeColor);
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
    (void)changed;
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
                    // Диалог заперт в проекте: ассет выбирается ИЗНУТРИ (см. assetslot::ProjectRoot).
                    c.StartDir = c.Root = assetslot::ProjectRoot(host);
                    ctx.Browser->Open(c);
                    if (ctx.BrowseTarget) *ctx.BrowseTarget = &v;
                }
            } else if (f.Editor == W::Font) {
                // Шрифт выбирается ТАКИМ ЖЕ слотом, как картинка: набирать путь
                // руками к файлу, который у набора лежит в подпапке рядом с
                // лицензией, — ровно то, ради чего слоты и заведены.
                const assetslot::Result r =
                    assetslot::Draw(host, f.Key, assetslot::Kind::Font, v, ctx.Preview,
                                    f.Tooltip ? T(f.Tooltip) : label);
                if (r.Changed) {
                    host.PushUndoSnapshot();
                    v = r.Path;
                    changed = true;
                }
                if (r.BrowseRequested && ctx.Browser) {
                    FileBrowser::Config c;
                    c.Title = T("Choose a font");
                    c.Filters = assetslot::Extensions(assetslot::Kind::Font);
                    c.FilterLabel = T("Fonts");
                    c.StartDir = c.Root = assetslot::ProjectRoot(host);
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
                        ImGui::SameLine(0.0f, 0.0f);
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
            Sage::UI::ColorField4(label, &ui::FieldAs<glm::vec4>(data, f).x);
            host.TrackLastImGuiItem();
            break;

        case K::Vec2:
            ImGui::DragFloat2(label, &ui::FieldAs<glm::vec2>(data, f).x, 1.0f, f.Min, f.Max);
            host.TrackLastImGuiItem();
            break;

        case K::Vec4:
            if (f.Editor == W::NineSliceBorder) {
                // Числа здесь остаются — иногда нарезка известна точно и её
                // просто вписывают. Но подбирают её ГЛАЗАМИ, по картинке, и
                // кнопка рядом ведёт туда, где это видно.
                ImGui::DragFloat4(label, &ui::FieldAs<glm::vec4>(data, f).x, 1.0f, f.Min, f.Max);
                host.TrackLastImGuiItem();
                if (ImGui::SmallButton(T("Edit on the picture…"))) {
                    // Окно правит ЭТОТ вид этого элемента вживую: какая часть и
                    // какое поле — знает само поле, картинку окно найдёт рядом.
                    EditorHost::NineSliceTarget target;
                    target.ElementId = obj.Id();
                    target.PartId = part.Id ? part.Id : "";
                    target.BorderKey = f.Key;
                    host.OpenNineSliceEditor(target);
                }
                break;
            }
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

        case K::Bindings: {
            // Связи «когда здесь случилось X — сделать Y». Тот же вид поля, что
            // и остальные, и это главное: они попадают в сцену и в инспектор по
            // ОБЩЕЙ таблице, а не отдельной веткой в сериализаторе и второй —
            // здесь. Своя часть игры получает собственные события одной
            // строкой в своей таблице полей.
            sage::events::Bindings& v = ui::FieldAs<sage::events::Bindings>(data, f);
            if (ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_DefaultOpen)) {
                if (f.Tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T(f.Tooltip));
                varsui::DrawBindings(host, f.Key, v, sage::events::UITriggers(), ctx.Preview);
                ImGui::TreePop();
            }
            // Подсказка уже показана внутри — общий хвост её бы задублировал.
            ImGui::PopID();
            return;
        }

        // Вид — заголовок группы; его поля рисует DrawPartFields.
        case K::Look:
            break;
    }
    if (f.Tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T(f.Tooltip));
    ImGui::PopID();

    // Картинке после смены пути нужен новый рантайм-указатель: иначе слот
    // показывает новый файл, а элемент рисует старый.
    if (changed && part.Id && std::string(part.Id) == "image") {
        if (ui::Image* im = reg.try_get<ui::Image>(e)) {
            im->Tex = im->Path.empty()
                          ? nullptr
                          : (im->Sharp() ? ResourceManager::Instance().GetTexture(
                                               im->Path, TextureFilter::Nearest, false)
                                         : ResourceManager::Instance().GetTexture(im->Path));
        }
    }
}

namespace {

// --- ПРАВКА НЕСКОЛЬКИХ ЭЛЕМЕНТОВ СРАЗУ ---------------------------------------
//
// Человек выделяет набор кнопок и красит их одним движением — это и есть
// обычная работа с интерфейсом. Пока инспектор правил только «первый
// выбранный», набор из двадцати кнопок красили по одной, и одна обязательно
// оставалась другого цвета: заметить это можно было, только пересчитав их
// глазами.
//
// РАБОТАЕТ ЭТО ТАК: поле крутят у первичного элемента (он и показан), а после
// правки то же самое значение ложится всем остальным выбранным, у кого есть
// ТА ЖЕ часть. Копируется ровно одно поле, а не компонент целиком: у кнопок
// набора разные подписи, и «покрасить все» не имеет права снести их тексты.
//
// Части, которой у соседа нет, НЕ ДОБАВЛЯЕМ: правка цвета не должна заводить
// подложку там, где её не было, — это уже не правка, а сборка чужого элемента.
void MirrorFieldToSelection(EditorHost& host, GameObject primary, const ui::PartType& part,
                            const ui::PartField& f) {
    Scene& scene = host.CurrentScene();
    entt::registry& reg = scene.Registry();
    const void* src = part.Get(reg, primary.Entity());
    if (!src) return;
    for (int id : host.Selection().All()) {
        GameObject other = scene.Get(id);
        if (!other.Valid() || other.Entity() == primary.Entity()) continue;
        if (!part.Has(reg, other.Entity())) continue;
        void* dst = part.GetMutable(reg, other.Entity());
        if (!dst) continue;
        ui::CopyField(f, src, dst);
        // Картинке после смены пути нужен свой рантайм-указатель: иначе сосед
        // получит путь нового файла и указатель на старую текстуру.
        if (part.Id && std::string(part.Id) == "image") {
            if (ui::Image* im = reg.try_get<ui::Image>(other.Entity())) ui::ResolveImageTexture(*im);
        }
    }
}

// Сколько элементов интерфейса выделено. По нему инспектор решает, говорить ли
// про набор вообще: у одного элемента слова «правится у всех выбранных» — шум.
int SelectedElements(EditorHost& host) {
    Scene& scene = host.CurrentScene();
    entt::registry& reg = scene.Registry();
    int n = 0;
    for (int id : host.Selection().All()) {
        GameObject o = scene.Get(id);
        if (o.Valid() && reg.all_of<ui::Element>(o.Entity())) ++n;
    }
    return n;
}

// Какие поля части рисовать: у реакции на мышь поля ВИДА (виды состояний)
// стоят среди оформления, а поля ПОВЕДЕНИЯ (действие, курсор) — своим
// разделом; связи событий — разделом «События».
enum class FieldFilter { Look, Behaviour, Bindings };

bool Passes(const ui::PartField& f, FieldFilter filter, bool splitContent) {
    const bool bindings = f.Type == ui::PartField::Kind::Bindings;
    if (filter == FieldFilter::Bindings) return bindings;
    if (bindings) return false;
    if (!splitContent) return filter == FieldFilter::Look;
    return (filter == FieldFilter::Behaviour) == f.Content;
}

// КАКИЕ ПОЛЯ ПОКАЗАТЬ ЗА ОДИН ПРОХОД. Инспектор делит поля части на
// основные (видны сразу) и редкие («Ещё настройки»), а поля состояний кнопки
// — по вкладкам. Одним описанием, чтобы «есть ли вообще что показать под
// “Ещё”» считалось тем же правилом, что и сам показ.
struct FieldView {
    FieldFilter Filter = FieldFilter::Look;
    bool SplitContent = false;   // у реакции на мышь поведение — отдельным разделом
    bool Advanced = false;       // false — основные поля, true — «Ещё настройки»
    const char* Tab = nullptr;   // nullptr — поля без вкладки; иначе только эта вкладка
};

// Есть ли у элемента часть, без которой поле не имеет смысла.
bool RequirementMet(const entt::registry& reg, entt::entity e, const ui::PartField& f) {
    if (!f.Requires || !*f.Requires) return true;
    const ui::PartType* need = ui::FindPart(f.Requires);
    return need && need->Has && need->Has(reg, e);
}

bool SameTab(const char* a, const char* b) {
    if (!a || !b) return a == b;
    return std::strcmp(a, b) == 0;
}

// Проходит ли поле (не вид) под нынешний показ.
bool Shows(const std::vector<ui::PartField>& fields, const ui::PartField& f, const void* data,
           const entt::registry& reg, entt::entity e, const FieldView& view) {
    if (f.Hidden || f.Type == ui::PartField::Kind::Look) return false;
    if (!Passes(f, view.Filter, view.SplitContent)) return false;
    if (!SameTab(f.Tab, view.Tab)) return false;
    if (f.Advanced != view.Advanced) return false;
    if (!RequirementMet(reg, e, f)) return false;
    return ui::FieldVisible(fields, f, data);
}

void DrawOneField(EditorHost& host, GameObject obj, const UIPropsContext& ctx, const ui::PartType& p,
                  const ui::PartField& f, void* data, int selected) {
    DrawPartField(host, obj, ctx, p, f, data);
    // Правку разносим по набору ПОСЛЕ каждого поля: который именно виджет её
    // принял, знает только он сам.
    if (selected > 1 && (ImGui::IsItemDeactivatedAfterEdit() || ImGui::IsItemEdited()))
        MirrorFieldToSelection(host, obj, p, f);
}

// Поля одного ВИДА (Kind::Look): основные сразу, редкие — своим «Ещё».
void DrawLookGroup(EditorHost& host, GameObject obj, const UIPropsContext& ctx, const ui::PartType& p,
                   const std::vector<ui::PartField>& fields, size_t first, size_t end, void* data,
                   const entt::registry& reg, entt::entity e, int selected) {
    bool anyAdvanced = false;
    for (size_t k = first; k < end; ++k) {
        const ui::PartField& lf = fields[k];
        if (lf.Hidden || !ui::FieldVisible(fields, lf, data) || !RequirementMet(reg, e, lf)) continue;
        if (lf.Advanced) { anyAdvanced = true; continue; }
        DrawOneField(host, obj, ctx, p, lf, data, selected);
    }
    if (anyAdvanced && ImGui::TreeNodeEx(T("More settings"), ImGuiTreeNodeFlags_SpanAvailWidth)) {
        for (size_t k = first; k < end; ++k) {
            const ui::PartField& lf = fields[k];
            if (lf.Hidden || !lf.Advanced || !ui::FieldVisible(fields, lf, data) || !RequirementMet(reg, e, lf))
                continue;
            DrawOneField(host, obj, ctx, p, lf, data, selected);
        }
        ImGui::TreePop();
    }
}

// Поля ОДНОЙ части под нынешний показ. Виды (Kind::Look) — свёрнутыми
// группами: у ползунка их три, и раскрытые разом они были бы простынёй.
// Возвращает, сколько нарисовано (заголовки видов тоже считаются).
int DrawFieldsOf(EditorHost& host, GameObject obj, const UIPropsContext& ctx, const ui::PartType& p,
                 void* data, const FieldView& view, int selected, bool dryRun = false) {
    const entt::registry& reg = host.CurrentScene().Registry();
    const entt::entity e = obj.Entity();
    const std::vector<ui::PartField> fields = ui::EditableFields(*p.Fields);
    const size_t lookSize = ui::LookFields().size();
    int drawn = 0;
    for (size_t i = 0; i < fields.size(); ++i) {
        const ui::PartField& f = fields[i];
        if (f.Type == ui::PartField::Kind::Look) {
            const size_t end = std::min(fields.size(), i + 1 + lookSize);
            const bool show = !f.Hidden && view.Filter == FieldFilter::Look && SameTab(f.Tab, view.Tab) &&
                              f.Advanced == view.Advanced && RequirementMet(reg, e, f) &&
                              ui::FieldVisible(fields, f, data);
            if (show) {
                ++drawn;
                if (!dryRun) {
                    ImGui::PushID(f.Key);
                    // Вид состояния на своей вкладке раскрыт сразу: вкладку и
                    // открывают ради него.
                    const ImGuiTreeNodeFlags flags =
                        ImGuiTreeNodeFlags_SpanAvailWidth | (view.Tab ? ImGuiTreeNodeFlags_DefaultOpen : 0);
                    const bool open = ImGui::TreeNodeEx(T(f.Label), flags);
                    if (f.Tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T(f.Tooltip));
                    if (open) {
                        DrawLookGroup(host, obj, ctx, p, fields, i + 1, end, data, reg, e, selected);
                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                }
            }
            i = end - 1;
            continue;
        }
        if (!Shows(fields, f, data, reg, e, view)) continue;
        ++drawn;
        if (!dryRun) DrawOneField(host, obj, ctx, p, f, data, selected);
    }
    return drawn;
}

// Раздел части: основные поля, а редкие — под «Ещё настройки» (свёрнуто).
void DrawPartSection(EditorHost& host, GameObject obj, const UIPropsContext& ctx, const ui::PartType& p,
                     void* data, FieldView view, int selected) {
    // Своя область имён: «Ещё настройки» бывает у каждой части и вкладки.
    ImGui::PushID(p.Id ? p.Id : "part");
    ImGui::PushID(view.Tab ? view.Tab : "");
    view.Advanced = false;
    DrawFieldsOf(host, obj, ctx, p, data, view, selected);
    view.Advanced = true;
    if (DrawFieldsOf(host, obj, ctx, p, data, view, selected, /*dryRun=*/true) > 0 &&
        ImGui::TreeNodeEx(T("More settings"), ImGuiTreeNodeFlags_SpanAvailWidth)) {
        DrawFieldsOf(host, obj, ctx, p, data, view, selected);
        ImGui::TreePop();
    }
    ImGui::PopID();
    ImGui::PopID();
}

// Совместимость: все поля части под фильтр (события, части сверх типа).
void DrawPartFields(EditorHost& host, GameObject obj, const UIPropsContext& ctx,
                    const ui::PartType& p, void* data, FieldFilter filter, bool splitContent,
                    int selected) {
    FieldView view;
    view.Filter = filter;
    view.SplitContent = splitContent;
    DrawPartSection(host, obj, ctx, p, data, view, selected);
}

// Вкладки состояний, которые у поля части вообще встречаются (по порядку
// таблицы) и к которым у элемента есть что показать.
std::vector<const char*> StateTabs(EditorHost& host, GameObject obj, const UIPropsContext& ctx,
                                   const ui::PartType& p, void* data) {
    std::vector<const char*> tabs;
    for (const ui::PartField& f : *p.Fields) {
        if (!f.Tab) continue;
        bool known = false;
        for (const char* t : tabs) known |= SameTab(t, f.Tab);
        if (known) continue;
        FieldView v;
        v.SplitContent = true;
        v.Tab = f.Tab;
        v.Advanced = false;
        int n = DrawFieldsOf(host, obj, ctx, p, data, v, 1, true);
        v.Advanced = true;
        n += DrawFieldsOf(host, obj, ctx, p, data, v, 1, true);
        if (n > 0) tabs.push_back(f.Tab);
    }
    return tabs;
}

// Входит ли часть в устройство типа. По флагам заготовки — тем же, по которым
// тип и собирается (ApplyPreset), а не своим списком.
bool TypeHasPart(const ui::Preset& t, const char* id) {
    const std::string s = id ? id : "";
    return (s == "fill" && t.HasFill) || (s == "label" && t.HasLabel) ||
           (s == "image" && t.HasImage) || (s == "bar" && t.HasBar) ||
           (s == "interactable" && t.HasInteractable) || (s == "textInput" && t.HasInput) ||
           (s == "range" && t.HasRange) || (s == "layout" && t.HasStack) ||
           (s == "mask" && t.HasMask) || (s == "scroll" && t.HasScroll) ||
           (s == "icon" && t.HasIcon);
}

// Разделы инспектора. Объявлены здесь, определены ниже: главная функция читается
// как оглавление.
void DrawLayoutSection(EditorHost& host, entt::entity e, ui::Element* xf, int selected);
void DrawStyleRow(EditorHost& host, GameObject obj, const UIPropsContext& ctx, ui::Element& xf);
void DrawGroupSettings(EditorHost& host, entt::entity e);

} // namespace

void OpenInInterfaceMode(EditorHost& host, GameObject obj) {
    Scene& scene = host.CurrentScene();
    const entt::entity owner = sage::ui::InterfaceOf(scene, obj.Entity());
    // Интерфейса над элементом нет — это «элементы без интерфейса» (их
    // собирает код или скрипт), и у них в списке своя строка с номером 0.
    int id = 0;
    if (owner != entt::null)
        if (const IdComponent* idc = scene.Registry().try_get<IdComponent>(owner)) id = idc->Id;

    host.SetWorkspace(EditorWorkspace::Interface);
    host.SetCurrentInterface(id);
    host.Selection().SetPrimary(obj.Id());
}

// Кнопка «открыть в режиме интерфейса» — одна на оба компонента.
//
// ЗАЧЕМ ОНА. Объект интерфейса и его элементы видны и в сцене, и найдя их
// там, человек идёт верстать: до сих пор это значило переключить пространство
// вручную, а потом ещё выбрать в списке нужный интерфейс из десятка похожих
// имён — при том, что объект уже был под рукой.
static void DrawOpenInInterfaceButton(EditorHost& host, GameObject obj) {
    if (host.Workspace() == EditorWorkspace::Interface) return;   // уже там
    if (EditorIcons::Button("ui-panel", T("Open in interface mode"),
                            T("Switch to layout and open this interface")))
        OpenInInterfaceMode(host, obj);
    ImGui::Separator();
}

void DrawInterfaceProperties(EditorHost& host, GameObject obj) {
    namespace ui = sage::ui;
    entt::registry& reg = host.CurrentScene().Registry();
    ui::InterfaceComponent* info = reg.try_get<ui::InterfaceComponent>(obj.Entity());
    if (!info) return;

    DrawOpenInInterfaceButton(host, obj);

    if (ImGui::Checkbox(T("Visible"), &info->Visible)) host.PushUndoSnapshot();
    EditorTheme::Hint(T("off — the whole interface is hidden, with all its elements"));

    if (ImGui::Checkbox(T("Receives input"), &info->ReceivesInput)) host.PushUndoSnapshot();
    EditorTheme::Hint(T("off — shown but not clickable: a splash, credits, a hint over the game"));

    ImGui::DragInt(T("Sort order"), &info->SortOrder, 0.1f, -100, 100);
    host.TrackLastImGuiItem();
    EditorTheme::Hint(T("order BETWEEN interfaces: the HUD below the pause menu"));

    // --- ХОЛСТ: ПОД КАКОЙ ЭКРАН ВЕРСТАЮТ -----------------------------------
    //
    // Свойство ВСЕГО интерфейса, а не его первого элемента: верстают под один
    // размер экрана весь экран. Пока это поле жило на корневом элементе, два
    // корня одного интерфейса могли спорить о масштабе.
    ImGui::SeparatorText(T("Canvas"));
    const char* kModes[] = {T("Pixels"), T("Scale to reference"), T("Whole-number scale (pixel art)")};
    int mode = (int)info->Canvas.Mode;
    if (ImGui::Combo(T("Scale mode"), &mode, kModes, 3)) {
        host.PushUndoSnapshot();
        info->Canvas.Mode = (ui::Canvas::Scale)mode;
    }
    if (info->Canvas.Mode == ui::Canvas::Scale::ScaleWithSize) {
        float ref[2] = {info->Canvas.Reference.x, info->Canvas.Reference.y};
        if (ImGui::DragFloat2(T("Reference resolution"), ref, 1.0f, 16.0f, 8192.0f, "%.0f")) {
            info->Canvas.Reference = {ref[0], ref[1]};
        }
        host.TrackLastImGuiItem();
        ImGui::SliderFloat(T("Match width/height"), &info->Canvas.MatchWidthOrHeight, 0.0f, 1.0f);
        host.TrackLastImGuiItem();
        EditorTheme::Hint(T("0 — follow width, 1 — follow height"));
    }
    if (info->Canvas.Mode == ui::Canvas::Scale::IntegerFit) {
        float ref[2] = {info->Canvas.Reference.x, info->Canvas.Reference.y};
        if (ImGui::DragFloat2(T("Reference resolution"), ref, 1.0f, 16.0f, 8192.0f, "%.0f")) {
            info->Canvas.Reference = {ref[0], ref[1]};
        }
        host.TrackLastImGuiItem();
        ImGui::DragInt(T("Largest scale"), &info->Canvas.MaxScale, 0.05f, 0, 16);
        host.TrackLastImGuiItem();
        EditorTheme::Hint(T("The largest whole number that fits the reference screen: pixel art stays crisp. 0 — no limit"));
    }

    // Элементы правятся у САМИХ ЭЛЕМЕНТОВ. Сказано прямо, потому что именно
    // здесь их и искали: объект с интерфейсом выглядит как «тот самый экран».
    ImGui::Separator();
    const std::vector<entt::entity> roots = ui::InterfaceRoots(host.CurrentScene(), obj.Entity());
    ImGui::Text(T("Root elements: %d"), (int)roots.size());
    EditorTheme::Hint(T("Element properties — colour, text, layout — live on the elements "
                        "themselves, inside this interface."));
}

void DrawUIElementProperties(EditorHost& host, GameObject obj,
                             const UIPropsContext& ctx) {
    entt::registry& reg = host.CurrentScene().Registry();
    const entt::entity e = obj.Entity();
    namespace ui = sage::ui;

    ui::Element* xf = reg.try_get<ui::Element>(e);
    if (!xf) return;

    DrawOpenInInterfaceButton(host, obj);

    // СКОЛЬКО ВЫБРАНО — сказано сразу, а не выясняется опытом. Инспектор
    // показывает поля одного элемента, а правит их у всех, и промолчать об
    // этом значит превратить обычную правку в неожиданность: покрасил одну
    // кнопку, покрасились двадцать.
    const int selected = SelectedElements(host);
    if (selected > 1) {
        ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(EditorTheme::Role::Accent));
        ImGui::Text(T("Selected: %d — an edit goes to all of them"), selected);
        ImGui::PopStyleColor();
        ImGui::Separator();
    }

    // --- ЧТО ЭТО ЗА ЭЛЕМЕНТ ------------------------------------------------
    //
    // ТИП — ПЕРВОЕ, ЧТО ВИДНО, и это не украшение шапки. Раньше на его месте
    // стоял мешок галок, и ответить на вопрос «что это» можно было, только
    // прочитав, какие из тринадцати включены. Теперь тип написан прямо, и от
    // него зависит всё остальное окно.
    // Элемент, собранный в обход типов (скриптом, демо-сценой, игрой),
    // приходит без типа. Опознаём его по набору частей — тем же правилом, что
    // и миграция сцен, — и ЗАПИСЫВАЕМ: иначе тип пришлось бы выводить заново
    // при каждом взгляде, и он не попал бы в сохранённую сцену.
    if (xf->Type.empty()) {
        const std::string guessed = ui::InferType(reg, e);
        if (!guessed.empty()) xf->Type = guessed;
    }
    const ui::Preset* type = ui::FindPreset(xf->Type);
    const bool isContainer = type && type->Container;
    EditorIcons::Inline(type ? type->Icon : "ui-empty");
    ImGui::SameLine(0.0f, EditorIcons::TextGap());
    ImGui::SetNextItemWidth(-90.0f);
    // Имя типа ПЕРЕВОДИТСЯ; само значение хранится английским ключом.
    //
    // В СПИСКЕ — ТОЛЬКО СВОЙ РОД. Элемент меняется на элемент, контейнер — на
    // контейнер: «сделать из кнопки столбец» значило бы снять с неё всё, что
    // делает её видимой, и это уже не смена типа, а другой объект.
    if (ImGui::BeginCombo(T("Type"), xf->Type.empty() ? T("Custom") : T(xf->Type.c_str()))) {
        std::string category;
        for (const ui::Preset& preset : ui::Presets()) {
            if (type && preset.Container != type->Container) continue;
            if (preset.Category != category) {
                category = preset.Category;
                ImGui::SeparatorText(T(category.c_str()));
            }
            // СМЕНА ТИПА ПЕРЕСОБИРАЕТ ЧАСТИ, но не трогает детей и место:
            // человек просил сменить тип, а не переставить элемент и не
            // потерять то, что внутри.
            if (ImGui::Selectable(T(preset.Name.c_str()), preset.Name == xf->Type)) {
                host.PushUndoSnapshot();
                const ui::Element keep = *xf;
                ui::ApplyPreset(host.CurrentScene(), e, preset.Name, /*replaceChildren=*/false);
                if (ui::Element* now = reg.try_get<ui::Element>(e)) {
                    const std::string newType = now->Type;
                    const glm::vec2 presetSize = now->Size;
                    const float presetGrow = now->Grow;
                    *now = keep;
                    now->Type = newType;
                    // Распорка без роста — не распорка: у неё рост и есть смысл.
                    if (presetGrow > 0.0f && now->Grow <= 0.0f) now->Grow = presetGrow;
                    if (now->Size.x <= 0.0f || now->Size.y <= 0.0f) now->Size = presetSize;
                    now->StyleVersion = -1;
                }
                xf = reg.try_get<ui::Element>(e);
            }
            if (preset.Hint && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T(preset.Hint));
        }
        ImGui::EndCombo();
    }
    if (!xf) return;
    type = ui::FindPreset(xf->Type);
    if (type && type->Hint) HintWrapped("%s", T(type->Hint));
    if (!type)
        HintWrapped("%s", T("An element from an older version or built by code: its parts are shown "
                            "as they are. Pick a type above to rebuild it."));

    // Стиль из файла — у видимых элементов (у контейнера виду взяться неоткуда).
    if (!isContainer) DrawStyleRow(host, obj, ctx, *xf);
    ImGui::Separator();

    // --- ВИД: то, из чего сделан тип ----------------------------------------
    //
    // Сверху — то, ради чего элемент заводили: у текста — текст, у картинки —
    // картинка, у кнопки — её вид в каждом состоянии. Включать и выключать тут
    // нечего: это устройство типа, а не набор галок.
    auto belongs = [&](const ui::PartType& p) { return !type || TypeHasPart(*type, p.Id); };
    if (!isContainer) {
        const std::string typeTitle =
            xf->Type.empty() ? std::string(T("Look")) : std::string(T(xf->Type.c_str()));
        if (ImGui::CollapsingHeader(typeTitle.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            // СОСТОЯНИЯ КНОПКИ — ВКЛАДКАМИ: «обычная», «при наведении»,
            // «при нажатии»... Каждая вкладка — вид в одном состоянии, а не
            // двадцать полей всех состояний подряд.
            const ui::PartType* act = ui::FindPart("interactable");
            void* actData = (act && act->Has(reg, e) && belongs(*act)) ? act->GetMutable(reg, e) : nullptr;
            const std::vector<const char*> tabs =
                actData ? StateTabs(host, obj, ctx, *act, actData) : std::vector<const char*>{};
            const ui::PartType* fillPart = ui::FindPart("fill");
            const bool fillInTabs = !tabs.empty() && fillPart && fillPart->Has(reg, e) && belongs(*fillPart);

            auto stateTabs = [&](bool withNormal) {
                if (!ImGui::BeginTabBar("##ui_states", ImGuiTabBarFlags_FittingPolicyScroll)) return;
                if (withNormal && ImGui::BeginTabItem(T("Normal"))) {
                    if (void* fd = fillPart->GetMutable(reg, e))
                        DrawPartSection(host, obj, ctx, *fillPart, fd, FieldView{}, selected);
                    ImGui::EndTabItem();
                }
                for (const char* tab : tabs) {
                    if (!ImGui::BeginTabItem(T(tab))) continue;
                    FieldView v;
                    v.SplitContent = true;
                    v.Tab = tab;
                    DrawPartSection(host, obj, ctx, *act, actData, v, selected);
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            };

            bool statesDrawn = false;
            for (const ui::PartType& p : ui::Parts()) {
                if (!p.Fields || p.Container || p.Hidden || !p.Has(reg, e) || !belongs(p)) continue;
                if (act && &p == act) continue;   // реакция — вкладками и «Поведением»
                void* data = p.GetMutable(reg, e);
                if (!data) continue;
                if (fillInTabs && &p == fillPart) {
                    stateTabs(true);
                    statesDrawn = true;
                    continue;
                }
                ImGui::SeparatorText(T(p.Title));
                ImGui::PushID(p.Id);
                DrawPartSection(host, obj, ctx, p, data, FieldView{}, selected);
                ImGui::PopID();
            }
            // Без подложки (ползунок, галка) у состояний только подкраска —
            // она и показывается своими вкладками после вида.
            if (!statesDrawn && !tabs.empty()) {
                ImGui::SeparatorText(T("States"));
                stateTabs(false);
            }
        }
    }

    // --- КОНТЕЙНЕР: как расставлены дети -------------------------------------
    if (isContainer || !type) {
        bool any = false;
        for (const ui::PartType& p : ui::Parts())
            if (p.Fields && p.Container && !p.Hidden && p.Has(reg, e)) any = true;
        if (any && ImGui::CollapsingHeader(T("Container"), ImGuiTreeNodeFlags_DefaultOpen)) {
            for (const ui::PartType& p : ui::Parts()) {
                if (!p.Fields || !p.Container || p.Hidden || !p.Has(reg, e)) continue;
                void* data = p.GetMutable(reg, e);
                if (!data) continue;
                ImGui::SeparatorText(T(p.Title));
                ImGui::PushID(p.Id);
                DrawPartSection(host, obj, ctx, p, data, FieldView{}, selected);
                ImGui::PopID();
            }
        }
        if (isContainer && !any)
            HintWrapped("%s", T("Children keep their own anchors inside this container."));
    }

    // --- ПОВЕДЕНИЕ: что элемент делает ----------------------------------------
    const ui::PartType* interaction = ui::FindPart("interactable");
    if (interaction && interaction->Has(reg, e) &&
        ImGui::CollapsingHeader(T("Behaviour"), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (void* data = interaction->GetMutable(reg, e)) {
            ImGui::PushID("behaviour");
            DrawPartFields(host, obj, ctx, *interaction, data, FieldFilter::Behaviour, true, selected);
            ImGui::PopID();
        }
    }

    if (ImGui::CollapsingHeader(T("Layout"), ImGuiTreeNodeFlags_DefaultOpen)) {
        DrawLayoutSection(host, e, xf, selected);
    }

    // --- События — только у тех, у кого они бывают ---------------------------
    //
    // Связи «когда здесь случилось X — сделать Y» — граница между интерфейсом
    // и игрой, поэтому одним разделом. У надписи и картинки событий нет, и
    // пустой раздел с советом «включите что-то» был бы шумом.
    {
        bool anyEvents = false;
        for (const ui::PartType& p : ui::Parts()) {
            if (!p.Fields || !p.Has(reg, e)) continue;
            for (const ui::PartField& f : *p.Fields)
                if (f.Type == ui::PartField::Kind::Bindings) anyEvents = true;
        }
        if (anyEvents && ImGui::CollapsingHeader(T("Events"), ImGuiTreeNodeFlags_DefaultOpen)) {
            for (const ui::PartType& p : ui::Parts()) {
                if (!p.Fields || !p.Has(reg, e)) continue;
                void* data = p.GetMutable(reg, e);
                if (!data) continue;
                ImGui::PushID(p.Id);
                DrawPartFields(host, obj, ctx, p, data, FieldFilter::Bindings, false, selected);
                ImGui::PopID();
            }
        }
    }

    // --- ЧАСТИ СВЕРХ ТИПА ------------------------------------------------------
    //
    // У элемента из старой сцены (или собранного кодом) бывают части, которых в
    // его типе нет: картинка на кнопке, раскладка на панели. Они работают и
    // показаны честно — здесь, отдельно от вида типа, с кнопкой «убрать».
    if (type) {
        std::vector<const ui::PartType*> extra;
        for (const ui::PartType& p : ui::Parts())
            if (p.Fields && !p.Hidden && p.Has(reg, e) && !TypeHasPart(*type, p.Id)) extra.push_back(&p);
        if (!extra.empty() && ImGui::CollapsingHeader(T("Parts beyond the type"))) {
            HintWrapped("%s", T("These parts are not part of this type. They still work; remove them "
                                "to keep the element simple."));
            for (const ui::PartType* p : extra) {
                ImGui::PushID(p->Id);
                ImGui::SeparatorText(T(p->Title));
                if (ImGui::SmallButton(T("Remove"))) {
                    host.PushUndoSnapshot();
                    p->Remove(reg, e);
                    ImGui::PopID();
                    break;
                }
                if (void* data = p->GetMutable(reg, e))
                    DrawPartFields(host, obj, ctx, *p, data, FieldFilter::Look, false, selected);
                ImGui::PopID();
            }
        }
    }

    ImGui::Separator();
    if (ImGui::Button(T("Remove UI Element"))) {
        host.PushUndoSnapshot();
        // Снимаем ВСЕ зарегистрированные части, а не список из этого файла:
        // часть, добавленная игрой, тоже должна уходить вместе с элементом.
        for (const ui::PartType& p : ui::Parts())
            if (p.Has(reg, e)) p.Remove(reg, e);
        reg.remove<ui::Element>(e);
    }
}

namespace {

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
        if (ImGui::SmallButton(T("Save this look as a style"))) {
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
    if (ImGui::SmallButton(T("Save changes to the style"))) {
        if (save(xf.Style)) xf.StyleVersion = -1;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("Every element with this style takes the new look."));
    ImGui::SameLine();
    if (ImGui::SmallButton(T("Detach"))) {
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

} // namespace

} // namespace sage::editor
