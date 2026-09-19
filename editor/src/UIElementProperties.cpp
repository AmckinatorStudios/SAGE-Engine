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
#include "sage/ui/UIPart.h"
#include "sage/ui/UIIcons.h"
#include "sage/ui/UIPresets.h"
#include "sage/ui/UISerialize.h"

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
            ImGui::ColorEdit4(label, &ui::FieldAs<glm::vec4>(data, f).x);
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
                    // Кнопка живёт ВНУТРИ полей девятины, а те видны только в
                    // своём режиме, — значит режим уже нужный, и включать его
                    // здесь нечего.
                    // Путь берётся у самой части: девятина описывает ту
                    // картинку, рядом с которой лежит, и спрашивать его у
                    // человека второй раз незачем.
                    const ui::Image* img = reg.try_get<ui::Image>(e);
                    host.OpenNineSliceEditor(img ? img->Path : std::string());
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

// Поля ОДНОЙ части с учётом режима и набора. Вынесено, потому что разделов
// теперь четыре и цикл по полям повторялся бы в каждом.
void DrawPartFields(EditorHost& host, GameObject obj, const UIPropsContext& ctx,
                    const ui::PartType& p, void* data, bool bindings, int selected) {
    for (const ui::PartField& f : *p.Fields) {
        if ((f.Type == ui::PartField::Kind::Bindings) != bindings) continue;
        if (!ui::FieldVisible(*p.Fields, f, data)) continue;
        const bool before = ImGui::IsAnyItemActive();
        (void)before;
        DrawPartField(host, obj, ctx, p, f, data);
        // Правку разносим по набору ПОСЛЕ каждого поля: который именно виджет
        // её принял, знает только он сам, а «элемент только что отпустили»
        // ImGui умеет сказать про любой.
        if (selected > 1 && (ImGui::IsItemDeactivatedAfterEdit() || ImGui::IsItemEdited()))
            MirrorFieldToSelection(host, obj, p, f);
    }
}

// Разделы инспектора. Объявлены здесь, определены ниже: главная функция читается
// как оглавление — четыре раздела подряд, — и это ровно то, чем она и стала.
void DrawLayoutSection(EditorHost& host, entt::entity e, ui::Element* xf, int selected);
void DrawComponentsSection(EditorHost& host, GameObject obj, const UIPropsContext& ctx,
                           entt::entity e, int selected);

} // namespace

void DrawUIElementProperties(EditorHost& host, GameObject obj,
                             const UIPropsContext& ctx) {
    entt::registry& reg = host.CurrentScene().Registry();
    const entt::entity e = obj.Entity();
    namespace ui = sage::ui;

    ui::Element* xf = reg.try_get<ui::Element>(e);
    if (!xf) return;

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
    EditorIcons::Inline(type ? type->Icon : "ui-empty");
    ImGui::SameLine(0.0f, EditorIcons::TextGap());
    ImGui::SetNextItemWidth(-90.0f);
    // Имя типа ПЕРЕВОДИТСЯ: «Panel» и «Vertical List» — такие же строки
    // интерфейса, как всё остальное, и по-английски посреди русского редактора
    // они читаются как чужие. Само значение при этом не трогаем: тип хранится
    // и сравнивается английским ключом (см. ApplyPreset).
    if (ImGui::BeginCombo(T("Type"), xf->Type.empty() ? T("Custom") : T(xf->Type.c_str()))) {
        std::string category;
        for (const ui::Preset& preset : ui::Presets()) {
            if (preset.Category != category) {
                category = preset.Category;
                ImGui::SeparatorText(T(category.c_str()));
            }
            // СМЕНА ТИПА ПЕРЕСОБИРАЕТ ЭЛЕМЕНТ: тип — это и есть его устройство,
            // и «сделать из надписи кнопку» означает поставить те части, из
            // которых кнопка состоит. Своё положение и размер элемент при этом
            // сохраняет: человек просил сменить тип, а не переставить элемент.
            if (ImGui::Selectable(T(preset.Name.c_str()), preset.Name == xf->Type)) {
                host.PushUndoSnapshot();
                const ui::Element keep = *xf;
                ui::ApplyPreset(host.CurrentScene(), e, preset.Name);
                if (ui::Element* now = reg.try_get<ui::Element>(e)) {
                    now->Anchor = keep.Anchor;
                    now->Mode = keep.Mode;
                    now->Position = keep.Position;
                    now->Size = keep.Size;
                    now->Margin = keep.Margin;
                    now->Pivot = keep.Pivot;
                    now->Rotation = keep.Rotation;
                    now->Order = keep.Order;
                    now->Visible = keep.Visible;
                    now->Active = keep.Active;
                    now->Locked = keep.Locked;
                }
            }
            if (preset.Hint && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T(preset.Hint));
        }
        ImGui::EndCombo();
    }
    if (type && type->Hint) HintWrapped("%s", T(type->Hint));
    ImGui::Separator();

    // --- РАЗДЕЛЫ, А НЕ ОДНА ПРОСТЫНЯ ----------------------------------------
    //
    // Четыре раздела отвечают на четыре разных вопроса, и это ровно те
    // вопросы, с которыми к инспектору и приходят: КАК выглядит ЭТОТ ТИП, ГДЕ
    // элемент стоит, какие у него ВОЗМОЖНОСТИ и ЧТО он делает. Свёрнутый
    // раздел остаётся свёрнутым — правя цвета, незачем проматывать раскладку.
    //
    // ПОРЯДОК: СНАЧАЛА ТО, ЗАЧЕМ ЭЛЕМЕНТ ЗАВЕЛИ. Раскладка стояла первой, и
    // ради самого частого дела — сменить надпись у текста, картинку у
    // картинки — приходилось проматывать якорь, режим растяжения, размер,
    // отступы, точку вращения, угол и порядок. Человек, открывший редактор
    // впервые, ищет поле «Текст» и не находит его на экране вовсе.
    //
    // Теперь сверху свойства типа: у Text — текст, у Image — картинка, у
    // Slider — пределы. Раскладка сразу под ними: «где стоит» спрашивают
    // вторым вопросом, а не первым, и держат элемент чаще мышью на холсте.

    // СВОЙСТВА ТИПА. Здесь ровно то, из чего этот тип сделан: у надписи —
    // настройки текста, у кнопки — подложка и реакция, у полосы — шкала.
    // Включать и выключать тут нечего: это не набор галок, а устройство типа.
    //
    // Что относится к типу, а что к возможностям, решает не этот файл:
    // возможность объявляет себя сама (PartType::Extra).
    const std::string typeTitle =
        xf->Type.empty() ? std::string(T("Properties")) : std::string(T(xf->Type.c_str()));
    if (ImGui::CollapsingHeader(typeTitle.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        bool any = false;
        for (const ui::PartType& p : ui::Parts()) {
            if (!p.Fields || p.Extra || !p.Has(reg, e)) continue;
            void* data = p.GetMutable(reg, e);
            if (!data) continue;
            any = true;
            ImGui::SeparatorText(T(p.Title));
            ImGui::PushID(p.Id);
            DrawPartFields(host, obj, ctx, p, data, /*bindings=*/false, selected);
            ImGui::PopID();
        }
        if (!any) {
            HintWrapped("%s", T("This type draws nothing by itself. Build the interface from\n"
                                "child elements, or pick another type above."));
        }
    }

    if (ImGui::CollapsingHeader(T("Layout"), ImGuiTreeNodeFlags_DefaultOpen)) {
        DrawLayoutSection(host, e, xf, selected);
    }

    if (ImGui::CollapsingHeader(T("Capabilities"))) {
        DrawComponentsSection(host, obj, ctx, e, selected);
    }

    // --- События ------------------------------------------------------------
    //
    // Связи «когда здесь случилось X — сделать Y» собраны СО ВСЕХ частей в один
    // раздел. Лежа каждая внутри своей части, они терялись: у элемента с тремя
    // частями события искали в трёх местах, и найти их можно было, только
    // раскрыв все.
    //
    // Это и есть граница, о которой говорит архитектура: интерфейс не знает
    // механики игры, а связывают их события и скрипты — и раз это граница, она
    // обязана быть видна одним разделом, а не растворяться среди цветов.
    {
        bool anyEvents = false;
        for (const ui::PartType& p : ui::Parts()) {
            if (!p.Fields || !p.Has(reg, e)) continue;
            for (const ui::PartField& f : *p.Fields)
                if (f.Type == ui::PartField::Kind::Bindings) { anyEvents = true; break; }
            if (anyEvents) break;
        }
        if (ImGui::CollapsingHeader(T("Events"), anyEvents ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
            if (!anyEvents) {
                HintWrapped("%s", T("Events come with the Interaction capability. Turn it on above."));
            }
            for (const ui::PartType& p : ui::Parts()) {
                if (!p.Fields || !p.Has(reg, e)) continue;
                void* data = p.GetMutable(reg, e);
                if (!data) continue;
                ImGui::PushID(p.Id);
                DrawPartFields(host, obj, ctx, p, data, /*bindings=*/true, selected);
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

// --- Раздел «Из чего сделан» -------------------------------------------------
// --- Раздел «Возможности» ----------------------------------------------------
//
// ЗДЕСЬ БЫЛ МЕШОК ГАЛОК. Тринадцать переключателей — «Заливка», «Текст»,
// «Картинка», «Полоса», «Значок»… — и элемент был ровно их набором. Три беды
// сразу, и все три молчаливые:
//
//   • «Кнопка» существовала только как знание человека о том, какие четыре
//     галки надо поставить. Снятая по ошибке подложка превращала её в
//     прямоугольник, и ни одна строка об этом не говорила.
//   • Инспектор показывал объединение всех свойств всех включённых частей: у
//     надписи спрашивали толщину рамки, у полосы — подсказку поля ввода.
//   • На вопрос «что это за элемент» ответить было нечем — ни редактору, ни
//     скрипту, ни человеку.
//
// Теперь элемент ЗНАЕТ СВОЙ ТИП, а устройство типа (подложка у панели, надпись
// у текста) не переключается вовсе: оно и ЕСТЬ этот тип. Здесь остались только
// ВОЗМОЖНОСТИ — добавки, осмысленные почти к любому типу и не нужные по
// умолчанию ни одному: маска, реакция на мышь, раскладка детей, прокрутка.
//
// Что возможность, а что устройство типа, объявляет САМА ЧАСТЬ
// (PartType::Extra). Своего списка частей у редактора по-прежнему нет, и часть
// из игры встаёт в нужное место сама.
void DrawComponentsSection(EditorHost& host, GameObject obj, const UIPropsContext& ctx,
                           entt::entity e, int selected) {
    entt::registry& reg = host.CurrentScene().Registry();

    HintWrapped("%s", T("Extras that any element may have. What the type itself is made of\n"
                        "is not switched here — it is the type."));

    for (const ui::PartType& p : ui::Parts()) {
        if (!p.Fields || !p.Extra) continue;
        ImGui::PushID(p.Id);
        bool on = p.Has(reg, e);
        // Значок перед галкой: список из пяти одинаковых строк читают целиком,
        // а «маску» от «прокрутки» отличают по рисунку до чтения.
        if (ImGui::Checkbox("##on", &on)) {
            host.PushUndoSnapshot();
            if (on) p.Add(reg, e);
            else p.Remove(reg, e);
        }
        ImGui::SameLine();
        EditorIcons::Inline(p.Icon ? p.Icon : "cube");
        ImGui::SameLine(0.0f, EditorIcons::TextGap());
        ImGui::TextUnformatted(T(p.Title));
        if (p.Hint && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T(p.Hint));

        // Поля возможности — СРАЗУ ПОД НЕЙ, а не отдельным разделом ниже:
        // включил прокрутку — тут же и настроил, не разыскивая её по окну.
        if (on) {
            if (void* data = p.GetMutable(reg, e)) {
                ImGui::Indent();
                DrawPartFields(host, obj, ctx, p, data, /*bindings=*/false, selected);
                ImGui::Unindent();
            }
        }
        ImGui::PopID();
    }
}

// --- Раздел «Где стоит» ------------------------------------------------------
void DrawLayoutSection(EditorHost& host, entt::entity e, ui::Element* xf, int selected) {
    entt::registry& reg = host.CurrentScene().Registry();
    (void)selected;
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
    ImGui::DragFloat2(T("Pivot"), &xf->Pivot.x, 0.01f, 0.0f, 1.0f); host.TrackLastImGuiItem();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", T("Which point of the element lands on the anchor"));
    }
    // УГОЛ — ЧИСЛОМ, а не только мышью. Ручка на холсте ставит его на глаз, а
    // «ровно 90» набирают здесь; обратное тоже верно — поэтому есть и то, и
    // другое.
    ImGui::DragFloat(T("Rotation"), &xf->Rotation, 1.0f, -180.0f, 180.0f, "%.1f\xc2\xb0");
    host.TrackLastImGuiItem();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("Around the element centre. The handle above the top edge\n"
                                  "does the same with the mouse."));
    ImGui::DragInt(T("Order"), &xf->Order, 1); host.TrackLastImGuiItem();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Higher draws on top of its siblings"));
    ImGui::Checkbox(T("Visible"), &xf->Visible);
}

} // namespace

} // namespace sage::editor
