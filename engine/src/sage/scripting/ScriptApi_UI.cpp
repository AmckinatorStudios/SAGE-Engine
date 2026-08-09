#include "ScriptEngine.h"

#include "sage/core/Log.h"
#include "sage/render/ResourceManager.h"
#include "sage/ui/UIDemos.h"
#include "sage/ui/UIShowcase.h"
#include "sage/ui/UISceneSystem.h"
#include "sage/ui/UIIcons.h"
#include "sage/ui/UI.h"
#include "sage/ui/UIPresets.h"
#include "sage/ui/UIBridge.h"

#include <cmath>

// ---------------------------------------------------------------------------
// Интерфейс сцены: UIElementComponent и sage.ui.*
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

// Вид элемента: у набора компонентов его нет, он ВЫВОДИТСЯ (см. UIBridge).
// Присваивание понимается как «сделай из этого вот такой элемент»: ставится
// определяющая часть и снимаются чужие определяющие.
void SetKind(UIRef& r, UIElementComponent::Kind kind) {
    using Kind = UIElementComponent::Kind;
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

} // namespace

void ScriptEngine::RegisterUIApi() {
    // --- UI: интерфейс сцены (набор компонентов, см. sage/ui/UI.h).
    // Скрипты правят текст/значение/видимость через ссылку на элемент — так худ
    // (здоровье, счёт, таймеры) обновляется из игровой логики. ---
    m_lua.new_enum<UIElementComponent::Kind>("UIKind", {
        {"Panel", UIElementComponent::Kind::Panel},
        {"Icon",  UIElementComponent::Kind::Icon},
        {"Input", UIElementComponent::Kind::Input},
        {"Checkbox", UIElementComponent::Kind::Checkbox},
        {"Slider", UIElementComponent::Kind::Slider},
        {"Label", UIElementComponent::Kind::Label},
        {"Image", UIElementComponent::Kind::Image},
        {"Bar",   UIElementComponent::Kind::Bar},
    });
    m_lua.new_enum<UIAnchor>("UIAnchor", {
        {"TopLeft", UIAnchor::TopLeft},       {"TopCenter", UIAnchor::TopCenter},
        {"TopRight", UIAnchor::TopRight},     {"CenterLeft", UIAnchor::CenterLeft},
        {"Center", UIAnchor::Center},         {"CenterRight", UIAnchor::CenterRight},
        {"BottomLeft", UIAnchor::BottomLeft}, {"BottomCenter", UIAnchor::BottomCenter},
        {"BottomRight", UIAnchor::BottomRight},
    });

    m_lua.new_usertype<UIRef>("UIElement",
        // Где стоит.
        "Anchor", UI_FIELD(sage::ui::Transform, Anchor),
        "Offset", UI_FIELD(sage::ui::Transform, Offset),
        "Size", UI_FIELD(sage::ui::Transform, Size),
        "Layer", UI_FIELD(sage::ui::Transform, Layer),
        "Visible", UI_FIELD(sage::ui::Transform, Visible),
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
        "Type", sol::property([](UIRef& r) { return sage::ui::Compose(*r.Reg, r.E).Type; },
                              &SetKind),
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

    // Собирает боевой интерфейс (инвентарь + дерево навыков) из UIElementComponent
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
