// ---------------------------------------------------------------------------
// Свойства элемента интерфейса — см. UIElementProperties.h.
//
// Раньше это была часть инспектора (InspectorPanel_Ui.cpp). Переехало сюда,
// когда у полей появился второй читатель — редактор интерфейса: два окна
// правят один и тот же набор компонентов, и держать для этого два списка
// полей значило бы разойтись на первой же новой галке.
// ---------------------------------------------------------------------------
#include "UIElementProperties.h"
#include "UIElementPropertiesDetail.h"
#include "InspectorControls.h"

#include <unordered_map>
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
#include "sage/ui/UIButton.h"
#include "sage/ui/UIPart.h"
#include "sage/ui/UIIcons.h"
#include "sage/ui/UIPresets.h"
#include "sage/ui/UISerialize.h"
#include "sage/ui/ShapePath.h"
#include "sage/ui/UIStyle.h"
#include "sage/core/Paths.h"
#include "ui/ColorPicker.h"

namespace fs = std::filesystem;

namespace ictl = sage::editor::ictl;

namespace sage::editor {

using namespace uiprops_detail;

namespace uiprops_detail {

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

// Якорь — сеткой 3x3, а не списком из девяти строк.
//
// Якорь ЕСТЬ положение на экране: «сверху слева», «по центру», «снизу справа».
// Выпадающий список заставлял читать девять названий и держать в голове, какое
// из них соответствует нужному углу; сетка показывает это буквально — кнопка
// стоит там же, где встанет элемент.
namespace uiprops_detail {

const void* DefaultPartData(const ui::PartType& p) {
    static entt::registry s_reg;
    static std::unordered_map<const ui::PartType*, entt::entity> s_made;
    auto it = s_made.find(&p);
    if (it == s_made.end()) {
        const entt::entity e = s_reg.create();
        if (p.Add) p.Add(s_reg, e);
        it = s_made.emplace(&p, e).first;
    }
    return p.Get && p.Has && p.Has(s_reg, it->second) ? p.Get(s_reg, it->second) : nullptr;
}

const ui::PartField* FieldByKey(const ui::PartType& p, const char* key) {
    if (!p.Fields) return nullptr;
    for (const ui::PartField& f : *p.Fields)
        if (f.Key && std::strcmp(f.Key, key) == 0) return &f;
    return nullptr;
}

std::string DisplayLabel(const char* label) {
    // «Sprite (x,y,w,h)» → «Sprite»: оси теперь подписаны у самих полей.
    std::string s = label ? label : "";
    const size_t open = s.rfind(" (");
    if (open != std::string::npos && !s.empty() && s.back() == ')' && s.find(',', open) != std::string::npos)
        s.erase(open);
    return s;
}

} // namespace uiprops_detail

bool DrawAnchorPicker(UIAnchor& anchor) {
    // Порядок — тот же, что у UIAnchor (строка * 3 + столбец), и это не
    // случайность: сетка кнопок и есть перечисление, разложенное по экрану.
    static const char* const kAnchorIcons[9] = {
        "anchor-tl", "anchor-tc", "anchor-tr",
        "anchor-cl", "anchor-cc", "anchor-cr",
        "anchor-bl", "anchor-bc", "anchor-br",
    };
    static const char* const kAnchorNames[9] = {
        "Top left", "Top", "Top right", "Left", "Center", "Right", "Bottom left", "Bottom", "Bottom right",
    };
    // СЕТКА 3×3 — ОДИН КОНТРОЛ kit: общая рамка, равные ячейки через тонкие
    // разделители, выбранная — акцентной подложкой. Подпись — в колонке подписей.
    ictl::GroupItem items[9];
    for (int i = 0; i < 9; ++i) {
        items[i].Icon = kAnchorIcons[i];
        items[i].Tooltip = T(kAnchorNames[i]);
        items[i].Active = (int)anchor == i;
    }
    ictl::Label(T("Anchor"), 3.0f * ImGui::GetFrameHeight());
    const int hit = ictl::ButtonGroup("##anchor", items, 9, ictl::ButtonLook::IconOnly, 3);
    const bool changed = hit >= 0 && hit != (int)anchor;
    if (changed) anchor = (UIAnchor)hit;
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
    // Значение по умолчанию — из части, созданной заново: к нему ведёт сброс.
    const void* def = DefaultPartData(part);

    ImGui::PushID(f.Key);
    switch (f.Type) {
        case K::Bool:
            changed = ictl::Switch(host, label, &ui::FieldAs<bool>(data, f));
            break;

        case K::Int: {
            int& v = ui::FieldAs<int>(data, f);
            const int min = f.Max > f.Min ? (int)f.Min : -1000000, max = f.Max > f.Min ? (int)f.Max : 1000000;
            changed = ictl::Integer(host, label, &v, min, max, def != nullptr, def ? ui::FieldAs<int>(def, f) : 0);
            break;
        }

        case K::Float: {
            float& v = ui::FieldAs<float>(data, f);
            const float reset = def ? ui::FieldAs<float>(def, f) : 0.0f;
            // Небольшой ограниченный диапазон (доли 0..1) — ползунок с точным
            // полем; остальное — числовое поле с шагом по диапазону.
            // ЗНАЧЕНИЕ В СВОЁМ ДИАПАЗОНЕ: у части есть поля min и max (ползунок,
            // шкала) — значение правится ползунком именно в них.
            const ui::PartField* fmin = FieldByKey(part, "min");
            const ui::PartField* fmax = FieldByKey(part, "max");
            const ui::PartField* ftoggle = FieldByKey(part, "toggle");
            if (std::strcmp(f.Key, "value") == 0 && ftoggle && ftoggle->Type == K::Bool &&
                ui::FieldAs<bool>(data, *ftoggle) && fmin && fmax) {
                // Значение переключателя — вкл/выкл, а не число: концы диапазона.
                const float lo = ui::FieldAs<float>(data, *fmin), hi = ui::FieldAs<float>(data, *fmax);
                bool on = v >= (lo + hi) * 0.5f;
                if (ictl::Switch(host, label, &on)) {
                    v = on ? hi : lo;
                    changed = true;
                }
            } else if (std::strcmp(f.Key, "value") == 0 && fmin && fmax && fmin->Type == K::Float &&
                ui::FieldAs<float>(data, *fmax) > ui::FieldAs<float>(data, *fmin)) {
                const float lo = ui::FieldAs<float>(data, *fmin), hi = ui::FieldAs<float>(data, *fmax);
                changed = ictl::SliderNumber(host, label, &v, lo, hi, hi - lo <= 2.0f ? "%.2f" : "%.1f",
                                             def != nullptr, reset);
            } else if (std::strcmp(f.Key, "min") == 0 && fmax && fmax->Type == K::Float) {
                // Min и Max — в одной строке группы Range.
                ictl::NumOpts o = f.Max > f.Min ? ictl::OptsForRange(f.Min, f.Max) : ictl::NumOpts{};
                changed = ictl::Range(host, T("Range"), &v, &ui::FieldAs<float>(data, *fmax), o);
            } else if (std::strcmp(f.Key, "max") == 0 && fmin && fmin->Type == K::Float) {
                // Нарисовано вместе с min.
            } else if (f.Max > f.Min && f.Max - f.Min <= 1.0001f) {
                changed = ictl::SliderNumber(host, label, &v, f.Min, f.Max, "%.2f", def != nullptr, reset);
            } else {
                ictl::NumOpts o = f.Max > f.Min ? ictl::OptsForRange(f.Min, f.Max) : ictl::NumOpts{};
                if (!(f.Max > f.Min)) o.Format = "%.2f", o.Step = 0.1f;
                o.HasReset = def != nullptr;
                o.Reset = reset;
                changed = ictl::Number(host, label, &v, o);
            }
            break;
        }

        case K::String: {
            std::string& v = ui::FieldAs<std::string>(data, f);
            const bool slot = f.Editor == W::Texture || f.Editor == W::Font || f.Editor == W::Sound;
            // Ресурс: подпись группы и компактный слот под ней.
            if (slot && f.Editor != W::Texture) ictl::GroupTitle(label);
            if (f.Editor == W::Texture && DrawImageSource(host, f.Key, v, changed)) {
            } else if (f.Editor == W::Texture) {
                ictl::GroupTitle(label);
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
            } else if (f.Editor == W::Sound) {
                const assetslot::Result r =
                    assetslot::Draw(host, f.Key, assetslot::Kind::Audio, v, ctx.Preview,
                                    f.Tooltip ? T(f.Tooltip) : label);
                if (r.Changed) {
                    host.PushUndoSnapshot();
                    v = r.Path;
                    changed = true;
                }
                if (r.BrowseRequested && ctx.Browser) {
                    FileBrowser::Config c;
                    c.Title = T("Choose a sound");
                    c.Filters = assetslot::Extensions(assetslot::Kind::Audio);
                    c.FilterLabel = T("Sounds");
                    c.StartDir = c.Root = assetslot::ProjectRoot(host);
                    ctx.Browser->Open(c);
                    if (ctx.BrowseTarget) *ctx.BrowseTarget = &v;
                }
            } else if (f.Editor == W::IconName) {
                // Значки выбираются ГЛАЗАМИ: список имён без картинок — это
                // угадывание, как выглядит «drop» и чем он отличается от «wire».
                ictl::Label(label);
                if (ictl::BeginDropdown("##v", v.empty() ? T("(none)") : v.c_str())) {
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
                    ictl::EndDropdown();
                }
            } else {
                char buf[512];
                std::snprintf(buf, sizeof(buf), "%s", v.c_str());
                const bool multi = f.Editor == W::Multiline;
                // Многострочное — подпись над полем во всю ширину; строка —
                // подпись слева.
                if (multi) ictl::GroupTitle(label);
                else ictl::Label(label);
                if (multi ? ImGui::InputTextMultiline("##v", buf, sizeof(buf),
                                                      ImVec2(-1.0f, ImGui::GetFrameHeight() * 2.6f))
                          : ImGui::InputText("##v", buf, sizeof(buf))) {
                    v = buf;
                    changed = true;
                }
                host.TrackLastImGuiItem();
            }
            break;
        }

        case K::Color:
            changed = ictl::Color(host, label, &ui::FieldAs<glm::vec4>(data, f));
            break;

        case K::Vec2: {
            // Оси подписаны: X/Y, а у размеров — Width/Height.
            glm::vec2& v = ui::FieldAs<glm::vec2>(data, f);
            ictl::NumOpts o = f.Max > f.Min ? ictl::OptsForRange(f.Min, f.Max) : ictl::NumOpts{};
            const std::string key = f.Key ? f.Key : "";
            const bool isSize = key.find("ize") != std::string::npos;
            changed = ictl::PairRow(host, DisplayLabel(label).c_str(), &v.x, &v.y, isSize ? "W" : "X",
                                    isSize ? "H" : "Y", o);
            break;
        }

        case K::Vec4:
            if (f.Editor == W::NineSliceBorder) {
                // Числа здесь остаются — иногда нарезка известна точно и её
                // просто вписывают. Но подбирают её ГЛАЗАМИ, по картинке, и
                // кнопка рядом ведёт туда, где это видно.
                ictl::NumOpts so = ictl::OptsForRange(f.Min, f.Max);
                changed = ictl::Sides(host, DisplayLabel(label).c_str(), &ui::FieldAs<glm::vec4>(data, f), so);
                if (ImGui::Button(T("Edit on the picture…"))) {
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
        {
            // Кусок листа — X/Y/W/H; остальные четвёрки — стороны L/T/R/B со
            // связью (поля, отступы, вставки).
            glm::vec4& v = ui::FieldAs<glm::vec4>(data, f);
            ictl::NumOpts o = f.Max > f.Min ? ictl::OptsForRange(f.Min, f.Max) : ictl::NumOpts{};
            if (std::string(label).find("(x,y,w,h)") != std::string::npos ||
                std::string(f.Label ? f.Label : "").find("(x,y,w,h)") != std::string::npos) {
                ictl::GroupTitle(DisplayLabel(label).c_str());
                const char* xywh[4] = {"X", "Y", "W", "H"};
                changed = ictl::Axes(host, f.Key, &v.x, 4, xywh, o);
            } else {
                changed = ictl::Sides(host, DisplayLabel(label).c_str(), &v, o);
            }
            break;
        }

        case K::Enum: {
            int& v = ui::FieldAs<int>(data, f);
            const char* preview =
                (f.EnumNames && v >= 0 && v < f.EnumCount) ? T(f.EnumNames[v]) : "?";
            ictl::Label(label);
            if (ictl::BeginDropdown("##v", preview)) {
                for (int i = 0; i < f.EnumCount; ++i) {
                    if (ImGui::Selectable(T(f.EnumNames[i]), i == v)) {
                        host.PushUndoSnapshot();
                        v = i;
                        changed = true;
                    }
                }
                ictl::EndDropdown();
            }
            break;
        }


        // Вид — заголовок группы; его поля рисует DrawPartFields.
        case K::Look:
            break;
    }
    if (f.Tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T(f.Tooltip));
    if (f.Type != K::Look) NoteStyleOverride(host, e, part, f, changed || ImGui::IsItemEdited());
    ImGui::PopID();

    // Картинке после смены пути нужен новый рантайм-указатель: иначе слот
    // показывает новый файл, а элемент рисует старый.
    if (changed && part.Id && std::string(part.Id) == "image") {
        if (ui::Image* im = reg.try_get<ui::Image>(e)) {
            // Тем же путём, что и кадр (он знает и про «rt:»): загрузка здесь
            // своими правилами грузила съёмку как файл и ругалась в журнал.
            im->Tex.reset();
            im->TexPath.clear();
            ui::EnsureImageTexture(*im);
        }
    }
}

namespace uiprops_detail {

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

bool Passes(const ui::PartField& f, FieldFilter filter, bool splitContent) {
    if (!splitContent) return filter == FieldFilter::Look;
    return (filter == FieldFilter::Behaviour) == f.Content;
}

// Оформление элемента, чьи поля сейчас рисуются: в оформлении движка поля
// «только для своего» не показываются вовсе. Одно на кадр инспектора — он
// рисует один элемент за раз.
bool g_engineSkin = false;

// Есть ли у элемента часть, без которой поле не имеет смысла.
bool RequirementMet(const entt::registry& reg, entt::entity e, const ui::PartField& f) {
    if (g_engineSkin && f.CustomOnly) return false;
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
    if (view.BySection && !SameTab(f.Section, view.Section)) return false;
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
    if (anyAdvanced && ictl::SubSection((std::string(p.Id ? p.Id : "") + "." + std::to_string(first) + ".more").c_str(),
                                        T("More settings"))) {
        for (size_t k = first; k < end; ++k) {
            const ui::PartField& lf = fields[k];
            if (lf.Hidden || !lf.Advanced || !ui::FieldVisible(fields, lf, data) || !RequirementMet(reg, e, lf))
                continue;
            DrawOneField(host, obj, ctx, p, lf, data, selected);
        }
        ictl::EndSubSection();
    }
}

// Поля ОДНОЙ части под нынешний показ. Виды (Kind::Look) — свёрнутыми
// группами: у ползунка их три, и раскрытые разом они были бы простынёй.
// Возвращает, сколько нарисовано (заголовки видов тоже считаются).
int DrawFieldsOf(EditorHost& host, GameObject obj, const UIPropsContext& ctx, const ui::PartType& p,
                 void* data, const FieldView& view, int selected, bool dryRun) {
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
                              (!view.BySection || SameTab(f.Section, view.Section)) &&
                              f.Advanced == view.Advanced && RequirementMet(reg, e, f) &&
                              ui::FieldVisible(fields, f, data);
            // Вид, у которого показать нечего, кроме цвета (оформление движка),
            // — одной строкой с подписью вида, а не раскрывашкой ради поля.
            int visibleInside = 0;
            const ui::PartField* only = nullptr;
            for (size_t k = i + 1; k < end; ++k) {
                const ui::PartField& lf = fields[k];
                if (lf.Hidden || !ui::FieldVisible(fields, lf, data) || !RequirementMet(reg, e, lf)) continue;
                ++visibleInside;
                only = &lf;
            }
            if (show && visibleInside == 1 && !view.Tab) {
                ++drawn;
                if (!dryRun) {
                    ui::PartField inl = *only;
                    inl.Label = f.Label;
                    DrawOneField(host, obj, ctx, p, inl, data, selected);
                }
            } else if (show && visibleInside > 0) {
                ++drawn;
                if (!dryRun) {
                    ImGui::PushID(f.Key);
                    // Вид состояния на своей вкладке раскрыт сразу: вкладку и
                    // открывают ради него.
                    const std::string gid = std::string(p.Id ? p.Id : "") + "." + f.Key + (view.Tab ? view.Tab : "");
                    const bool open = ictl::SubSection(gid.c_str(), T(f.Label), view.Tab != nullptr);
                    if (f.Tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T(f.Tooltip));
                    if (open) {
                        DrawLookGroup(host, obj, ctx, p, fields, i + 1, end, data, reg, e, selected);
                        ictl::EndSubSection();
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
    ImGui::PushID(view.Section ? view.Section : "");
    view.Advanced = false;
    DrawFieldsOf(host, obj, ctx, p, data, view, selected);
    view.Advanced = true;
    if (DrawFieldsOf(host, obj, ctx, p, data, view, selected, /*dryRun=*/true) > 0 &&
        ictl::SubSection((std::string(p.Id ? p.Id : "part") + (view.Tab ? view.Tab : "") +
                          (view.Section ? view.Section : ""))
                             .c_str(),
                         T("Advanced"))) {
        DrawFieldsOf(host, obj, ctx, p, data, view, selected);
        ictl::EndSubSection();
    }
    ImGui::PopID();
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

// У кого выбирается оформление None / Image / Shape: у Panel и у кнопки.
bool AppearanceSwitchable(const ui::Preset& t) {
    return (t.HasShape || t.HasFill) && (t.Container || t.Name == "Button");
}

// Входит ли часть в устройство типа. По флагам заготовки — тем же, по которым
// тип и собирается (ApplyPreset), а не своим списком.
bool TypeHasPart(const ui::Preset& t, const char* id) {
    const std::string s = id ? id : "";
    return (s == "fill" && t.HasFill) || (s == "label" && t.HasLabel) ||
           (s == "image" && t.HasImage) || (s == "bar" && t.HasBar) ||
           (s == "interactable" && t.HasInteractable) || (s == "textInput" && t.HasInput) ||
           (s == "range" && t.HasRange) || (s == "arrangement" && t.HasArrangement) ||
           (s == "dropdown" && t.HasDropdown) ||
           // Обрезка и прокрутка — настройки контейнера («Clip Children»,
           // «Scroll»), а не добавка сверх типа.
           ((s == "mask" || s == "scroll") && t.Container) || (s == "icon" && t.HasIcon) ||
           (s == "shape" && t.HasShape) ||
           // Оформление Panel — форма ИЛИ картинка (Appearance): обе части её.
           ((s == "fill" || s == "shape") && AppearanceSwitchable(t));
}

// APPEARANCE панели: None | Image | Shape. Режим — это набор частей, а не
// поле: «картинка» — подложка (Fill) с её видом, «форма» — часть Shape.
// Значения прежнего режима не держатся: части снимаются целиком, и
// переключение — шаг отмены, который возвращает их.
void DrawAppearanceSwitch(EditorHost& host, entt::entity e) {
    entt::registry& reg = host.CurrentScene().Registry();
    const bool shape = reg.all_of<ui::Shape>(e);
    const bool fill = reg.all_of<ui::Fill>(e);
    int mode = shape ? 2 : fill ? 1 : 0;
    const char* kModes[] = {T("None"), T("Image"), T("Shape")};
    ictl::Label(T("Background"));
    if (!ictl::Combo("##Appearance", &mode, kModes, 3)) return;
    host.PushUndoSnapshot();
    if (mode != 2) reg.remove<ui::Shape>(e);
    if (mode != 1) reg.remove<ui::Fill>(e);
    if (mode == 1 && !fill) {
        ui::Fill& f = reg.emplace<ui::Fill>(e);
        f.Color = {1.0f, 1.0f, 1.0f, 1.0f};   // картинка без оттенка
        f.Rounding = 0.0f;
    }
    if (mode == 2 && !shape) reg.emplace<ui::Shape>(e);
}

// СВОЙ КОНТУР: что делать мышью — сказано рядом с полями, а новый контур
// (форма или отверстие) добавляется кнопкой: его негде «нарисовать» без точки.
void DrawCustomShapeTools(EditorHost& host, entt::entity e) {
    entt::registry& reg = host.CurrentScene().Registry();
    ui::Shape* shape = reg.try_get<ui::Shape>(e);
    if (!shape || shape->Form != ui::Shape::Geometry::Custom) return;
    HintWrapped("%s", T("On the canvas: drag points and handles (Alt — one handle), double-click a line to add "
                        "a point, right-click a point for a curve, delete or open/close. A contour inside "
                        "another one cuts a hole."));
    if (ImGui::Button(T("Add contour"))) {
        host.PushUndoSnapshot();
        std::vector<ui::PathContour> c;
        ui::ParseShapePath(shape->Path, c);
        if (c.empty()) c.push_back({{{{0, 0}}, {{1, 0}}, {{1, 1}}, {{0, 1}}}, true});
        c.push_back({{{{0.35f, 0.35f}}, {{0.65f, 0.35f}}, {{0.65f, 0.65f}}, {{0.35f, 0.65f}}}, true});
        shape->Path = ui::FormatShapePath(c);
    }
}

// CONTENT контейнера: Arrangement — полями своей части (поля режима видны
// только в нём), Clip Children — галкой, Scroll — выбором оси. Обрезка и
// прокрутка остаются частями (Mask, Scroll), но включаются здесь, а не
// «добавкой»: это настройки контейнера, а не новые элементы.
void DrawContentSection(EditorHost& host, GameObject obj, const UIPropsContext& ctx, int selected) {
    entt::registry& reg = host.CurrentScene().Registry();
    const entt::entity e = obj.Entity();
    if (const ui::PartType* arr = ui::FindPart("arrangement")) {
        if (!arr->Has(reg, e)) {
            // Контейнер без своей части (собран кодом) получает её здесь: у
            // любого контейнера есть Arrangement, хотя бы ручной.
            if (ImGui::Button(T("Arrange children"))) {
                host.PushUndoSnapshot();
                arr->Add(reg, e);
            }
        } else if (void* data = arr->GetMutable(reg, e)) {
            ImGui::PushID("arrangement");
            DrawPartSection(host, obj, ctx, *arr, data, FieldView{}, selected);
            ImGui::PopID();
        }
    }

    ImGui::Spacing();
    const ui::PartType* mask = ui::FindPart("mask");
    bool clip = mask && mask->Has(reg, e);
    if (mask && ictl::Switch(host, T("Clip Children"), &clip)) {
        // Источник обрезки — автоматически по оформлению контейнера: форма
        // Panel режет её геометрией, картинка — альфой (Mask::Shape::Auto).
        if (clip) reg.emplace_or_replace<ui::Mask>(e).Form = ui::Mask::Shape::Auto;
        else mask->Remove(reg, e);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("Children are cut off at the edges of this container"));
    if (clip && mask) {
        if (void* data = mask->GetMutable(reg, e)) {
            ImGui::PushID("mask");
            ImGui::Indent();
            DrawPartSection(host, obj, ctx, *mask, data, FieldView{}, selected);
            ImGui::Unindent();
            ImGui::PopID();
        }
    }

    // Прокрутка: ось выбирается одним списком, а не двумя галками и добавкой.
    if (const ui::PartType* scroll = ui::FindPart("scroll")) {
        ui::Scroll* sc = reg.try_get<ui::Scroll>(e);
        int axis = !sc ? 0 : sc->Horizontal && sc->Vertical ? 3 : sc->Horizontal ? 1 : 2;
        const char* kAxes[] = {T("Off"), T("Horizontal"), T("Vertical"), T("Both")};
        ictl::Label(T("Scroll"));
        if (ictl::Combo("##Scroll", &axis, kAxes, 4)) {
            host.PushUndoSnapshot();
            if (axis == 0) {
                scroll->Remove(reg, e);
            } else {
                ui::Scroll& s = reg.get_or_emplace<ui::Scroll>(e);
                s.Horizontal = axis == 1 || axis == 3;
                s.Vertical = axis == 2 || axis == 3;
            }
            sc = reg.try_get<ui::Scroll>(e);
        }
        if (sc) {
            if (void* data = scroll->GetMutable(reg, e)) {
                ImGui::PushID("scroll");
                ImGui::Indent();
                FieldView v;
                DrawPartSection(host, obj, ctx, *scroll, data, v, selected);
                ImGui::Unindent();
                ImGui::PopID();
            }
        }
    }
}

// Разделы инспектора. Объявлены здесь, определены ниже: главная функция читается
// как оглавление.

} // namespace uiprops_detail

} // namespace sage::editor
