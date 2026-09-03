#include "UISceneSystem.h"
#include "sage/ui/UIPart.h"

#include "sage/ui/UI.h"
#include "sage/ui/UILegacy.h"
#include "sage/core/Profiler.h"
#include "UIRenderer.h"
#include "UIIcons.h"
#include "sage/scene/Scene.h"
#include "sage/scene/Components.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace sage::ui {

bool IsElement(const entt::registry& reg, entt::entity e) {
    return reg.valid(e) && reg.all_of<Transform>(e);
}

namespace {

// Ширина элемента по СОДЕРЖИМОМУ: отступ, значок, отступ, текст, отступ.
// Высота не подгоняется — её задаёт вёрстка (строка интерфейса одна на всех).
//
// Живёт здесь, а не в решателе раскладки (UI.cpp), потому что единственный, кто
// знает ширину строки, — шрифт, а он есть только у отрисовки.
glm::vec2 MeasuredWidth(const entt::registry& reg, entt::entity e, glm::vec2 size,
                        UIRenderer& ui) {
    const Label* label = reg.try_get<Label>(e);
    if (!label || !label->AutoWidth) return size;
    const Icon* icon = reg.try_get<Icon>(e);
    const bool hasIcon = icon && !icon->Name.empty() && icon->Color.a > 0.0f;
    // Значок занимает квадрат в высоту элемента — текст начинается за ним
    // (ровно там же, где его кладёт DrawElement).
    float w = hasIcon ? size.y : label->PadX;
    if (!label->Text.empty() && label->Color.a > 0.0f) w += ui.MeasureText(label->Text, label->Scale);
    return {glm::max(w + label->PadX, size.y), size.y};
}

// Дети сущности с интерфейсной частью, отсортированные по Layer (стабильно;
// при равенстве — по Id, чтобы порядок был детерминирован).
std::vector<entt::entity> SortedUIChildren(Scene& scene, entt::entity parent) {
    std::vector<entt::entity> kids;
    entt::registry& reg = scene.Registry();
    if (const auto* h = reg.try_get<HierarchyComponent>(parent)) {
        for (auto c : h->Children)
            if (IsElement(reg, c)) kids.push_back(c);
    }
    std::stable_sort(kids.begin(), kids.end(), [&reg](entt::entity a, entt::entity b) {
        const Transform& ta = reg.get<Transform>(a);
        const Transform& tb = reg.get<Transform>(b);
        if (ta.Layer != tb.Layer) return ta.Layer < tb.Layer;
        return reg.get<IdComponent>(a).Id < reg.get<IdComponent>(b).Id;
    });
    return kids;
}

// Корневые UI-сущности: без родителя ЛИБО родитель не UI-элемент (3D-сущность
// может «держать» интерфейс — он всё равно якорится к экрану).
//
// Порядок между корнями задаёт СЛОЙ ХОЛСТА (Canvas::SortOrder), а не только
// Layer: HUD должен быть под меню паузы, меню — под диалогом, и раскладывать
// это одним числом на элемент значило подбирать номера так, чтобы случайно не
// перекрыть чужую панель.
std::vector<entt::entity> SortedUIRoots(Scene& scene) {
    std::vector<entt::entity> roots;
    entt::registry& reg = scene.Registry();
    for (auto e : reg.view<Transform>()) {
        entt::entity parent = scene.ParentOf(e);
        if (parent == entt::null || !IsElement(reg, parent)) roots.push_back(e);
    }
    auto canvasOrder = [&reg](entt::entity e) {
        const Canvas* c = reg.try_get<Canvas>(e);
        return c ? c->SortOrder : 0;
    };
    std::stable_sort(roots.begin(), roots.end(), [&](entt::entity a, entt::entity b) {
        const int ca = canvasOrder(a), cb = canvasOrder(b);
        if (ca != cb) return ca < cb;
        const Transform& ta = reg.get<Transform>(a);
        const Transform& tb = reg.get<Transform>(b);
        if (ta.Layer != tb.Layer) return ta.Layer < tb.Layer;
        return reg.get<IdComponent>(a).Id < reg.get<IdComponent>(b).Id;
    });
    return roots;
}

// Шаг влево/вправо по строке UTF-8. Курсор живёт в БАЙТАХ (строка — байты), но
// двигаться обязан по СИМВОЛАМ: шаг в один байт разрежет кириллическую букву
// пополам, и в поле окажется невалидный UTF-8.
int PrevCharBoundary(const std::string& s, int i) {
    if (i <= 0) return 0;
    --i;
    while (i > 0 && (static_cast<unsigned char>(s[(size_t)i]) & 0xC0) == 0x80) --i;
    return i;
}
int NextCharBoundary(const std::string& s, int i) {
    const int n = (int)s.size();
    if (i >= n) return n;
    ++i;
    while (i < n && (static_cast<unsigned char>(s[(size_t)i]) & 0xC0) == 0x80) ++i;
    return i;
}

// Что показывать вместо содержимого поля-пароля. Точки, а не звёздочки: в
// пиксельных шрифтах звёздочка часто выше строки и ломает базовую линию.
std::string MaskText(const std::string& text) {
    std::string out;
    out.reserve((size_t)Utf8Length(text) * 2);
    for (int i = 0; i < Utf8Length(text); ++i) out += "\u2022";
    return out;
}


// ОТРИСОВКА ЭЛЕМЕНТА — ПО РЕЕСТРУ ЧАСТЕЙ, а не по списку в этой функции.
//
// Здесь была функция на триста строк, которая знала все части наперечёт и
// заодно решала, как они влияют друг на друга: значок сдвигал текст, галка
// сдвигала текст, картинка отменяла подложку, рамка рисовалась после шкалы, но
// до значка. Каждая новая часть означала правку этого клубка — то есть
// набор частей был ЗАШИТ В ДВИЖОК.
//
// Теперь движок не знает ни одной части. Он перебирает реестр (UIPart.h) в
// порядке Order и даёт каждой нарисовать себя в прямоугольнике элемента; вторым
// проходом идут слои «поверх всего» (рамка подложки). Своя часть — файл рядом с
// UIParts.cpp, ни строки правок здесь.
//
// scale — множитель холста (см. Canvas): раскладка считается в ОПОРНЫХ
// единицах, а рисуется в экранных. Прямоугольник приходит уже переведённым, а
// всё, что задано числом рядом с ним (скругление, рамка, кегль), переводит
// сама часть — иначе на 4K панель станет вдвое больше, а её рамка останется.
void DrawElement(const entt::registry& reg, entt::entity e, const UIRect& r, float scale,
                 float alpha, UIRenderer& ui) {
    const Interactable* act = reg.try_get<Interactable>(e);

    PartDrawContext c;
    c.Reg = &reg;
    c.Entity = e;
    c.Rect = r;
    c.Scale = scale;
    c.Alpha = alpha;
    c.Ui = &ui;
    // Состояние — у ЭЛЕМЕНТА, а не у части: нажали не «подложку», а элемент, и
    // потемнеть должны все его слои разом.
    c.Hovered = act && act->Runtime.Hovered;
    c.Pressed = act && act->Runtime.Pressed;
    c.Focused = act && act->Runtime.Focused;
    c.Enabled = !act || act->Enabled;

    for (const PartType& p : Parts()) {
        if (!p.Draw || !p.Has || !p.Has(reg, e)) continue;
        c.Data = p.Get(reg, e);
        p.Draw(c);
    }
    for (const PartType& p : Parts()) {
        if (!p.DrawOver || !p.Has || !p.Has(reg, e)) continue;
        c.Data = p.Get(reg, e);
        p.DrawOver(c);
    }
}

// --- Один решатель на три задачи ------------------------------------------
//
// Раньше сцену обходили ТРИЖДЫ и каждый раз заново считали прямоугольники: для
// отрисовки, для попадания курсором и для ввода. Обходы жили в разных функциях
// и уже расходились — отрисовка учитывала измеренную ширину текста, а HitTest
// брал заданную, и по кнопке с авто-шириной приходилось попадать не туда, где
// она нарисована. Теперь раскладка считается ОДИН раз за кадр, а рисование,
// попадание и ввод читают её результат.
struct Solved {
    entt::entity Entity;
    UIRect Rect;
    UIRect Clip;      // окно обрезки (нулевая ширина/высота — не обрезан)
    bool Clipped = false;
    float Alpha = 1.0f;      // накопленная прозрачность групп
    bool Interactive = true; // группа может запретить ввод всему поддереву
    // Масштаб холста этого корня: раскладка считается в опорных единицах, а
    // прямоугольник ниже — уже экранный. Число нужно отрисовке для всего, что
    // задано рядом с прямоугольником, но не выводится из него: кегль шрифта,
    // скругление, толщина рамки.
    float Scale = 1.0f;
    // Виден ли элемент сам по себе. Рантайму это не нужно (невидимые в список
    // не попадают вовсе), а редактору нужно: выключенный элемент надо ПОКАЗАТЬ
    // рамкой, иначе его нельзя найти и включить обратно.
    bool Visible = true;
};

// Рекурсивный обход: считает прямоугольники, применяет раскладку, маски и
// групповые свойства. forced — прямоугольник, назначенный раскладкой родителя
// (nullptr — элемент стоит по своему якорю).
void SolveSubtree(Scene& scene, entt::entity ent, const UIRect& parentRect, UIRenderer* ui,
                  bool clipped, const UIRect& clip, float alpha, bool interactive,
                  const UIRect* forced, bool includeHidden, std::vector<Solved>& out) {
    entt::registry& reg = scene.Registry();
    const Transform& t = reg.get<Transform>(ent);
    // Невидимый прячет и всё поддерево — но только в игре. Редактор просит
    // includeHidden и получает выключенные элементы тоже: иначе выключить
    // элемент значило бы потерять его насовсем.
    if (!t.Visible && !includeHidden) return;

    // ГЕОМЕТРИЯ БЕРЁТСЯ ИЗ Transform, а не из плоского описания.
    //
    // Плоское описание — это то, чем элемент РИСУЕТСЯ, и растяжения, полей и
    // точки привязки в нём нет: у прежнего компонента их не было вовсе.
    // Считать по нему раскладку значило бы, что панель «во всю ширину экрана»
    // молча остаётся размером 200x56 — то есть самая заметная возможность
    // новой системы не работает, и понять почему неоткуда.
    glm::vec2 size = ResolveSize(t, parentRect);
    // Ширина по содержимому — единственное, что знает шрифт, а не раскладка.
    if (ui) size = MeasuredWidth(reg, ent, size, *ui);
    UIRect r = forced ? *forced : Resolve(t, parentRect, size);
    if (forced) size = {forced->w, forced->h};
    // Фактический размер запоминается в САМОМ элементе: его читают попадание
    // курсором и следующий кадр, когда шрифта под рукой может не оказаться.
    reg.get<Transform>(ent).LayoutSize = size;

    // Групповые свойства накапливаются вниз по дереву: спрятать панель — это
    // одно число на ней, а не проход скриптом по каждому её ребёнку.
    float myAlpha = alpha;
    bool myInteractive = interactive;
    if (const Group* g = reg.try_get<Group>(ent)) {
        myAlpha *= glm::clamp(g->Alpha, 0.0f, 1.0f);
        if (!g->Interactable || !g->BlockRaycasts) myInteractive = false;
    }

    const size_t self = out.size();
    out.push_back(Solved{ent, r, clip, clipped, myAlpha, myInteractive});
    out.back().Visible = t.Visible;

    std::vector<entt::entity> kids = SortedUIChildren(scene, ent);
    if (kids.empty()) return;

    // Маска: окно обрезки пересекается с родительским — вложенные маски режут
    // друг друга (список внутри окна виден только на их пересечении).
    bool childClipped = clipped;
    UIRect childClip = clip;
    const Mask* mask = reg.try_get<Mask>(ent);
    if (mask) {
        const UIRect window = MaskWindow(*mask, r);
        if (!mask->ShowOutside) {
            childClip = childClipped ? Intersect(childClip, window) : window;
            childClipped = true;
        }
    }

    // Раскладка: контейнер сам расставляет детей. Их якоря при этом не
    // работают — в том и смысл, что позиции считает родитель.
    if (const Layout* layout = reg.try_get<Layout>(ent)) {
        std::vector<LayoutSlot> slots(kids.size());
        auto measure = [&] {
            for (size_t i = 0; i < kids.size(); ++i) {
                const Transform& kt = reg.get<Transform>(kids[i]);
                slots[i].Size = ResolveSize(kt, r);
                if (ui) slots[i].Size = MeasuredWidth(reg, kids[i], slots[i].Size, *ui);
            }
        };
        measure();
        const glm::vec2 content = ApplyLayout(*layout, r, slots);

        // FitContent: контейнер обнимает содержимое. Без этого панель задавалась
        // числом, которое разъезжается при добавлении строки — а настройка в
        // инспекторе была, и не делала ничего.
        //
        // Раскладка считается ВТОРОЙ раз: от размера контейнера зависят и
        // положение детей, и растяжение поперёк, поэтому подогнать его и
        // оставить прежние места нельзя.
        if (layout->FitContent && content.x > 0.0f && content.y > 0.0f) {
            // ApplyLayout возвращает место, занятое ДЕТЬМИ, — без полей
            // контейнера. Подогнать панель ровно по нему значит обрезать её на
            // величину полей: дети начинаются с отступа сверху, а панель
            // кончается там же, где последний ребёнок, и он вылезает наружу.
            const glm::vec2 padded{content.x + layout->Padding.x + layout->Padding.z,
                                   content.y + layout->Padding.y + layout->Padding.w};
            const glm::vec2 fitted =
                layout->Direction == Layout::Flow::Horizontal
                    ? glm::vec2{padded.x, r.h}
                    : (layout->Direction == Layout::Flow::Vertical ? glm::vec2{r.w, padded.y}
                                                                   : padded);
            if (fitted != glm::vec2{r.w, r.h}) {
                r = forced ? UIRect{r.x, r.y, fitted.x, fitted.y}
                           : Resolve(t, parentRect, fitted);
                reg.get<Transform>(ent).LayoutSize = fitted;
                out[self].Rect = r;
                measure();
                ApplyLayout(*layout, r, slots);
            }
        }
        for (size_t i = 0; i < kids.size(); ++i) {
            const UIRect kr{slots[i].Pos.x, slots[i].Pos.y, slots[i].Size.x, slots[i].Size.y};
            SolveSubtree(scene, kids[i], r, ui, childClipped, childClip, myAlpha, myInteractive,
                         &kr, includeHidden, out);
        }
        return;
    }

    for (auto k : kids) {
        SolveSubtree(scene, k, r, ui, childClipped, childClip, myAlpha, myInteractive, nullptr,
                     includeHidden, out);
    }
}

// Все элементы сцены в ПОРЯДКЕ ОТРИСОВКИ. ui нужен для измерения текста; без
// него берётся размер, посчитанный на прошлом кадре.
std::vector<Solved> SolveScene(Scene& scene, UIRenderer* ui, int screenW, int screenH,
                              bool includeHidden = false) {
    std::vector<Solved> out;
    entt::registry& reg = scene.Registry();
    for (auto root : SortedUIRoots(scene)) {
        // Холст задаёт масштаб интерфейса: свёрстанное под 1920x1080 не должно
        // сжиматься вчетверо на 4K.
        UIRect screen{0.0f, 0.0f, (float)screenW, (float)screenH};
        float scale = 1.0f;
        if (const Canvas* c = reg.try_get<Canvas>(root)) {
            const float k = CanvasScale(*c, {(float)screenW, (float)screenH});
            if (k > 0.0f && k != 1.0f) {
                scale = k;
                screen.w = (float)screenW / scale;
                screen.h = (float)screenH / scale;
            }
        }
        const size_t first = out.size();
        SolveSubtree(scene, root, screen, ui, false, UIRect{}, 1.0f, true, nullptr, includeHidden,
                     out);
        // ПЕРЕВОД В ЭКРАННЫЕ КООРДИНАТЫ. Раскладка считалась в опорных единицах
        // холста — иначе вёрстка под 1920x1080 не сохранила бы пропорции на
        // другом разрешении. Дальше её читают отрисовка, попадание курсором и
        // ввод, и всем троим нужны настоящие пиксели: пока перевода не было,
        // интерфейс с холстом уезжал за край экрана.
        if (scale != 1.0f) {
            for (size_t i = first; i < out.size(); ++i) {
                Solved& it = out[i];
                it.Rect = {it.Rect.x * scale, it.Rect.y * scale, it.Rect.w * scale,
                           it.Rect.h * scale};
                it.Clip = {it.Clip.x * scale, it.Clip.y * scale, it.Clip.w * scale,
                           it.Clip.h * scale};
                it.Scale = scale;
            }
        }
    }
    return out;
}

} // namespace

std::vector<ElementRect> SolveSceneRects(Scene& scene, int screenW, int screenH,
                                         bool includeHidden) {
    entt::registry& reg = scene.Registry();
    // Без UIRenderer: авто-ширину надписи меряет шрифт, а его здесь нет.
    // Прошлый кадр её уже посчитал и положил в Transform::LayoutSize, поэтому
    // рамка редактора отстаёт от изменившегося текста ровно на один кадр —
    // цена за то, что редактор не тащит за собой отрисовку.
    const std::vector<Solved> items = SolveScene(scene, nullptr, screenW, screenH, includeHidden);

    std::vector<ElementRect> out;
    out.reserve(items.size());
    for (const Solved& it : items) {
        ElementRect e;
        e.Entity = it.Entity;
        e.Rect = it.Rect;
        e.Scale = it.Scale;
        e.Visible = it.Visible;
        // Прямоугольник родителя ищется среди уже посчитанных: считать его
        // заново значило бы завести вторую версию тех же формул.
        const entt::entity parent = scene.ParentOf(it.Entity);
        e.Parent = UIRect{0.0f, 0.0f, (float)screenW, (float)screenH};
        if (parent != entt::null && reg.valid(parent)) {
            for (const Solved& p : items)
                if (p.Entity == parent) { e.Parent = p.Rect; break; }
            e.InLayout = reg.all_of<Layout>(parent);
        }
        out.push_back(e);
    }
    return out;
}

namespace {

bool PointIn(const UIRect& r, glm::vec2 p) {
    return p.x >= r.x && p.x < r.x + r.w && p.y >= r.y && p.y < r.y + r.h;
}

} // namespace

void DrawSceneUI(Scene& scene, UIRenderer& ui, int screenW, int screenH) {
    SAGE_PROFILE("Интерфейс сцены");
    const std::vector<Solved> items = SolveScene(scene, &ui, screenW, screenH);
    const entt::registry& reg = scene.Registry();

    for (const Solved& it : items) {
        if (it.Clipped) {
            if (it.Clip.w <= 0.0f || it.Clip.h <= 0.0f) continue; // полностью обрезан
            ui.PushClipRect(it.Clip.x, it.Clip.y, it.Clip.w, it.Clip.h);
        }
        // Прозрачность группы идёт отдельным числом и множится на КАЖДЫЙ цвет:
        // панель с полупрозрачным фоном не должна становиться непрозрачной от
        // того, что группу показали наполовину.
        DrawElement(reg, it.Entity, it.Rect, it.Scale, it.Alpha, ui);
        if (it.Clipped) ui.PopClipRect();
    }
}

int HitTest(Scene& scene, float x, float y, int screenW, int screenH) {
    const std::vector<Solved> items = SolveScene(scene, nullptr, screenW, screenH);
    const entt::registry& reg = scene.Registry();
    int bestId = -1;
    for (const Solved& it : items) {
        if (it.Clipped && !PointIn(it.Clip, {x, y})) continue;
        if (PointIn(it.Rect, {x, y})) bestId = reg.get<IdComponent>(it.Entity).Id;
    }
    return bestId;
}

UIInputResult UpdateSceneUI(Scene& scene, const UIInputState& input, int screenW, int screenH) {
    UIInputResult result;
    result.Size = glm::vec2((float)screenW, (float)screenH);
    entt::registry& reg = scene.Registry();
    const std::vector<Solved> items = SolveScene(scene, nullptr, screenW, screenH);

    // Состояние взаимодействия живёт в Interactable::Runtime, и его НЕТ у
    // элементов, которые мышь не ловят. Это не мелочь: раньше поля Hovered,
    // Pressed и Caret были у каждой надписи и каждой рамки, и «под курсором» у
    // подписи означало ровно ничего — но проверить это было нельзя, потому что
    // поле есть у всех.
    auto stateOf = [&reg](entt::entity e) -> State* {
        Interactable* act = reg.try_get<Interactable>(e);
        return act ? &act->Runtime : nullptr;
    };
    auto usable = [&reg](entt::entity e) {
        const Interactable* act = reg.try_get<Interactable>(e);
        return act && act->Enabled;
    };
    // Текст поля ввода — это надпись элемента: у поля без надписи набирать
    // некуда, и заводить ей отдельное хранилище значило бы держать две строки,
    // из которых видна одна.
    auto textOf = [&reg](entt::entity e) -> Label* { return reg.try_get<Label>(e); };

    // Кто под курсором: последний нарисованный из тех, кто ловит мышь и не
    // обрезан своей маской.
    entt::entity hovered = entt::null;
    for (const Solved& it : items) {
        if (!it.Interactive || !usable(it.Entity)) continue;
        if (it.Clipped && !PointIn(it.Clip, input.Mouse)) continue;
        if (PointIn(it.Rect, input.Mouse)) hovered = it.Entity;
    }

    // СВЯЗИ СОБЫТИЙ. Кнопка делает то, что у неё настроено, САМА — не дожидаясь
    // скрипта, который каждый кадр спрашивал бы «не нажали ли». Отправка идёт в
    // шину сцены (Scene::Events), которую слушают и Lua, и код на C++.
    //
    // Отправитель — Id элемента: без него обработчик «нажали кнопку» не узнает,
    // КАКУЮ нажали, и каждой кнопке пришлось бы придумывать своё имя события.
    auto fire = [&](entt::entity e, const char* trigger) {
        const Interactable* act = reg.try_get<Interactable>(e);
        if (!act || act->Events.empty()) return;
        const int sender = reg.all_of<IdComponent>(e) ? reg.get<IdComponent>(e).Id : 0;
        for (const sage::events::Binding* b : sage::events::ForTrigger(act->Events, trigger)) {
            sage::events::Event ev;
            // Имя события не задано — берём имя триггера: связь, у которой
            // забыли вписать событие, должна быть заметна, а не молчать.
            ev.Name = b->Event.empty() ? std::string(trigger) : b->Event;
            ev.Arg = b->Arg;
            ev.Sender = sender;
            // Адресная часть едет В ТОМ ЖЕ событии: настроить связь дважды —
            // отдельно «кому» и отдельно «что» — значит однажды поправить одно
            // и забыть другое.
            ev.Target = b->Target;
            ev.Method = b->Method;
            scene.Events.Emit(ev);
        }
    };

    // Флаги «за этот кадр» гасим у всех: их читает игра сразу после нас, и
    // оставшийся с прошлого кадра Clicked сработал бы второй раз.
    for (const Solved& it : items) {
        if (State* st = stateOf(it.Entity)) {
            const bool wasHovered = st->Hovered;
            st->Clicked = false;
            st->Changed = false;
            st->Hovered = (it.Entity == hovered);
            // Вход и выход курсора — отдельные события: подсветка соседа,
            // подсказка и звук наведения нужны именно на переходе, а не каждый
            // кадр, пока курсор стоит на месте.
            if (st->Hovered && !wasHovered) fire(it.Entity, "hoverIn");
            if (!st->Hovered && wasHovered) fire(it.Entity, "hoverOut");
            // Pressed здесь НЕ сбрасываем: в кадре отпускания кнопка уже не
            // удерживается, и сброс до разбора отпускания съел бы сам щелчок.
        }
    }

    // Нажатие: назначает фокус (полю ввода) и «прижимает» элемент.
    if (input.MousePressed) {
        if (hovered != entt::null) {
            result.PressedAction = reg.get<Interactable>(hovered).Action;
            fire(hovered, "press");
        }
        for (const Solved& it : items) {
            State* st = stateOf(it.Entity);
            if (!st) continue;
            const bool hit = (it.Entity == hovered);
            if (st->Focused && !hit) st->Focused = false; // клик мимо снимает фокус
            if (!hit) continue;
            st->Pressed = true;
            if (reg.all_of<TextInput>(it.Entity)) {
                st->Focused = true;
                const Label* lbl = textOf(it.Entity);
                st->Caret = lbl ? (int)lbl->Text.size() : 0;
                st->CaretBlink = 0.0f;
            }
        }
    }

    // Отпускание НАД тем же элементом — это и есть щелчок. Отпускание в стороне
    // щелчком не считается: увести палец с кнопки — общепринятый способ
    // передумать, и ломать его нельзя.
    if (input.MouseReleased && hovered != entt::null) {
        // Отпустили НАД этим элементом — независимо от того, где нажали.
        // Именно этим щелчок отличается от переноса, и знать надо оба.
        result.ReleasedAction = reg.get<Interactable>(hovered).Action;
        fire(hovered, "release");
        if (State* st = stateOf(hovered)) {
            if (st->Pressed) {
                st->Clicked = true;
                result.ClickedId = reg.get<IdComponent>(hovered).Id;
                result.ClickedAction = reg.get<Interactable>(hovered).Action;
                // Галка — тот же диапазон, у которого два конца: щелчок
                // перекидывает значение между ними.
                if (Range* range = reg.try_get<Range>(hovered); range && range->Toggle) {
                    const float mid = (range->Min + range->Max) * 0.5f;
                    range->Value = range->Value >= mid ? range->Min : range->Max;
                    st->Changed = true;
                }
                fire(hovered, "click");
                // Значение изменилось щелчком по галке — это то же «change»,
                // что и у ползунка: слушателю всё равно, чем его подвинули.
                if (st->Changed) fire(hovered, "change");
            }
        }
    }

    // Ползунок: тянется, пока кнопка удерживается, даже если курсор ушёл за
    // пределы дорожки — иначе значение срывается от малейшего движения вбок.
    for (const Solved& it : items) {
        Range* range = reg.try_get<Range>(it.Entity);
        State* st = stateOf(it.Entity);
        if (!range || range->Toggle || !st || !usable(it.Entity)) continue;
        if (!st->Pressed || !input.MouseDown) continue;
        const float w = glm::max(it.Rect.w, 1.0f);
        const float t = glm::clamp((input.Mouse.x - it.Rect.x) / w, 0.0f, 1.0f);
        float v = range->Min + t * (range->Max - range->Min);
        // Шаг: громкость по 5% должна прилипать к пятёркам, иначе ползунок
        // выдаёт 0.4732 там, где человек ждёт 0.45.
        if (range->Step > 0.0f) v = range->Min + std::round((v - range->Min) / range->Step) * range->Step;
        v = glm::clamp(v, glm::min(range->Min, range->Max), glm::max(range->Min, range->Max));
        if (v != range->Value) {
            range->Value = v;
            st->Changed = true;
            fire(it.Entity, "change");
        }
        result.WantsMouse = true;
    }

    // Ввод текста — только в поле с фокусом.
    for (const Solved& it : items) {
        const TextInput* field = reg.try_get<TextInput>(it.Entity);
        State* st = stateOf(it.Entity);
        Label* lbl = textOf(it.Entity);
        if (!field || !st || !lbl || !st->Focused || !usable(it.Entity)) continue;
        result.WantsKeyboard = true;
        st->CaretBlink += input.DeltaTime;
        st->Caret = glm::clamp(st->Caret, 0, (int)lbl->Text.size());
        if (field->ReadOnly) continue;   // показывать можно, править нельзя

        if (!input.TypedText.empty()) {
            const bool room = field->MaxLength <= 0 ||
                              Utf8Length(lbl->Text) + Utf8Length(input.TypedText) <= field->MaxLength;
            if (room) {
                lbl->Text.insert((size_t)st->Caret, input.TypedText);
                st->Caret += (int)input.TypedText.size();
                st->Changed = true;
                st->CaretBlink = 0.0f;
            }
        }
        if (input.Backspace && st->Caret > 0) {
            const int prev = PrevCharBoundary(lbl->Text, st->Caret);
            lbl->Text.erase((size_t)prev, (size_t)(st->Caret - prev));
            st->Caret = prev;
            st->Changed = true;
            st->CaretBlink = 0.0f;
        }
        if (input.Delete && st->Caret < (int)lbl->Text.size()) {
            const int next = NextCharBoundary(lbl->Text, st->Caret);
            lbl->Text.erase((size_t)st->Caret, (size_t)(next - st->Caret));
            st->Changed = true;
            st->CaretBlink = 0.0f;
        }
        if (input.Left) { st->Caret = PrevCharBoundary(lbl->Text, st->Caret); st->CaretBlink = 0.0f; }
        if (input.Right) { st->Caret = NextCharBoundary(lbl->Text, st->Caret); st->CaretBlink = 0.0f; }
        if (input.Home) { st->Caret = 0; st->CaretBlink = 0.0f; }
        if (input.End) { st->Caret = (int)lbl->Text.size(); st->CaretBlink = 0.0f; }
        if (input.Enter || input.Escape) st->Focused = false;
    }

    // Сглаживание полос: значение едет к цели, а не прыгает.
    for (const Solved& it : items) {
        if (Bar* bar = reg.try_get<Bar>(it.Entity)) {
            if (bar->Smoothing <= 0.0f) { bar->Displayed = bar->Value; continue; }
            if (bar->Displayed < 0.0f) bar->Displayed = bar->Value;
            const float step = bar->Smoothing * input.DeltaTime;
            const float diff = bar->Value - bar->Displayed;
            bar->Displayed += glm::clamp(diff, -step, step);
        }
    }

    // Кнопка отпущена — гасим «прижатие» у всех. ПОСЛЕ разбора отпускания:
    // до него Pressed ещё нужен, чтобы отличить щелчок от «отпустил в стороне».
    if (!input.MouseDown) {
        for (const Solved& it : items) {
            if (State* st = stateOf(it.Entity)) st->Pressed = false;
        }
    }

    if (hovered != entt::null) result.WantsMouse = true;

    // Что видела мышь в этом кадре — в саму сцену: игра читает это через
    // sage.ui.* и не зависит от того, кто именно крутит кадр (см. Scene::UiFrame).
    result.Cursor = input.Mouse;
    result.MouseDown = input.MouseDown;
    scene.UiFrame = result;
    return result;
}

} // namespace sage::ui
