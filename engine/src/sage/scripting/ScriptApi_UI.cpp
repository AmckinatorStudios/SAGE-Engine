#include "ScriptEngine.h"


#include "sage/core/Log.h"
#include "sage/render/ResourceManager.h"
#include "sage/ui/UIDemos.h"
#include "sage/ui/UIShowcase.h"
#include "sage/ui/UISceneSystem.h"
#include "sage/ui/UIIcons.h"
#include "sage/ui/UI.h"
#include "sage/ui/UIPresets.h"
#include "sage/ui/SpriteSheet.h"
#include "sage/ui/UISerialize.h"
#include "sage/core/Paths.h"
#include "sage/assets/AssetDatabase.h"
#include "sage/assets/Pack.h"

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
// Имена режимов картинки для скриптов — в порядке sage::ui::Image::Mode.
const char* const kFitNames[] = {"stretch", "nineslice", "tile", "fit", "cover"};

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
// Выравнивание надписи по имени: "start" | "center" | "end". Неизвестное имя —
// ошибка скрипта, а не молчаливое «по центру»: опечатка видна сразу.
std::string LabelAlignName(UIRef& r, bool vertical) {
    const sage::ui::Label* l = r.Peek<sage::ui::Label>();
    const sage::ui::Label::Align a = l ? (vertical ? l->Vertical : l->Horizontal) : sage::ui::Label::Align::Center;
    return a == sage::ui::Label::Align::Start ? "start" : a == sage::ui::Label::Align::End ? "end" : "center";
}

void SetLabelAlign(UIRef& r, const std::string& name, bool vertical) {
    if (!r.Alive()) return;
    sage::ui::Label::Align a;
    if (name == "start") a = sage::ui::Label::Align::Start;
    else if (name == "center") a = sage::ui::Label::Align::Center;
    else if (name == "end") a = sage::ui::Label::Align::End;
    else throw std::runtime_error("выравнивание надписи: \"" + name + "\" — нужно start, center или end");
    sage::ui::Label& l = r.Part<sage::ui::Label>();
    (vertical ? l.Vertical : l.Horizontal) = a;
}

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
// (то есть для любой, написанной на Lua целиком), это означало, что
// сетка инвентаря раскладывается вручную по формуле «i-й слот в столбце
// i % columns», меню паузы рисуется поверх худа только если угадать номер слоя,
// а спрятать панель целиком нечем, кроме обхода всех её детей.
//
// Разбор таблицы, а не десяток отдельных функций: раскладка задаётся ОДНИМ
// решением («это сетка по пять в ряд с зазором шесть»), и разносить его по
// пяти вызовам значило бы дать возможность настроить половину.
// Режим раскладки по слову. «row»/«column» — прежние слова ui.group{layout}.
sage::ui::Arrangement::Kind ModeFromName(const std::string& name, bool wrap) {
    using K = sage::ui::Arrangement::Kind;
    if (name == "manual") return K::Manual;
    if (name == "grid") return K::Grid;
    if (name == "wrap" || wrap) return K::Wrap;
    if (name == "row" || name == "horizontal") return K::Horizontal;
    return K::Vertical;
}

sage::ui::Arrangement::Spread SpreadFromName(const std::string& name) {
    using S = sage::ui::Arrangement::Spread;
    if (name == "center") return S::Center;
    if (name == "end") return S::End;
    if (name == "between" || name == "space-between") return S::SpaceBetween;
    if (name == "around" || name == "space-around") return S::SpaceAround;
    if (name == "evenly" || name == "space-evenly") return S::SpaceEvenly;
    return S::Start;
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

namespace {
// Лист спрайтов для скрипта: путь картинки и её описание.
struct SheetRef {
    std::string Path;
    sage::ui::SpriteSheet Sheet;
};
// Описание листа — рядом с НАСТОЯЩИМ файлом картинки (путь проекта ->
// путь на диске): и прочитать, и записать его надо там, где лежит картинка.
std::string SheetSidecar(const std::string& path) {
    return sage::ui::SpriteSheet::SidecarPath(sage::AssetDatabase::Instance().LocatePath(path));
}
sage::assets::ResolvedSprite ImageSprite(const sage::ui::Image& image) {
    auto& resources = ResourceManager::Instance();
    if (image.SpriteRef.Valid()) return resources.ResolveSprite(image.SpriteRef);
    auto resolved = resources.ResolveSpriteImage(image.Path);
    if (!resolved) return {};
    if (!image.Sequence.empty()) {
        const auto* sequence = resolved.Resource->Sheet.FindSequence(image.Sequence);
        if (!sequence) return {};
        resolved.Ref.Item = sequence->Id();
        resolved.Ref.Type = sage::assets::SpriteRef::Kind::Sequence;
    } else if (!image.NamedSprite.empty()) {
        const auto* region = resolved.Resource->Sheet.Find(image.NamedSprite);
        if (!region) return {};
        resolved.Ref.Item = region->Id();
    }
    return resolved;
}

void SetRawSprite(sage::ui::Image& image, glm::vec4 rect) {
    image.SpriteRef = {};
    image.NamedSprite.clear();
    image.Sequence.clear();
    image.Sprite = rect;
}

void OverrideSlice(sage::ui::Image& image) {
    if (image.OverrideSpriteSlice) return;
    if (image.SpriteRef.Valid() || !image.NamedSprite.empty()) {
        const auto resolved = ImageSprite(image);
        if (const auto* frame = resolved.Region(image.SequenceTime)) image.SetSlice(frame->NineSliceOf());
    }
    image.OverrideSpriteSlice = true;
}

std::shared_ptr<const sage::assets::SpriteImage> SheetPixels(const std::string& path) {
    auto source = ResourceManager::Instance().ResolveSpriteImage(path, true);
    if (!source || !source.Resource->Image)
        throw std::runtime_error("sprite_sheet: " + source.Diagnostic);
    return source.Resource->Image;
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
    m_lua.new_enum<sage::ui::Element::Stretch>("UIStretch", {
        {"None", sage::ui::Element::Stretch::None},
        {"Horizontal", sage::ui::Element::Stretch::Horizontal},
        {"Vertical", sage::ui::Element::Stretch::Vertical},
        {"Both", sage::ui::Element::Stretch::Both},
    });

    m_lua.new_usertype<UIRef>("UIElement",
        // Где стоит.
        "Anchor", UI_FIELD(sage::ui::Element, Anchor),
        "Position", UI_FIELD(sage::ui::Element, Position),
        "Size", UI_FIELD(sage::ui::Element, Size),
        "Rotation", UI_FIELD(sage::ui::Element, Rotation),
        // Как элемент ложится на фон: "normal", "add", "multiply", "screen",
        // "invert" (прицел, видный на любом фоне). Опечатка — ошибка.
        "Blend", sol::property(
                     [](UIRef& r) {
                         const sage::ui::Element* el = r.Peek<sage::ui::Element>();
                         return std::string(UIBlendName(el ? el->Blend : UIBlend::Normal));
                     },
                     [](UIRef& r, const std::string& name) {
                         if (!r.Alive()) return;
                         UIBlend b;
                         if (!UIBlendFromName(name, b))
                             throw std::runtime_error("Blend: \"" + name +
                                                      "\" — нужно normal, add, multiply, screen или invert");
                         r.Part<sage::ui::Element>().Blend = b;
                     }),
        "Order", UI_FIELD(sage::ui::Element, Order),
        // Слой отрисовки поверх всего интерфейса (0 — как у родителя).
        "DrawLayer", UI_FIELD(sage::ui::Element, DrawLayer),
        // В контейнере: доля свободного места и «стоять по своему якорю».
        "Grow", UI_FIELD(sage::ui::Element, Grow),
        "IgnoreLayout", UI_FIELD(sage::ui::Element, IgnoreLayout),
        // Адаптивное размещение: пределы, пропорции, безопасная область.
        "MinSize", UI_FIELD(sage::ui::Element, MinSize),
        "MaxSize", UI_FIELD(sage::ui::Element, MaxSize),
        "AspectRatio", UI_FIELD(sage::ui::Element, AspectRatio),
        "SafeArea", UI_FIELD(sage::ui::Element, SafeArea),
        // Оформление: "engine" — движка (только цвета), "custom" — своё.
        "Skin", sol::property(
                    [](UIRef& r) {
                        const sage::ui::Element* el = r.Peek<sage::ui::Element>();
                        return std::string(el && el->Skin == sage::ui::Element::SkinMode::Engine ? "engine"
                                                                                                : "custom");
                    },
                    [](UIRef& r, const std::string& mode) {
                        if (!r.Alive()) return;
                        r.Part<sage::ui::Element>().Skin = mode == "engine"
                                                                ? sage::ui::Element::SkinMode::Engine
                                                                : sage::ui::Element::SkinMode::Custom;
                    }),
        // Файл стиля (.sageuistyle): смена пути кладёт новый стиль на следующем
        // кадре — версия сбрасывается.
        "Style", sol::property(
                     [](UIRef& r) {
                         const sage::ui::Element* el = r.Peek<sage::ui::Element>();
                         return el ? el->Style : std::string();
                     },
                     [](UIRef& r, const std::string& path) {
                         if (!r.Alive()) return;
                         sage::ui::Element& el = r.Part<sage::ui::Element>();
                         el.Style = path;
                         el.StyleVersion = -1;
                     }),
        "Visible", UI_FIELD(sage::ui::Element, Visible),
        "Active", UI_FIELD(sage::ui::Element, Active),
        // Растяжение и поля: «на весь экран с отступом 24» задаётся здесь, а не
        // пересчитывается скриптом при каждом изменении размера окна.
        "Stretch", UI_FIELD(sage::ui::Element, Mode),
        "Margin", UI_FIELD(sage::ui::Element, Margin),
        "Pivot", UI_FIELD(sage::ui::Element, Pivot),
        // Групповые свойства поддерева: спрятать плавно — одно число на корне
        // панели вместо прохода по всем её детям с правкой альфы у каждого.
        "Alpha", UI_FIELD(sage::ui::Group, Alpha),
        "BlockRaycasts", UI_FIELD(sage::ui::Group, BlockRaycasts),
        // Фактический размер после раскладки: только для чтения — его считает
        // раскладка, а не задаёт скрипт.
        "ResolvedSize", sol::readonly_property([](UIRef& r) {
            const sage::ui::Element* t = r.Peek<sage::ui::Element>();
            return t ? t->Resolved : glm::vec2(0.0f);
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
        // Выравнивание надписи строкой: "start" | "center" | "end" (и для
        // вертикали — TextVAlign). Флаг TextCentered знал только два из трёх:
        // число в правом нижнем углу ячейки (стопка, счёт) скрипт поставить не
        // мог — подгонял шириной рамки и промахивался на любом другом шрифте.
        "TextAlign", sol::property([](UIRef& r) { return LabelAlignName(r, false); },
                                   [](UIRef& r, const std::string& a) { SetLabelAlign(r, a, false); }),
        "TextVAlign", sol::property([](UIRef& r) { return LabelAlignName(r, true); },
                                    [](UIRef& r, const std::string& a) { SetLabelAlign(r, a, true); }),
        // Шрифт и начертание надписи — из скрипта тоже: диалоги, титры и
        // всплывающие подсказки собирает код, и «этот заголовок жирным» там
        // такая же обычная просьба, как в редакторе.
        "Font", UI_FIELD(sage::ui::Label, Font),
        "FontPixelHeight", UI_FIELD(sage::ui::Label, FontPixelHeight),
        // Фильтрация строкой: "smooth" или "nearest" (см. TextureFiltering).
        "FontSnapPixels", UI_FIELD(sage::ui::Label, FontSnapPixels),
        "FontFilter", sol::property(
                          [](UIRef& r) {
                              const sage::ui::Label* l = r.Peek<sage::ui::Label>();
                              return std::string(l && l->Sharp() ? "nearest" : "smooth");
                          },
                          [](UIRef& r, const std::string& mode) {
                              if (!r.Alive()) return;
                              r.Part<sage::ui::Label>().FontFiltering =
                                  mode == "nearest" ? sage::ui::TextureFiltering::Nearest
                                                    : sage::ui::TextureFiltering::Smooth;
                          }),
        "Bold", sol::property([](UIRef& r) {
                                  const sage::ui::Label* l = r.Peek<sage::ui::Label>();
                                  return l && (l->Face == sage::ui::Label::Style::Bold ||
                                               l->Face == sage::ui::Label::Style::BoldItalic);
                              },
                              [](UIRef& r, bool bold) {
                                  if (!r.Alive()) return;
                                  sage::ui::Label& l = r.Part<sage::ui::Label>();
                                  const bool italic = l.Face == sage::ui::Label::Style::Italic ||
                                                      l.Face == sage::ui::Label::Style::BoldItalic;
                                  l.Face = bold ? (italic ? sage::ui::Label::Style::BoldItalic
                                                          : sage::ui::Label::Style::Bold)
                                                : (italic ? sage::ui::Label::Style::Italic
                                                          : sage::ui::Label::Style::Regular);
                              }),
        "Italic", sol::property([](UIRef& r) {
                                    const sage::ui::Label* l = r.Peek<sage::ui::Label>();
                                    return l && (l->Face == sage::ui::Label::Style::Italic ||
                                                 l->Face == sage::ui::Label::Style::BoldItalic);
                                },
                                [](UIRef& r, bool italic) {
                                    if (!r.Alive()) return;
                                    sage::ui::Label& l = r.Part<sage::ui::Label>();
                                    const bool bold = l.Face == sage::ui::Label::Style::Bold ||
                                                      l.Face == sage::ui::Label::Style::BoldItalic;
                                    l.Face = italic ? (bold ? sage::ui::Label::Style::BoldItalic
                                                            : sage::ui::Label::Style::Italic)
                                                    : (bold ? sage::ui::Label::Style::Bold
                                                            : sage::ui::Label::Style::Regular);
                                }),
        "WrapText", UI_FIELD(sage::ui::Label, Wrap),
        "AutoWidth", UI_FIELD(sage::ui::Label, AutoWidth),
        "PadX", UI_FIELD(sage::ui::Label, PadX),
        // Тень и обводка текста (см. sage::ui::Label).
        "TextShadowOffset", UI_FIELD(sage::ui::Label, ShadowOffset),
        "TextShadowColor", UI_FIELD(sage::ui::Label, ShadowColor),
        "TextOutline", UI_FIELD(sage::ui::Label, OutlineWidth),
        "TextOutlineColor", UI_FIELD(sage::ui::Label, OutlineColor),
        // Картинка.
        "TexturePath", sol::property([](UIRef& r) {
            const auto* image = r.Peek<sage::ui::Image>();
            return image ? image->Path : std::string{};
        }, [](UIRef& r, const std::string& path) {
            if (!r.Alive()) return;
            auto& image = r.Part<sage::ui::Image>();
            if (image.Path != path) {
                image.SpriteRef = {};
                image.NamedSprite.clear();
            }
            image.Path = path;
        }),
        "Sprite", sol::property([](UIRef& r) {
            const auto* image = r.Peek<sage::ui::Image>();
            if (!image) return glm::vec4(0);
            if (image->SpriteRef.Valid() || !image->NamedSprite.empty() || !image->Sequence.empty()) {
                const auto resolved = ImageSprite(*image);
                if (const auto* frame = resolved.Region(image->SequenceTime)) return glm::vec4(frame->Rect);
            }
            return image->Sprite;
        }, [](UIRef& r, glm::vec4 rect) {
            if (r.Alive()) SetRawSprite(r.Part<sage::ui::Image>(), rect);
        }),
        "SpriteHover", sol::property([](UIRef& r) {
            const auto* image = r.Peek<sage::ui::Image>();
            return image ? image->SpriteHover : glm::vec4(0);
        }, [](UIRef& r, glm::vec4 rect) {
            if (!r.Alive()) return;
            auto& image = r.Part<sage::ui::Image>();
            image.HoverSpriteRef = {};
            image.SpriteHover = rect;
        }),
        "SpritePressed", sol::property([](UIRef& r) {
            const auto* image = r.Peek<sage::ui::Image>();
            return image ? image->SpritePressed : glm::vec4(0);
        }, [](UIRef& r, glm::vec4 rect) {
            if (!r.Alive()) return;
            auto& image = r.Part<sage::ui::Image>();
            image.PressedSpriteRef = {};
            image.SpritePressed = rect;
        }),
        "SliceBorder", sol::property([](UIRef& r) {
            const auto* image = r.Peek<sage::ui::Image>();
            if (!image) return glm::vec4(0);
            if (!image->OverrideSpriteSlice && (image->SpriteRef.Valid() || !image->NamedSprite.empty())) {
                const auto resolved = ImageSprite(*image);
                if (const auto* frame = resolved.Region(image->SequenceTime)) return frame->Slice;
            }
            return image->SliceBorder;
        }, [](UIRef& r, glm::vec4 border) {
            if (!r.Alive()) return;
            auto& image = r.Part<sage::ui::Image>();
            OverrideSlice(image);
            image.SliceBorder = border;
        }),
        "PixelScale", UI_FIELD(sage::ui::Image, PixelScale),
        // Замощение: копий по ширине и высоте (vec2; 0 по оси — своим размером).
        "Repeat", UI_FIELD(sage::ui::Image, Repeat),
        "SnapPixels", UI_FIELD(sage::ui::Image, SnapPixels),
        "Filter", sol::property(
                      [](UIRef& r) {
                          const sage::ui::Image* im = r.Peek<sage::ui::Image>();
                          return std::string(im && im->Sharp() ? "nearest" : "smooth");
                      },
                      [](UIRef& r, const std::string& mode) {
                          if (!r.Alive()) return;
                          r.Part<sage::ui::Image>().Filtering =
                              mode == "nearest" ? sage::ui::TextureFiltering::Nearest
                                                : sage::ui::TextureFiltering::Smooth;
                      }),
        // Как картинка ложится в элемент: "stretch" (растянуть), "nineslice"
        // (углы неподвижны), "tile" (повторить куском своего размера ×
        // PixelScale — фон из плитки), "fit" (целиком), "cover" (заполнить).
        "Fit", sol::property(
                   [](UIRef& r) {
                       const sage::ui::Image* im = r.Peek<sage::ui::Image>();
                       if (!im) return std::string(kFitNames[0]);
                       if (im->UseSpriteFit && im->Fit == sage::ui::Image::Mode::Normal &&
                           (im->SpriteRef.Valid() || !im->NamedSprite.empty())) {
                           const auto resolved = ImageSprite(*im);
                           if (const auto* frame = resolved.Region(im->SequenceTime)) {
                               const auto border = im->OverrideSpriteSlice ? im->SliceBorder : frame->Slice;
                               if (border != glm::vec4(0)) return std::string(kFitNames[1]);
                           }
                       }
                       return std::string(kFitNames[int(im->Fit)]);
                   },
                   [](UIRef& r, const std::string& name) {
                       for (int i = 0; i < 5; ++i) {
                           if (name == kFitNames[i]) {
                               if (r.Alive()) {
                                   auto& image = r.Part<sage::ui::Image>();
                                   image.Fit = (sage::ui::Image::Mode)i;
                                   image.UseSpriteFit = false;
                               }
                               return;
                           }
                       }
                       throw std::runtime_error("ui.Fit: «" + name + "» — нужно stretch, nineslice, tile, fit или cover");
                   }),
        // Полоса и значок.
        // Цвет заполнения — цвет его вида (sage::ui::Bar::Filled); имя свойства
        // прежнее, скрипты игр им пользуются.
        "BarFillColor", sol::property(
                            [](UIRef& r) {
                                const sage::ui::Bar* b = r.Peek<sage::ui::Bar>();
                                return b ? b->Filled.Color : sage::ui::Bar{}.Filled.Color;
                            },
                            [](UIRef& r, const glm::vec4& c) {
                                if (!r.Alive()) return;
                                r.Part<sage::ui::Bar>().SetFillColor(c);
                            }),
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
        using A = sage::ui::Arrangement;
        obj.Registry()->get_or_emplace<sage::ui::Element>(obj.Entity());
        A& arr = obj.Registry()->get_or_emplace<A>(obj.Entity());
        const std::string dir = opts.get_or<std::string>("dir", "column");
        const bool wrap = opts.get_or("wrap", false);
        arr.Mode = ModeFromName(dir, wrap && dir != "grid");
        arr.WrapVertical = arr.Mode == A::Kind::Wrap && dir == "column";
        arr.Distribution = SpreadFromName(opts.get_or<std::string>("justify", "start"));
        arr.Spacing = opts.get_or("spacing", arr.Spacing);
        arr.Padding = PaddingFrom(opts.get<sol::object>("padding"), arr.Padding);
        arr.Columns = std::max(0, opts.get_or("columns", arr.Columns));
        arr.Rows = std::max(0, opts.get_or("rows", arr.Rows));
        // Поперёк: start / center / end — выравнивание; stretch — дети во всё
        // место поперёк оси (размер ребёнка Fill).
        const std::string c = opts.get_or<std::string>("cross", "stretch");
        arr.Alignment = c == "center" ? A::Align::Center : c == "end" ? A::Align::End : A::Align::Start;
        const A::ChildSize across = c == "stretch" ? A::ChildSize::Fill : A::ChildSize::Fixed;
        if (arr.Horizontal()) arr.ChildHeight = across;
        else if (arr.Mode != A::Kind::Grid) arr.ChildWidth = across;
        arr.FitContent = opts.get_or("fit", arr.FitContent);
    });
    Bind("ui", "ClearLayout", "ClearUILayout", [](GameObject& obj) {
        if (!obj.Valid()) return;
        if (auto* arr = obj.Registry()->try_get<sage::ui::Arrangement>(obj.Entity()))
            arr->Mode = sage::ui::Arrangement::Kind::Manual;
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
        obj.Registry()->get_or_emplace<sage::ui::Element>(obj.Entity());
        sage::ui::Canvas& canvas = obj.Registry()->get_or_emplace<sage::ui::Canvas>(obj.Entity());
        canvas.SortOrder = opts.get_or("order", canvas.SortOrder);
        if (opts["scale"].valid()) {
            canvas.Mode = opts.get_or("scale", false) ? sage::ui::Canvas::Scale::ScaleWithSize
                                                      : sage::ui::Canvas::Scale::Pixels;
        }
        // Целый масштаб для пиксельного интерфейса: {pixelScale = true, maxScale = 3}.
        if (opts.get_or("pixelScale", false)) canvas.Mode = sage::ui::Canvas::Scale::IntegerFit;
        canvas.MaxScale = opts.get_or("maxScale", canvas.MaxScale);
        if (opts["reference"].valid()) canvas.Reference = opts.get<glm::vec2>("reference");
        canvas.MatchWidthOrHeight = opts.get_or("match", canvas.MatchWidthOrHeight);
    });

    // Картинка UI-элемента: путь + резкая ли фильтрация. Отдельной функцией, а
    // не полем TexturePath, потому что путь без ЗАГРУЗКИ ничего не рисует, а
    // загрузить надо с правильной фильтрацией — из скрипта об этом помнить
    // незачем.
    Bind("ui", "SetImage", "SetUIImage", [](GameObject& obj, const std::string& path,
                                        sol::optional<bool> sharp) {
        if (!obj.Valid() || !sage::ui::IsElement(*obj.Registry(), obj.Entity())) return;
        sage::ui::Image& im = obj.Registry()->get_or_emplace<sage::ui::Image>(obj.Entity());
        if (im.Path != path || path.empty()) {
            im.SpriteRef = {};
            im.NamedSprite.clear();
        }
        im.Path = path;
        im.Filtering = sharp.value_or(false) ? sage::ui::TextureFiltering::Nearest
                                             : sage::ui::TextureFiltering::Smooth;
        sage::ui::EnsureImageTexture(im);
    });

    // Убрать картинку с элемента СОВСЕМ. Не то же, что пустой путь: пока
    // компонент на элементе, он участвует в отрисовке, и переключить слот
    // обратно на плоский значок, не сняв его, нельзя.
    Bind("ui", "ClearImage", "ClearUIImage", [](GameObject& obj) {
        if (!obj.Valid()) return;
        obj.Registry()->remove<sage::ui::Image>(obj.Entity());
    });

    // Спрайт из листа: кусок в ПИКСЕЛЯХ исходника. Так набор интерфейса
    // описывается ровно теми числами, что напечатаны в его документации.
    Bind("ui", "SetSprite", "SetUISprite", [](GameObject& obj, float x, float y, float w, float h) {
        if (!obj.Valid() || !sage::ui::IsElement(*obj.Registry(), obj.Entity())) return;
        SetRawSprite(obj.Registry()->get_or_emplace<sage::ui::Image>(obj.Entity()), glm::vec4(x, y, w, h));
    });

    // Девятина: неподвижные углы в пикселях исходника (лево, верх, право, низ).
    Bind("ui", "SetSlice", "SetUISlice", [](GameObject& obj, float l, float t, float r, float b,
                                        sol::optional<float> scale) {
        if (!obj.Valid() || !sage::ui::IsElement(*obj.Registry(), obj.Entity())) return;
        sage::ui::Image& im = obj.Registry()->get_or_emplace<sage::ui::Image>(obj.Entity());
        OverrideSlice(im);
        im.SliceBorder = glm::vec4(l, t, r, b);
        im.PixelScale = scale.value_or(0.0f);
    });

    // Нарезка ИЗ ФАЙЛА, лежащего рядом с картинкой (см. sage/ui/NineSlice.h).
    // Ради этого описание и вынесено в файл: одна рамка на десяти элементах
    // описана один раз, и скрипту не приходится повторять её четыре числа —
    // вместе с режимами заполнения и полой серединой, которых у SetUISlice нет.
    // false — файла рядом нет или он не разобрался (картинка при этом рисуется
    // целиком, а не пропадает).
    Bind("ui", "LoadSlice", "LoadUISlice", [](GameObject& obj) {
        if (!obj.Valid() || !sage::ui::IsElement(*obj.Registry(), obj.Entity())) return false;
        sage::ui::Image* im = obj.Registry()->try_get<sage::ui::Image>(obj.Entity());
        if (!im || im->Path.empty()) return false;
        using sage::assets::vfs::Exists;
        const auto path = sage::AssetDatabase::Instance().LocatePath(im->Path);
        if (!Exists(sage::ui::SpriteSheet::SidecarPath(path)) && !Exists(sage::ui::NineSlice::SidecarPath(path)))
            return false;
        const auto source = ResourceManager::Instance().ResolveSpriteImage(im->Path, true);
        if (!source) return false;
        im->SetSlice(source.Resource->Sheet.TextureSlice);
        return true;
    });

    // --- Лист спрайтов (sage/ui/SpriteSheet.h) ---------------------------------
    //
    // sheet = sprite_sheet("assets/ui/gui.png") — описание из gui.png.sagesheet
    // (или пустое). Методы через точку: names(), rect(name), grid{...},
    // auto{...}, set(name, x, y, w, h, slice?), rename, remove, save().
    // SetUISpriteNamed(obj, path, name) — картинка, кусок и его девятина разом.
    m_lua.new_usertype<SheetRef>(
        "SpriteSheetRef", sol::no_constructor,
        "Names", [](const SheetRef& r, sol::this_state ts) {
            sol::state_view L(ts);
            sol::table out = L.create_table();
            int i = 1;
            for (const auto& reg : r.Sheet.Regions()) out[i++] = reg.Name;
            return out;
        },
        "Count", [](const SheetRef& r) { return (int)r.Sheet.Regions().size(); },
        "Rect", [](const SheetRef& r, const std::string& name, sol::this_state ts) {
            sol::state_view L(ts);
            const sage::ui::SpriteRegion* reg = r.Sheet.Find(name);
            if (!reg) return sol::make_object(L, sol::lua_nil);
            sol::table out = L.create_table();
            out["x"] = reg->Rect.x; out["y"] = reg->Rect.y; out["w"] = reg->Rect.z; out["h"] = reg->Rect.w;
            if (reg->Slice != glm::vec4(0.0f)) {
                sol::table sl = L.create_table();
                for (int k = 0; k < 4; ++k) sl[k + 1] = reg->Slice[k];
                out["slice"] = sl;
            }
            return sol::make_object(L, out);
        },
        "Set", [](SheetRef& r, const std::string& name, int x, int y, int w, int h, sol::optional<sol::table> slice) {
            sage::ui::SpriteRegion reg{name, {x, y, w, h}, glm::vec4(0.0f)};
            if (slice)
                for (int k = 0; k < 4; ++k) reg.Slice[k] = slice->get_or(k + 1, 0.0f);
            r.Sheet.Set(reg);
        },
        "Rename", [](SheetRef& r, const std::string& a, const std::string& b) { return r.Sheet.Rename(a, b); },
        "Remove", [](SheetRef& r, const std::string& a) { return r.Sheet.Remove(a); },
        "Grid", [](SheetRef& r, sol::table o) {
            const auto px = SheetPixels(r.Path);
            sage::ui::SpriteGrid g;
            g.CellW = o.get_or("w", g.CellW);
            g.CellH = o.get_or("h", g.CellW);
            g.PadX = o.get_or("pad_x", o.get_or("pad", 0));
            g.PadY = o.get_or("pad_y", o.get_or("pad", 0));
            g.OffX = o.get_or("x", 0);
            g.OffY = o.get_or("y", 0);
            g.Prefix = o.get_or("prefix", g.Prefix);
            g.SkipEmpty = o.get_or("skip_empty", true);
            return r.Sheet.SliceGrid(px->Width, px->Height, g, px->Pixels.data(), false);
        },
        "Auto", [](SheetRef& r, sol::optional<sol::table> o) {
            const auto px = SheetPixels(r.Path);
            const float cut = o ? o->get_or("cutoff", 0.05f) : 0.05f;
            const int minSize = o ? o->get_or("min", 2) : 2;
            const std::string prefix = o ? o->get_or("prefix", std::string("sprite")) : std::string("sprite");
            return r.Sheet.SliceAuto(px->Pixels.data(), px->Width, px->Height, false, cut, minSize, prefix);
        },
        "Save", [](const SheetRef& r) {
            std::string e;
            if (!r.Sheet.SaveFile(SheetSidecar(r.Path), e))
                throw std::runtime_error("sprite_sheet.save: " + e);
            auto& database = sage::AssetDatabase::Instance();
            const auto sidecar = sage::ui::SpriteSheet::SidecarPath(r.Path);
            if (!database.ProjectDir().empty() && !sage::PathFromUtf8(sidecar).is_absolute()) {
                const auto guid = database.Register(sidecar);
                if (guid.Valid()) ResourceManager::Instance().NotifySpriteChanged(guid);
            }
            return true;
        });
    Bind("ui", "Sheet", nullptr, [](const std::string& path) {
        SheetRef r;
        r.Path = path;
        const auto source = ResourceManager::Instance().ResolveSpriteImage(path, true);
        if (source) r.Sheet = source.Resource->Sheet;
        else throw std::runtime_error("sprite_sheet: " + source.Diagnostic);
        return r;
    });
    // Стиль SAGE — методы через точку: тонкая обёртка над объектом C++.
    m_lua.script(R"LUA(
function sprite_sheet(path)
    local r = sage.ui.Sheet(path)
    return {
        path = path,
        names = function() return r:Names() end,
        count = function() return r:Count() end,
        rect = function(name) return r:Rect(name) end,
        set = function(name, x, y, w, h, slice) r:Set(name, x, y, w, h, slice) end,
        rename = function(a, b) return r:Rename(a, b) end,
        remove = function(a) return r:Remove(a) end,
        grid = function(o) return r:Grid(o) end,
        auto = function(o) return r:Auto(o) end,
        save = function() return r:Save() end,
    }
end
)LUA");
    // Кусок листа по имени — на элемент: картинка, прямоугольник и девятина
    // (если она у куска есть, картинка ложится девятиной). false — нет куска.
    Bind("ui", "SetSpriteNamed", "SetUISpriteNamed", [](GameObject& obj, const std::string& path,
                                                       const std::string& name, sol::optional<bool> sharp) {
        if (!obj.Valid() || !sage::ui::IsElement(*obj.Registry(), obj.Entity())) return false;
        const auto sheet = ResourceManager::Instance().ResolveSpriteImage(path);
        const auto* region = sheet ? sheet.Resource->Sheet.Find(name) : nullptr;
        const auto* sequence = sheet && !region ? sheet.Resource->Sheet.FindSequence(name) : nullptr;
        if (!region && !sequence) return false;
        auto& im = obj.Registry()->get_or_emplace<sage::ui::Image>(obj.Entity());
        if (im.Path != path || sharp)
            im.Filtering = sharp.value_or(true) ? sage::ui::TextureFiltering::Nearest : sage::ui::TextureFiltering::Smooth;
        const sage::assets::SpriteRef ref{sheet.Ref.Asset, region ? region->Id() : sequence->Id(),
            region ? sage::assets::SpriteRef::Kind::Region : sage::assets::SpriteRef::Kind::Sequence};
        if (im.SpriteRef.Item != ref.Item || im.SpriteRef.Asset != ref.Asset || im.Path != path)
            im.SequenceTime = 0.0f;
        im.Path = path;
        im.SpriteRef = ref;
        im.NamedSprite = !ref.Valid() && region ? name : std::string{};
        im.Sequence = !ref.Valid() && sequence ? name : std::string{};
        im.Sprite = glm::vec4(0);
        im.OverrideSpriteSlice = false;
        im.UseSpriteFit = true;
        if (im.Fit == sage::ui::Image::Mode::NineSlice) im.Fit = sage::ui::Image::Mode::Normal;
        im.Tex.reset();
        return true;
    });

    // Повторять тянущиеся куски вместо растягивания. Для рисунка по пикселям и узоров
    // это единственный правильный ответ: орнамент из 16 пикселей, растянутый на
    // 300, превращается в мыло.
    Bind("ui", "SetSliceTiled", "SetUISliceTiled", [](GameObject& obj, bool tiled) {
        if (!obj.Valid() || !sage::ui::IsElement(*obj.Registry(), obj.Entity())) return;
        sage::ui::Image& im = obj.Registry()->get_or_emplace<sage::ui::Image>(obj.Entity());
        OverrideSlice(im);
        const auto fill = tiled ? sage::ui::SliceFill::Tile : sage::ui::SliceFill::Stretch;
        im.SliceCenterFill = fill;
        im.SliceEdgeFill = fill;
    });

    // Рисовать ли середину. У рамки-обводки её нет: сквозь неё видно то, что
    // под элементом.
    Bind("ui", "SetSliceDrawsCenter", "SetUISliceDrawsCenter", [](GameObject& obj, bool draw) {
        if (!obj.Valid() || !sage::ui::IsElement(*obj.Registry(), obj.Entity())) return;
        obj.Registry()->get_or_emplace<sage::ui::Image>(obj.Entity()).SliceDrawCenter = draw;
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

    // Курсор в координатах интерфейса и то, что мышь сделала за кадр.
    //
    // Ради перетаскивания предметов: пока игра знала только «щёлкнули по
    // такому-то действию», перенести вещь из ячейки в ячейку было нечем —
    // щелчок это нажатие и отпускание на ОДНОМ элементе, а перенос на разных.
    // Плюс сам предмет надо нарисовать под курсором, а точки курсора в
    // координатах кадра у скрипта не было вовсе: у окна свои координаты, у
    // letterbox-кадра свои, у холста свой масштаб.
    Bind("ui", "Cursor", "UICursor", [this]() -> glm::vec2 {
        return m_scene ? m_scene->UiFrame.Cursor : glm::vec2(-1.0f);
    });
    // Размер кадра в координатах интерфейса. Нужен всему, что игра ставит
    // ПОД КУРСОР: подсказке и перетаскиваемой вещи надо отпрыгивать от краёв,
    // а размера кадра у скрипта нет — окно, letterbox и масштаб холста дают
    // три разных числа.
    Bind("ui", "ScreenSize", "UIScreenSize", [this]() -> glm::vec2 {
        return m_scene ? m_scene->UiFrame.Size : glm::vec2(0.0f);
    });
    Bind("ui", "MouseDown", "UIMouseDown", [this]() -> bool {
        return m_scene && m_scene->UiFrame.MouseDown;
    });
    Bind("ui", "PressedAction", "UIPressedAction", [this]() -> std::string {
        return m_scene ? m_scene->UiFrame.PressedAction : std::string();
    });
    Bind("ui", "ReleasedAction", "UIReleasedAction", [this]() -> std::string {
        return m_scene ? m_scene->UiFrame.ReleasedAction : std::string();
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
        o.Registry()->get_or_emplace<sage::ui::Element>(o.Entity());
        return UIRef{o.Registry(), o.Entity()};
    };
    t["RemoveUI"] = [](GameObject& o) {
        if (!o.Valid()) return;
        namespace ui = sage::ui;
        o.Registry()->remove<ui::Element, ui::Fill, ui::Label, ui::Image, ui::Bar, ui::Icon,
                             ui::Interactable, ui::TextInput, ui::Range, ui::Mask, ui::Arrangement,
                             ui::Canvas, ui::Group>(o.Entity());
    };
}
