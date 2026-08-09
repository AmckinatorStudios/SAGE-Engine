#include "ScriptEngine.h"

#include "sage/core/Log.h"
#include "sage/render/ResourceManager.h"
#include "sage/ui/UIDemos.h"
#include "sage/ui/UIShowcase.h"
#include "sage/ui/UISceneSystem.h"
#include "sage/ui/UIIcons.h"
#include "sage/ui/UI.h"
#include "sage/ui/UIPresets.h"
#include "sage/ui/UILegacy.h"

#include <algorithm>
#include <cmath>

// ---------------------------------------------------------------------------
// Интерфейс сцены: элемент из компонентов и sage.ui.*
//
// Часть Lua-API движка. Раньше ВСЕ привязки жили в одном ScriptEngine.cpp на
// 1800 строк: 126 функций, восемнадцать областей, и чтобы дописать одну
// строчку про анимацию, приходилось листать интерфейс, физику и таймеры.
// Определения разъехались по файлам ScriptApi_*.cpp — по файлу на область;
// объявления методов остались в ScriptEngine.h, поэтому порядок регистрации
// по-прежнему записан в одном месте (RegisterEngineApi) и не зависит от того,
// в каком файле лежит тело.
// ---------------------------------------------------------------------------

namespace {

// Ссылка на элемент интерфейса для Lua.
//
// ЗАЧЕМ ПРОКСИ, А НЕ УКАЗАТЕЛЬ НА КОМПОНЕНТ. Элемент интерфейса — это НАБОР
// компонентов (Transform + Fill + Label + ...), а скрипты во всех играх пишут
// `obj:GetUI().Text = "..."`. Отдать им указатель на один из компонентов
// значит либо сломать все эти скрипты, либо навсегда оставить в движке
// компонент-мешок ради удобства одной привязки.
//
// Прокси решает обе задачи: снаружи он выглядит как прежний элемент, внутри
// читает и пишет ровно ту часть, которой поле принадлежит. И делает ещё одно,
// чего плоский компонент не умел: ЗАПИСЬ СОЗДАЁТ ЧАСТЬ. `ui.Text = "Дальше"`
// на голой панели заводит ей надпись — раньше поле существовало всегда и
// молча ничего не значило, если вид элемента был не тот.
// Вид элемента, как его видит Lua. Держится здесь, а не в движке: сам движок
// видами не оперирует — он оперирует частями.
enum class UIKind { Panel, Label, Image, Bar, Icon, Input, Checkbox, Slider };

struct UIRef {
    entt::registry* Reg = nullptr;
    entt::entity E = entt::null;

    bool Alive() const { return Reg && Reg->valid(E); }
    template <typename T> T& Part() const { return Reg->get_or_emplace<T>(E); }
    template <typename T> const T* Peek() const { return Alive() ? Reg->try_get<T>(E) : nullptr; }
    template <typename T> void Drop() const { if (Alive()) Reg->remove<T>(E); }
};

// Поле части: чтение отдаёт значение по умолчанию, если части нет (у надписи
// нет скругления — и спрашивать его не ошибка), запись часть создаёт.
#define UI_FIELD(Comp, field)                                                        \
    sol::property([](UIRef& r) { const Comp* c = r.Peek<Comp>();                     \
                                 return c ? c->field : Comp{}.field; },              \
                  [](UIRef& r, decltype(Comp{}.field) v) {                           \
                      if (r.Alive()) r.Part<Comp>().field = v; })

// Рантайм-состояние: только чтение. Его пишет система ввода, и разрешить игре
// «нажать кнопку присваиванием» значило бы завести второй источник правды.
#define UI_STATE(field)                                                              \
    sol::readonly_property([](UIRef& r) {                                            \
        const sage::ui::Interactable* a = r.Peek<sage::ui::Interactable>();           \
        return a ? a->Runtime.field : sage::ui::State{}.field; })

// Признак наличия части: чтение — есть ли она, запись — добавить или убрать.
#define UI_HAS(Comp)                                                                 \
    sol::property([](UIRef& r) { return r.Peek<Comp>() != nullptr; },                \
                  [](UIRef& r, bool on) {                                            \
                      if (!r.Alive()) return;                                        \
                      if (on) r.Part<Comp>(); else r.Drop<Comp>(); })

// «Вид элемента» для Lua. У набора компонентов вида НЕТ — он выводится из
// того, из чего элемент собран, и существует ровно ради обратной совместимости
// скриптов: `ui.Type == UIKind.Bar` писали до перехода на компоненты.
//
// Читается как ответ на вопрос «чем этот элемент является в первую очередь»,
// пишется как «сделай из этого вот такой элемент»: ставится определяющая часть
// и снимаются чужие определяющие.
UIKind KindOf(const UIRef& r) {
    using namespace sage::ui;
    if (r.Peek<sage::ui::Image>()) return UIKind::Image;
    if (r.Peek<TextInput>()) return UIKind::Input;
    if (const Range* range = r.Peek<Range>())
        return range->Toggle ? UIKind::Checkbox : UIKind::Slider;
    if (r.Peek<Bar>()) return UIKind::Bar;
    if (r.Peek<Icon>() && !r.Peek<Fill>() && !r.Peek<Label>()) return UIKind::Icon;
    if (r.Peek<Label>() && !r.Peek<Fill>()) return UIKind::Label;
    return UIKind::Panel;
}

void SetKind(UIRef& r, UIKind kind) {
    using Kind = UIKind;
    using namespace sage::ui;
    if (!r.Alive()) return;
    r.Drop<sage::ui::Image>();
    r.Drop<Bar>();
    r.Drop<TextInput>();
    r.Drop<Range>();
    switch (kind) {
        case Kind::Image: r.Part<sage::ui::Image>(); break;
        case Kind::Bar:   r.Part<Bar>(); break;
        case Kind::Input: r.Part<TextInput>(); r.Part<Label>(); r.Part<Interactable>(); break;
        case Kind::Slider: r.Part<Range>().Toggle = false; r.Part<Interactable>(); break;
        case Kind::Checkbox: {
            Range& range = r.Part<Range>();
            range.Toggle = true;
            range.Step = 1.0f;
            r.Part<Interactable>();
            break;
        }
        case Kind::Icon:  r.Part<Icon>(); break;
        case Kind::Label: r.Part<Label>(); r.Drop<Fill>(); break;
        case Kind::Panel: r.Part<Fill>(); break;
    }
}

// Значение: у полосы — её заполнение, у ползунка и галки — доля 0..1.
//
// Доля, а не игровые единицы, потому что таким это поле было всегда, и скрипты
// пишут в него 0.5. Игровые единицы отдаёт ui.GetValue — и он же остаётся
// единственным местом перевода.
float GetValue(UIRef& r) {
    if (const sage::ui::Range* range = r.Peek<sage::ui::Range>()) {
        const float span = range->Max - range->Min;
        return std::abs(span) < 1e-6f ? 0.0f : (range->Value - range->Min) / span;
    }
    if (const sage::ui::Bar* bar = r.Peek<sage::ui::Bar>()) return bar->Value;
    return 0.0f;
}

void SetValue(UIRef& r, float v) {
    if (!r.Alive()) return;
    if (sage::ui::Range* range = r.Peek<sage::ui::Range>() ? &r.Part<sage::ui::Range>() : nullptr) {
        range->Value = range->Min + glm::clamp(v, 0.0f, 1.0f) * (range->Max - range->Min);
        return;
    }
    r.Part<sage::ui::Bar>().Value = v;
}

// Элемент сущности для Lua. Пусто (nil) — у сущности интерфейсной части нет.
sol::object UIOf(GameObject& obj, sol::this_state ts) {
    if (!obj.Valid() || !sage::ui::IsElement(*obj.Registry(), obj.Entity()))
        return sol::make_object(ts, sol::lua_nil);
    return sol::make_object(ts, UIRef{obj.Registry(), obj.Entity()});
}

// --- Раскладка, холст и группа из скрипта ------------------------------------
//
// Эти три компонента существовали в движке с самого появления новой системы
// интерфейса, но добраться до них можно было ТОЛЬКО из редактора: инспектор их
// показывал, а Lua-API — нет. Для игры, которая собирает свои экраны скриптом
// (а такая игра на этом движке одна и есть — «The Boat»), это означало, что
// сетка инвентаря раскладывается вручную по формуле «i-й слот в столбце
// i % columns», меню паузы рисуется поверх худа только если угадать номер слоя,
// а спрятать панель целиком нечем, кроме обхода всех её детей.
//
// Разбор таблицы, а не десяток отдельных функций: раскладка задаётся ОДНИМ
// решением («это сетка по пять в ряд с зазором шесть»), и разносить его по
// пяти вызовам значило бы дать возможность настроить половину.
sage::ui::Layout::Flow FlowFromName(const std::string& name) {
    if (name == "row" || name == "horizontal") return sage::ui::Layout::Flow::Horizontal;
    if (name == "grid") return sage::ui::Layout::Flow::Grid;
    return sage::ui::Layout::Flow::Vertical;
}

sage::ui::Layout::Align AlignFromName(const std::string& name) {
    if (name == "center") return sage::ui::Layout::Align::Center;
    if (name == "end") return sage::ui::Layout::Align::End;
    if (name == "between" || name == "space-between") return sage::ui::Layout::Align::SpaceBetween;
    return sage::ui::Layout::Align::Start;
}

// Поля контейнера: число («со всех сторон одинаково») или Vec4 (л, в, п, н).
// Одно число покрывает девять случаев из десяти, и требовать ради него вектор
// значило бы делать типичный вызов длиннее ради редкого.
glm::vec4 PaddingFrom(const sol::object& value, glm::vec4 fallback) {
    if (value.is<float>()) {
        const float p = value.as<float>();
        return {p, p, p, p};
    }
    if (value.is<glm::vec4>()) return value.as<glm::vec4>();
    return fallback;
}

} // namespace

void ScriptEngine::RegisterUIApi() {
    // --- UI: интерфейс сцены (набор компонентов, см. sage/ui/UI.h).
    // Скрипты правят текст/значение/видимость через ссылку на элемент — так худ
    // (здоровье, счёт, таймеры) обновляется из игровой логики. ---
    m_lua.new_enum<UIKind>("UIKind", {
        {"Panel", UIKind::Panel},
        {"Icon",  UIKind::Icon},
        {"Input", UIKind::Input},
        {"Checkbox", UIKind::Checkbox},
        {"Slider", UIKind::Slider},
        {"Label", UIKind::Label},
        {"Image", UIKind::Image},
        {"Bar",   UIKind::Bar},
    });
    m_lua.new_enum<UIAnchor>("UIAnchor", {
        {"TopLeft", UIAnchor::TopLeft},       {"TopCenter", UIAnchor::TopCenter},
        {"TopRight", UIAnchor::TopRight},     {"CenterLeft", UIAnchor::CenterLeft},
        {"Center", UIAnchor::Center},         {"CenterRight", UIAnchor::CenterRight},
        {"BottomLeft", UIAnchor::BottomLeft}, {"BottomCenter", UIAnchor::BottomCenter},
        {"BottomRight", UIAnchor::BottomRight},
    });
    // Растяжение: панель во всю ширину экрана — это ОДНО поле, а не пересчёт
    // размера скриптом на каждое изменение окна. Затемнение под меню паузы
    // иначе приходится каждый кадр подгонять под разрешение вручную.
    m_lua.new_enum<sage::ui::Transform::Stretch>("UIStretch", {
        {"None", sage::ui::Transform::Stretch::None},
        {"Horizontal", sage::ui::Transform::Stretch::Horizontal},
        {"Vertical", sage::ui::Transform::Stretch::Vertical},
        {"Both", sage::ui::Transform::Stretch::Both},
    });

    m_lua.new_usertype<UIRef>("UIElement",
        // Где стоит.
        "Anchor", UI_FIELD(sage::ui::Transform, Anchor),
        "Offset", UI_FIELD(sage::ui::Transform, Offset),
        "Size", UI_FIELD(sage::ui::Transform, Size),
        "Layer", UI_FIELD(sage::ui::Transform, Layer),
        "Visible", UI_FIELD(sage::ui::Transform, Visible),
        // Растяжение и поля: «на весь экран с отступом 24» задаётся здесь, а не
        // пересчитывается скриптом при каждом изменении размера окна.
        "Stretch", UI_FIELD(sage::ui::Transform, Mode),
        "Margin", UI_FIELD(sage::ui::Transform, Margin),
        "Pivot", UI_FIELD(sage::ui::Transform, Pivot),
        // Групповые свойства поддерева: спрятать плавно — одно число на корне
        // панели вместо прохода по всем её детям с правкой альфы у каждого.
        "Alpha", UI_FIELD(sage::ui::Group, Alpha),
        "BlockRaycasts", UI_FIELD(sage::ui::Group, BlockRaycasts),
        "LayoutSize", sol::readonly_property([](UIRef& r) {
            const sage::ui::Transform* t = r.Peek<sage::ui::Transform>();
            return t ? t->LayoutSize : glm::vec2(0.0f);
        }),
        // Подложка.
        "Color", sol::property([](UIRef& r) {
                                   if (const sage::ui::Image* im = r.Peek<sage::ui::Image>())
                                       return im->Tint;
                                   const sage::ui::Fill* f = r.Peek<sage::ui::Fill>();
                                   return f ? f->Color : glm::vec4(0.0f);
                               },
                               [](UIRef& r, glm::vec4 v) {
                                   if (!r.Alive()) return;
                                   // У картинки «цвет» — это её тонирование:
                                   // так было и раньше, и скрипты красят
                                   // картинки именно через Color.
                                   if (r.Peek<sage::ui::Image>()) r.Part<sage::ui::Image>().Tint = v;
                                   else r.Part<sage::ui::Fill>().Color = v;
                               }),
        "Rounding", UI_FIELD(sage::ui::Fill, Rounding),
        "BorderThickness", UI_FIELD(sage::ui::Fill, BorderThickness),
        "BorderColor", UI_FIELD(sage::ui::Fill, BorderColor),
        "GradientColor", UI_FIELD(sage::ui::Fill, Gradient),
        "ShadowSize", UI_FIELD(sage::ui::Fill, ShadowSize),
        // Надпись.
        "Text", UI_FIELD(sage::ui::Label, Text),
        "TextScale", UI_FIELD(sage::ui::Label, Scale),
        "TextColor", UI_FIELD(sage::ui::Label, Color),
        "TextCentered", sol::property([](UIRef& r) {
                                          const sage::ui::Label* l = r.Peek<sage::ui::Label>();
                                          return l && l->Horizontal == sage::ui::Label::Align::Center;
                                      },
                                      [](UIRef& r, bool centered) {
                                          if (!r.Alive()) return;
                                          r.Part<sage::ui::Label>().Horizontal =
                                              centered ? sage::ui::Label::Align::Center
                                                       : sage::ui::Label::Align::Start;
                                      }),
        "WrapText", UI_FIELD(sage::ui::Label, Wrap),
        "AutoWidth", UI_FIELD(sage::ui::Label, AutoWidth),
        "PadX", UI_FIELD(sage::ui::Label, PadX),
        // Картинка.
        "TexturePath", UI_FIELD(sage::ui::Image, Path),
        "Sprite", UI_FIELD(sage::ui::Image, Sprite),
        "SpriteHover", UI_FIELD(sage::ui::Image, SpriteHover),
        "SpritePressed", UI_FIELD(sage::ui::Image, SpritePressed),
        "SliceBorder", UI_FIELD(sage::ui::Image, SliceBorder),
        "PixelScale", UI_FIELD(sage::ui::Image, PixelScale),
        "PixelArt", UI_FIELD(sage::ui::Image, PixelArt),
        // Полоса и значок.
        "BarFillColor", UI_FIELD(sage::ui::Bar, FillColor),
        "Icon", UI_FIELD(sage::ui::Icon, Name),
        "IconColor", UI_FIELD(sage::ui::Icon, Color),
        // Размер значка в пикселях; 0 — «во всю высоту элемента», как было.
        // Без него значок рядом с подписью всегда ростом с элемент, и на
        // кнопке меню высотой в полсотни пикселей он забивает саму подпись.
        "IconSize", UI_FIELD(sage::ui::Icon, Size),
        // Значение (полоса/ползунок/галка).
        "Value", sol::property(&GetValue, &SetValue),
        "MinValue", UI_FIELD(sage::ui::Range, Min),
        "MaxValue", UI_FIELD(sage::ui::Range, Max),
        // Поле ввода.
        "Placeholder", UI_FIELD(sage::ui::TextInput, Placeholder),
        "MaxLength", UI_FIELD(sage::ui::TextInput, MaxLength),
        "Password", UI_FIELD(sage::ui::TextInput, Password),
        // Поведение.
        "Interactive", UI_HAS(sage::ui::Interactable),
        "Enabled", UI_FIELD(sage::ui::Interactable, Enabled),
        "Action", UI_FIELD(sage::ui::Interactable, Action),
        "ClipChildren", UI_HAS(sage::ui::Mask),
        // Вид элемента — производная от набора частей (см. SetKind).
        "Type", sol::property(&KindOf, &SetKind),
        // Состояние взаимодействия — только чтение.
        "Hovered", UI_STATE(Hovered),
        "Pressed", UI_STATE(Pressed),
        "Clicked", UI_STATE(Clicked),
        "Changed", UI_STATE(Changed),
        "Focused", sol::property([](UIRef& r) {
                                     const sage::ui::Interactable* a = r.Peek<sage::ui::Interactable>();
                                     return a && a->Runtime.Focused;
                                 },
                                 [](UIRef& r, bool on) {
                                     if (r.Alive()) r.Part<sage::ui::Interactable>().Runtime.Focused = on;
                                 })
    );

    // Заготовка элемента: одной строкой то, для чего иначе нужно знать, какие
    // семь полей поменять у панели, чтобы получилась кнопка. Таблица заготовок
    // — общая с редактором (sage/ui/UIPresets.h), поэтому Button из скрипта и
    // Button из меню редактора — один и тот же элемент, а не два похожих.
    Bind("ui", "Preset", "SetUIPreset", [](GameObject& obj, const std::string& preset) {
        if (!obj.Valid()) throw std::runtime_error("ui.Preset: сущность недействительна");
        if (!sage::ui::ApplyPreset(*obj.Registry(), obj.Entity(), preset)) {
            // Молчаливый отказ означал бы «элемент создан, но выглядит не так»
            // — и искать причину пришлось бы в отрисовке.
            std::string known;
            for (const std::string& n : sage::ui::PresetNames()) {
                if (!known.empty()) known += ", ";
                known += n;
            }
            throw std::runtime_error("ui.Preset: неизвестная заготовка '" + preset +
                                     "'; есть: " + known);
        }
    });

    // Раскладка детей: ряд, столбец, сетка. Родитель сам расставляет детей по
    // порядку Layer, а размеры и зазоры заданы один раз.
    //
    // Ради чего это в скриптах: сетка инвентаря и меню из пяти кнопок иначе
    // раскладываются формулой «x = -total/2 + i * (slot + gap)», посчитанной в
    // самом скрипте. Формула не ошибается ровно до первого изменения размера
    // слота — а дальше её надо править в стольких местах, сколько сеток в игре.
    //
    //   sage.ui.SetLayout(panel, {dir = "grid", columns = 5, spacing = 6,
    //                             padding = 10, justify = "center", fit = true})
    Bind("ui", "SetLayout", "SetUILayout", [](GameObject& obj, sol::table opts) {
        if (!obj.Valid()) throw std::runtime_error("ui.SetLayout: сущность недействительна");
        obj.Registry()->get_or_emplace<sage::ui::Transform>(obj.Entity());
        sage::ui::Layout& layout = obj.Registry()->get_or_emplace<sage::ui::Layout>(obj.Entity());
        layout.Direction = FlowFromName(opts.get_or<std::string>("dir", "column"));
        layout.Justify = AlignFromName(opts.get_or<std::string>("justify", "start"));
        layout.Spacing = opts.get_or("spacing", layout.Spacing);
        layout.Padding = PaddingFrom(opts.get<sol::object>("padding"), layout.Padding);
        layout.Columns = std::max(1, opts.get_or("columns", layout.Columns));
        layout.StretchCross = opts.get_or("stretch", layout.StretchCross);
        layout.FitContent = opts.get_or("fit", layout.FitContent);
    });
    Bind("ui", "ClearLayout", "ClearUILayout", [](GameObject& obj) {
        if (obj.Valid()) obj.Registry()->remove<sage::ui::Layout>(obj.Entity());
    });

    // Холст: масштаб интерфейса и порядок МЕЖДУ корнями.
    //
    // Порядок — не украшение: меню паузы обязано быть поверх худа, а диалог —
    // поверх меню. Между корнями его задаёт только Canvas::SortOrder, и без
    // доступа к нему из скрипта игра могла лишь надеяться, что её меню создано
    // позже худа (порядок при равном SortOrder решает номер сущности).
    //
    //   sage.ui.SetCanvas(menuRoot, {order = 100, scale = true})
    Bind("ui", "SetCanvas", "SetUICanvas", [](GameObject& obj, sol::table opts) {
        if (!obj.Valid()) throw std::runtime_error("ui.SetCanvas: сущность недействительна");
        obj.Registry()->get_or_emplace<sage::ui::Transform>(obj.Entity());
        sage::ui::Canvas& canvas = obj.Registry()->get_or_emplace<sage::ui::Canvas>(obj.Entity());
        canvas.SortOrder = opts.get_or("order", canvas.SortOrder);
        if (opts["scale"].valid()) {
            canvas.Mode = opts.get_or("scale", false) ? sage::ui::Canvas::Scale::ScaleWithSize
                                                      : sage::ui::Canvas::Scale::Pixels;
        }
        if (opts["reference"].valid()) canvas.Reference = opts.get<glm::vec2>("reference");
        canvas.MatchWidthOrHeight = opts.get_or("match", canvas.MatchWidthOrHeight);
    });

    // Картинка UI-элемента: путь + признак пиксель-арта. Отдельной функцией, а
    // не полем TexturePath, потому что путь без ЗАГРУЗКИ ничего не рисует, а
    // загрузить надо с правильной фильтрацией — из скрипта об этом помнить
    // незачем.
    Bind("ui", "SetImage", "SetUIImage", [](GameObject& obj, const std::string& path,
                                        sol::optional<bool> pixelArt) {
        if (!obj.Valid() || !sage::ui::IsElement(*obj.Registry(), obj.Entity())) return;
        sage::ui::Image& im = obj.Registry()->get_or_emplace<sage::ui::Image>(obj.Entity());
        im.Path = path;
        im.PixelArt = pixelArt.value_or(false);
        im.Tex = path.empty() ? nullptr
                 : im.PixelArt
                     ? ResourceManager::Instance().GetTexture(path, TextureFilter::Nearest, false)
                     : ResourceManager::Instance().GetTexture(path);
    });

    // Спрайт из листа: кусок в ПИКСЕЛЯХ исходника. Так набор интерфейса
    // описывается ровно теми числами, что напечатаны в его документации.
    Bind("ui", "SetSprite", "SetUISprite", [](GameObject& obj, float x, float y, float w, float h) {
        if (!obj.Valid() || !sage::ui::IsElement(*obj.Registry(), obj.Entity())) return;
        obj.Registry()->get_or_emplace<sage::ui::Image>(obj.Entity()).Sprite = glm::vec4(x, y, w, h);
    });

    // Девятина: неподвижные углы в пикселях исходника (лево, верх, право, низ).
    Bind("ui", "SetSlice", "SetUISlice", [](GameObject& obj, float l, float t, float r, float b,
                                        sol::optional<float> scale) {
        if (!obj.Valid() || !sage::ui::IsElement(*obj.Registry(), obj.Entity())) return;
        sage::ui::Image& im = obj.Registry()->get_or_emplace<sage::ui::Image>(obj.Entity());
        im.SliceBorder = glm::vec4(l, t, r, b);
        im.PixelScale = scale.value_or(0.0f);
    });

    // Значение ползунка/галки в ЕДИНИЦАХ ИГРЫ — 0..100 громкости, -180..180
    // угла. Диапазон теперь хранит их прямо (sage::ui::Range), и эти две
    // функции стали тем, чем и должны были быть: коротким доступом к значению,
    // а не местом перевода долей в единицы.
    Bind("ui", "GetValue", "GetUIValue", [](GameObject& obj) -> float {
        if (!obj.Valid()) return 0.0f;
        if (const auto* range = obj.Registry()->try_get<sage::ui::Range>(obj.Entity()))
            return range->Value;
        if (const auto* bar = obj.Registry()->try_get<sage::ui::Bar>(obj.Entity()))
            return bar->Value;
        return 0.0f;
    });
    Bind("ui", "SetValue", "SetUIValue", [](GameObject& obj, float value) {
        if (!obj.Valid()) return;
        if (auto* range = obj.Registry()->try_get<sage::ui::Range>(obj.Entity())) {
            const float lo = glm::min(range->Min, range->Max);
            const float hi = glm::max(range->Min, range->Max);
            range->Value = glm::clamp(value, lo, hi);
            return;
        }
        if (auto* bar = obj.Registry()->try_get<sage::ui::Bar>(obj.Entity())) bar->Value = value;
    });

    // Размер картинки в пикселях — по нему скрипт нарезает лист на спрайты, не
    // зная его размера заранее.
    Bind("ui", "ImageSize", "ImageSize", [](const std::string& path) {
        std::shared_ptr<Texture> tex = ResourceManager::Instance().GetTexture(path);
        if (!tex) return std::make_tuple(0, 0);
        return std::make_tuple(tex->Width(), tex->Height());
    });

    // Список доступных векторных иконок — чтобы игра могла проверить имя, а не
    // узнавать об опечатке по пустому месту в интерфейсе.
    Bind("ui", "HasIcon", "HasIcon", [](const std::string& name) {
        return sage::ui::HasIcon(name);
    });
    Bind("ui", "IconNames", "IconNames", [this]() -> sol::table {
        sol::table list = m_lua.create_table();
        int i = 1;
        for (const std::string& n : sage::ui::IconNames()) list[i++] = n;
        return list;
    });

    // Собирает боевой интерфейс (инвентарь + дерево навыков) из компонентов
    // в сцену — плотный реальный экран одним вызовом. Возвращает id корня.
    Bind("ui", "SpawnShowcase", "SpawnUIShowcase", [this]() -> int {
        if (!m_scene) throw std::runtime_error("SpawnUIShowcase: сцена не привязана");
        return sage::ui::BuildShowcase(*m_scene);
    });

    // Готовый экран интерфейса: «menu», «hud», «settings» (см. sage/ui/UIDemos.h).
    // Не украшение: это работающий образец того, как из частей собирают экран, —
    // его можно поставить в сцену и разобрать в инспекторе.
    Bind("ui", "SpawnDemo", "SpawnUIDemo", [this](const std::string& name) -> int {
        if (!m_scene) throw std::runtime_error("SpawnUIDemo: сцена не привязана");
        const int id = sage::ui::BuildDemo(*m_scene, name);
        if (id < 0) {
            std::string known;
            for (const std::string& n : sage::ui::DemoNames()) {
                if (!known.empty()) known += ", ";
                known += n;
            }
            throw std::runtime_error("SpawnUIDemo: нет демо '" + name + "'; есть: " + known);
        }
        return id;
    });

    // Что нажали в интерфейсе за этот кадр — ПО ИМЕНИ ДЕЙСТВИЯ.
    //
    // Раньше игра узнавала о нажатии по номеру сущности, который меняется при
    // любой пересборке сцены: связь меню с логикой держалась на числе, которое
    // нигде не записано. Имя задаётся рядом с кнопкой в инспекторе.
    Bind("ui", "ClickedAction", "UIClickedAction", [this]() -> std::string {
        if (!m_scene) return {};
        for (auto e : m_scene->Registry().view<sage::ui::Interactable>()) {
            const auto& act = m_scene->Registry().get<sage::ui::Interactable>(e);
            if (act.Runtime.Clicked && !act.Action.empty()) return act.Action;
        }
        return {};
    });

    // Что под курсором — тем же именем действия. Пара к ClickedAction, и нужна
    // ровно там же, где она: подсказка о предмете, из чего он делается и чего
    // не хватает, показывается ДО щелчка, а не после. Без этого игре пришлось
    // бы опрашивать поле Hovered у каждого слота инвентаря каждый кадр.
    Bind("ui", "HoveredAction", "UIHoveredAction", [this]() -> std::string {
        if (!m_scene) return {};
        for (auto e : m_scene->Registry().view<sage::ui::Interactable>()) {
            const auto& act = m_scene->Registry().get<sage::ui::Interactable>(e);
            if (act.Runtime.Hovered && !act.Action.empty()) return act.Action;
        }
        return {};
    });
}


// Аксессоры интерфейса на GameObject. Элемент — набор компонентов, поэтому
// «есть ли он» это наличие обязательного Transform, «добавить» — поставить его
// (остальные части появятся при первой же записи в них), а «убрать» — снять все
// части сразу: элемент без Transform не рисуется, но его Fill и Label остались
// бы висеть в сцене невидимым мусором.
void ScriptEngine::BindUIAccessors(sol::usertype<GameObject>& t) {
    t["HasUI"] = [](GameObject& o) {
        return o.Valid() && sage::ui::IsElement(*o.Registry(), o.Entity());
    };
    t["GetUI"] = &UIOf;
    t["AddUI"] = [](GameObject& o) -> UIRef {
        if (!o.Valid()) throw std::runtime_error("AddUI: невалидная сущность");
        o.Registry()->get_or_emplace<sage::ui::Transform>(o.Entity());
        return UIRef{o.Registry(), o.Entity()};
    };
    t["RemoveUI"] = [](GameObject& o) {
        if (!o.Valid()) return;
        namespace ui = sage::ui;
        o.Registry()->remove<ui::Transform, ui::Fill, ui::Label, ui::Image, ui::Bar, ui::Icon,
                             ui::Interactable, ui::TextInput, ui::Range, ui::Mask, ui::Layout,
                             ui::Canvas, ui::Group>(o.Entity());
    };
}
