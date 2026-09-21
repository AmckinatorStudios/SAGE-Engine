#include "UISceneSystem.h"
#include "sage/ui/UISerialize.h"
#include "sage/ui/UIPart.h"

#include "sage/ui/UI.h"
#include "sage/scene/SceneLegacyUI.h"
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
    return reg.valid(e) && reg.all_of<Element>(e);
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
    // Ширина по тексту считается ТЕМ ЖЕ шрифтом и начертанием, которым текст
    // будет нарисован: посчитанная шрифтом интерфейса, она обрезала бы конец
    // надписи, набранной своим шрифтом или жирным.
    if (!label->Text.empty() && label->Color.a > 0.0f) {
        UITextStyle style;
        if (!label->Font.empty())
            style.UseFont = ui.LoadFont(label->Font, label->FontPixelHeight, label->FontPixelArt);
        style.Bold = label->Face == Label::Style::Bold || label->Face == Label::Style::BoldItalic;
        style.Italic = label->Face == Label::Style::Italic || label->Face == Label::Style::BoldItalic;
        w += ui.MeasureText(label->Text, label->Scale, style);
    }
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
        const Element& ta = reg.get<Element>(a);
        const Element& tb = reg.get<Element>(b);
        if (ta.Order != tb.Order) return ta.Order < tb.Order;
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
// Порядок КОРНЕЙ ОДНОГО интерфейса: больше Order — выше (рисуется поверх).
// Порядок между интерфейсами задаёт их собственный SortOrder (см.
// SortedInterfaces), а не число на элементе: раньше это было поле Canvas у
// корня, то есть у элемента — и два корня одного интерфейса могли спорить о
// том, каким он показывается.
void SortRoots(Scene& scene, std::vector<entt::entity>& roots) {
    entt::registry& reg = scene.Registry();
    auto canvasOrder = [&reg](entt::entity e) {
        const Canvas* c = reg.try_get<Canvas>(e);
        return c ? c->SortOrder : 0;
    };
    std::stable_sort(roots.begin(), roots.end(), [&](entt::entity a, entt::entity b) {
        const int ca = canvasOrder(a), cb = canvasOrder(b);
        if (ca != cb) return ca < cb;
        const Element& ta = reg.get<Element>(a);
        const Element& tb = reg.get<Element>(b);
        if (ta.Order != tb.Order) return ta.Order < tb.Order;
        return reg.get<IdComponent>(a).Id < reg.get<IdComponent>(b).Id;
    });
}

std::vector<entt::entity> SortedUIRoots(Scene& scene) {
    std::vector<entt::entity> roots;
    entt::registry& reg = scene.Registry();
    for (auto e : reg.view<Element>()) {
        entt::entity parent = scene.ParentOf(e);
        if (parent == entt::null || !IsElement(reg, parent)) roots.push_back(e);
    }
    SortRoots(scene, roots);
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

    // ПОВОРОТ — на весь элемент разом, включая текст и значки: рисовальщиков у
    // него семь, и заставить каждого знать про поворот значит получить семь
    // мест, где про него забудут.
    const Element* box = reg.try_get<Element>(e);
    const bool rotated = box && std::fabs(box->Rotation) > 0.0001f;
    if (rotated) ui.PushRotation({r.x + r.w * 0.5f, r.y + r.h * 0.5f}, box->Rotation);

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

    if (rotated) ui.PopRotation();
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
    // Виден ли элемент сам по себе.
    //
    // Невидимые ОСТАЮТСЯ в списке, и это не недосмотр: спрятанный элемент
    // держит своё место в раскладке родителя, иначе «спрятать кнопку на время»
    // означало бы, что все соседи в списке разъехались и вернулись обратно
    // рывком. Не рисуют и не ловят мышь их те, кто читает этот список.
    //
    // Редактору флаг нужен отдельно: выключенный элемент надо ПОКАЗАТЬ рамкой,
    // иначе его нельзя найти и включить обратно.
    bool Visible = true;
};

// Насколько сдвинуть содержимое прокручиваемого элемента.
//
// ЗАПОМИНАЕТ РАЗМЕР СОДЕРЖИМОГО В САМОМ КОМПОНЕНТЕ: предел прокрутки
// («дальше своих границ не пускать») известен только после раскладки, а
// применить его надо к тому же кадру. Без этого список либо не докручивался
// до конца, либо уезжал в пустоту — и то и другое выглядит как сломанная
// прокрутка, а не как выключенный флажок.
glm::vec2 ScrollShift(entt::registry& reg, entt::entity ent, const UIRect& r,
                      glm::vec2 content) {
    Scroll* s = reg.try_get<Scroll>(ent);
    if (!s) return glm::vec2(0.0f);
    if (content.x > 0.0f || content.y > 0.0f) s->Content = content;
    glm::vec2 off = s->Offset;
    if (!s->Horizontal) off.x = 0.0f;
    if (!s->Vertical) off.y = 0.0f;
    if (s->Clamp) {
        const float maxX = std::max(0.0f, s->Content.x - r.w);
        const float maxY = std::max(0.0f, s->Content.y - r.h);
        off.x = glm::clamp(off.x, 0.0f, maxX);
        off.y = glm::clamp(off.y, 0.0f, maxY);
        s->Offset = glm::vec2(s->Horizontal ? off.x : s->Offset.x,
                              s->Vertical ? off.y : s->Offset.y);
    }
    return -off;   // содержимое уезжает ВВЕРХ, когда крутят вниз
}

// Рекурсивный обход: считает прямоугольники, применяет раскладку, маски и
// групповые свойства. forced — прямоугольник, назначенный раскладкой родителя
// (nullptr — элемент стоит по своему якорю).
void SolveSubtree(Scene& scene, entt::entity ent, const UIRect& parentRect, UIRenderer* ui,
                  bool clipped, const UIRect& clip, float alpha, bool interactive,
                  const UIRect* forced, bool includeHidden, std::vector<Solved>& out) {
    entt::registry& reg = scene.Registry();
    const Element& t = reg.get<Element>(ent);
    // НЕАКТИВНЫЙ выпадает из всего вместе с поддеревом: ни отрисовки, ни ввода,
    // ни участия в раскладке родителя. НЕВИДИМЫЙ — только не рисуется: место он
    // держит, и соседи в списке не съезжают, пока он спрятан. Пока флаг был
    // один, «спрятать панель на время анимации, не сломав раскладку соседей»
    // выразить было нечем.
    //
    // Редактор просит includeHidden и получает и то, и другое: иначе выключить
    // элемент значило бы потерять его насовсем.
    if (!t.Active && !includeHidden) return;

    // ГЕОМЕТРИЯ БЕРЁТСЯ ИЗ Element, а не из плоского описания.
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
    reg.get<Element>(ent).Resolved = size;

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
    out.back().Visible = t.Shown();

    std::vector<entt::entity> kids = SortedUIChildren(scene, ent);
    // ВЫКЛЮЧЕННЫЙ РЕБЁНОК НЕ ЗАНИМАЕТ МЕСТА В РАСКЛАДКЕ. Отсеивать его надо
    // ЗДЕСЬ, до ApplyLayout: рекурсия ниже и так не пойдёт в выключенного, но
    // место под него уже было бы выделено, и в списке осталась бы дыра — то
    // есть «выключить» выглядело бы как «спрятать», и разницы между флагами не
    // стало бы. Спрятанный (Visible=false) место держит и потому остаётся.
    if (!includeHidden) {
        kids.erase(std::remove_if(kids.begin(), kids.end(),
                                  [&reg](entt::entity k) {
                                      return !reg.get<Element>(k).Active;
                                  }),
                   kids.end());
    }
    if (kids.empty()) return;

    // Маска: окно обрезки пересекается с родительским — вложенные маски режут
    // друг друга (список внутри окна виден только на их пересечении).
    bool childClipped = clipped;
    UIRect childClip = clip;
    const Mask* mask = reg.try_get<Mask>(ent);
    // ПРОКРУТКА РЕЖЕТ САМА, даже без маски. Содержимое, уехавшее за край
    // элемента, и есть то, ради чего прокрутку включают: не обрезать его
    // значит нарисовать весь список поверх соседних панелей — то есть
    // получить не прокрутку, а кашу. Отдельной галкой это не делается:
    // «прокрутка без обрезки» не значит ничего.
    const Scroll* scroll = reg.try_get<Scroll>(ent);
    if (scroll) {
        childClip = childClipped ? Intersect(childClip, r) : r;
        childClipped = true;
    }
    if (mask) {
        const UIRect window = MaskWindow(*mask, r);
        if (!mask->ShowOutside) {
            childClip = childClipped ? Intersect(childClip, window) : window;
            childClipped = true;
        }
    }

    // Раскладка: контейнер сам расставляет детей. Их якоря при этом не
    // работают — в том и смысл, что позиции считает родитель.
    if (const Stack* layout = reg.try_get<Stack>(ent)) {
        std::vector<LayoutSlot> slots(kids.size());
        auto measure = [&] {
            for (size_t i = 0; i < kids.size(); ++i) {
                const Element& kt = reg.get<Element>(kids[i]);
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
                layout->Direction == Stack::Flow::Horizontal
                    ? glm::vec2{padded.x, r.h}
                    : (layout->Direction == Stack::Flow::Vertical ? glm::vec2{r.w, padded.y}
                                                                   : padded);
            if (fitted != glm::vec2{r.w, r.h}) {
                r = forced ? UIRect{r.x, r.y, fitted.x, fitted.y}
                           : Resolve(t, parentRect, fitted);
                reg.get<Element>(ent).Resolved = fitted;
                out[self].Rect = r;
                measure();
                ApplyLayout(*layout, r, slots);
            }
        }
        // СДВИГ ПРОКРУТКОЙ — В САМОМ КОНЦЕ, к готовым местам. Считать
        // раскладку в сдвинутых координатах нельзя: от них зависят и
        // выравнивание, и перенос по столбцам, и «по содержимому», — список
        // менял бы форму от того, насколько его прокрутили.
        const glm::vec2 shift = ScrollShift(reg, ent, r, content);
        for (size_t i = 0; i < kids.size(); ++i) {
            const UIRect kr{slots[i].Pos.x + shift.x, slots[i].Pos.y + shift.y, slots[i].Size.x,
                            slots[i].Size.y};
            SolveSubtree(scene, kids[i], r, ui, childClipped, childClip, myAlpha, myInteractive,
                         &kr, includeHidden, out);
        }
        return;
    }

    // Без раскладки дети стоят по своим якорям ВНУТРИ родителя, и сдвинуть их
    // можно, сдвинув сам прямоугольник, от которого они считаются. Обрезка при
    // этом остаётся по настоящему элементу — иначе уехало бы и окно.
    UIRect inner = r;
    if (scroll) {
        const glm::vec2 shift = ScrollShift(reg, ent, r, glm::vec2(0.0f));
        inner.x += shift.x;
        inner.y += shift.y;
    }
    for (auto k : kids) {
        SolveSubtree(scene, k, inner, ui, childClipped, childClip, myAlpha, myInteractive, nullptr,
                     includeHidden, out);
    }
}

// Все элементы сцены в ПОРЯДКЕ ОТРИСОВКИ. ui нужен для измерения текста; без
// него берётся размер, посчитанный на прошлом кадре.
std::vector<Solved> SolveScene(Scene& scene, UIRenderer* ui, int screenW, int screenH,
                              bool includeHidden = false,
                              const UIScope& scope = UIScope::All()) {
    std::vector<Solved> out;
    entt::registry& reg = scene.Registry();

    // ГРУППА — ЭТО ИНТЕРФЕЙС. Раньше корни сцены шли одним списком, и два
    // интерфейса перемешивались между собой: порядок решало число на элементе,
    // а не то, какому экрану он принадлежит. Теперь считается интерфейс
    // целиком: свои корни, свой холст, своя видимость.
    struct Group {
        entt::entity Interface = entt::null;
        const InterfaceComponent* Info = nullptr;
    };
    std::vector<Group> groups;
    // «Без интерфейса» — первым: такие элементы собирают кодом и скриптом
    // (превью ассета, тест, интерфейс, созданный на лету), и они существуют
    // независимо от того, завёл ли кто-то интерфейсы в сцене.
    if (scope.Accepts(entt::null) && !InterfaceRoots(scene, entt::null).empty())
        groups.push_back({entt::null, nullptr});
    for (entt::entity iface : SortedInterfaces(scene)) {
        if (!scope.Accepts(iface)) continue;
        const InterfaceComponent& info = reg.get<InterfaceComponent>(iface);
        // Выключенный интерфейс не рисуется и не ловит мышь — целиком.
        // includeHidden (режим редактора) показывает и его: иначе выключенный
        // интерфейс нельзя было бы ни найти, ни включить обратно.
        if (!info.Visible && !includeHidden) continue;
        groups.push_back({iface, &info});
    }

    for (const Group& group : groups) {
        UIRect screen{0.0f, 0.0f, (float)screenW, (float)screenH};
        float scale = 1.0f;
        std::vector<entt::entity> roots = InterfaceRoots(scene, group.Interface);
        if (roots.empty()) continue;

        // Холст ИНТЕРФЕЙСА, а не первого его элемента. Компонент Canvas на
        // корневом элементе по-прежнему читается: так верстали до появления
        // интерфейсов, и сцены с ним обязаны открываться как раньше.
        const Canvas* canvas = group.Info ? &group.Info->Canvas : reg.try_get<Canvas>(roots.front());
        if (canvas) {
            const float k = CanvasScale(*canvas, {(float)screenW, (float)screenH});
            if (k > 0.0f && k != 1.0f) {
                scale = k;
                screen.w = (float)screenW / scale;
                screen.h = (float)screenH / scale;
            }
        }

        const size_t first = out.size();
        for (entt::entity root : roots)
            SolveSubtree(scene, root, screen, ui, false, UIRect{}, 1.0f, true, nullptr,
                         includeHidden, out);
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

entt::entity InterfaceOf(Scene& scene, entt::entity element) {
    entt::registry& reg = scene.Registry();
    for (entt::entity e = element; e != entt::null && reg.valid(e); e = scene.ParentOf(e))
        if (reg.all_of<InterfaceComponent>(e)) return e;
    return entt::null;
}

std::vector<entt::entity> SortedInterfaces(Scene& scene) {
    std::vector<entt::entity> out;
    entt::registry& reg = scene.Registry();
    for (entt::entity e : reg.view<InterfaceComponent>()) out.push_back(e);
    std::stable_sort(out.begin(), out.end(), [&reg](entt::entity a, entt::entity b) {
        const int sa = reg.get<InterfaceComponent>(a).SortOrder;
        const int sb = reg.get<InterfaceComponent>(b).SortOrder;
        if (sa != sb) return sa < sb;
        // При равном порядке — по номеру объекта: порядок обхода ECS не обещан
        // и меняется при удалении сущностей, а «какое меню сверху» не должно
        // зависеть от того, что удалили в соседнем углу сцены.
        const IdComponent* ia = reg.try_get<IdComponent>(a);
        const IdComponent* ib = reg.try_get<IdComponent>(b);
        return (ia ? ia->Id : 0) < (ib ? ib->Id : 0);
    });
    return out;
}

std::vector<entt::entity> InterfaceRoots(Scene& scene, entt::entity interfaceEntity) {
    std::vector<entt::entity> roots;
    entt::registry& reg = scene.Registry();
    for (entt::entity e : reg.view<Element>()) {
        // Корень — тот, над кем нет ЭЛЕМЕНТА: он якорится к экрану, а не к
        // чужому прямоугольнику.
        const entt::entity parent = scene.ParentOf(e);
        if (parent != entt::null && IsElement(reg, parent)) continue;
        if (InterfaceOf(scene, e) != interfaceEntity) continue;
        roots.push_back(e);
    }
    SortRoots(scene, roots);
    return roots;
}

std::vector<ElementRect> SolveSceneRects(Scene& scene, int screenW, int screenH,
                                         bool includeHidden, const UIScope& scope) {
    entt::registry& reg = scene.Registry();
    // Без UIRenderer: авто-ширину надписи меряет шрифт, а его здесь нет.
    // Прошлый кадр её уже посчитал и положил в Element::LayoutSize, поэтому
    // рамка редактора отстаёт от изменившегося текста ровно на один кадр —
    // цена за то, что редактор не тащит за собой отрисовку.
    const std::vector<Solved> items =
        SolveScene(scene, nullptr, screenW, screenH, includeHidden, scope);

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
            e.InLayout = reg.all_of<Stack>(parent);
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

void DrawSceneUI(Scene& scene, UIRenderer& ui, int screenW, int screenH, const UIScope& scope) {
    SAGE_PROFILE("Интерфейс сцены");
    // КАРТИНКИ — ПЕРЕД ОТРИСОВКОЙ. Путь мог смениться с прошлого кадра (слот в
    // инспекторе, перетаскивание файла, отмена, скрипт), а загрузка жила
    // только в чтении сцены: картинка появлялась лишь после Play/Stop.
    scene.Registry().view<Image>().each([](Image& im) { EnsureImageTexture(im); });

    const std::vector<Solved> items =
        SolveScene(scene, &ui, screenW, screenH, /*includeHidden=*/false, scope);
    const entt::registry& reg = scene.Registry();

    for (const Solved& it : items) {
        // Спрятанный элемент место в раскладке держит, а на экране его нет.
        if (!it.Visible) continue;
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

int HitTest(Scene& scene, float x, float y, int screenW, int screenH, const UIScope& scope) {
    const std::vector<Solved> items =
        SolveScene(scene, nullptr, screenW, screenH, /*includeHidden=*/false, scope);
    const entt::registry& reg = scene.Registry();
    int bestId = -1;
    for (const Solved& it : items) {
        // Невидимый не ловит точку. Прозрачная зона нажатия делается заливкой с
        // нулевой альфой, а не спрятанным элементом: спрятанный, который всё
        // равно кликается, — это ловушка, которую не видно ни на экране, ни в
        // дереве.
        if (!it.Visible) continue;
        if (it.Clipped && !PointIn(it.Clip, {x, y})) continue;
        if (PointIn(it.Rect, {x, y})) bestId = reg.get<IdComponent>(it.Entity).Id;
    }
    return bestId;
}

UIInputResult UpdateSceneUI(Scene& scene, const UIInputState& input, int screenW, int screenH,
                            const UIScope& scope) {
    UIInputResult result;
    result.Size = glm::vec2((float)screenW, (float)screenH);
    entt::registry& reg = scene.Registry();
    std::vector<Solved> items =
        SolveScene(scene, nullptr, screenW, screenH, /*includeHidden=*/false, scope);
    // Спрятанный не ловит ввод по той же причине, что и не ловит точку.
    items.erase(std::remove_if(items.begin(), items.end(),
                               [](const Solved& s) { return !s.Visible; }),
                items.end());
    // И интерфейс, который ПОКАЗЫВАЮТ, но которым не пользуются: заставка,
    // титры, подсказка поверх игры. Без этого «не ловит мышь» пришлось бы
    // выражать прозрачной заглушкой поверх всего экрана.
    items.erase(std::remove_if(items.begin(), items.end(),
                               [&scene, &reg](const Solved& s) {
                                   const entt::entity iface = InterfaceOf(scene, s.Entity);
                                   if (iface == entt::null) return false;
                                   const InterfaceComponent* info =
                                       reg.try_get<InterfaceComponent>(iface);
                                   return info && !info->ReceivesInput;
                               }),
                items.end());

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

    // --- КОЛЕСО: ПРОКРУТКА ---------------------------------------------------
    //
    // Ищется САМЫЙ ВЕРХНИЙ прокручиваемый под курсором, и не важно, ловит ли он
    // мышь: список с прокруткой обычно из неё и состоит — панель, а внутри
    // кнопки. Требовать от него ещё и Interactable значило бы «крутится только
    // то, что нажимается», а это разные вопросы.
    //
    // Вложенные прокрутки: крутится ВНУТРЕННЯЯ, потому что items идут в порядке
    // отрисовки, и последний совпавший — самый глубокий. Ровно этого и ждут:
    // курсор стоит над внутренним списком.
    if (input.Wheel != 0.0f) {
        Scroll* target = nullptr;
        for (const Solved& it : items) {
            if (it.Clipped && !PointIn(it.Clip, input.Mouse)) continue;
            if (!PointIn(it.Rect, input.Mouse)) continue;
            if (Scroll* sc = reg.try_get<Scroll>(it.Entity)) target = sc;
        }
        if (target) {
            // Вертикаль в приоритете: колесо у мыши одно, и список, у которого
            // разрешены обе оси, крутят вниз, а не вбок.
            if (target->Vertical) target->Offset.y -= input.Wheel * target->Speed;
            else if (target->Horizontal) target->Offset.x -= input.Wheel * target->Speed;
            // Щелчок колеса СЪЕДЕН интерфейсом: иначе тот же щелчок отъедет
            // камерой сцены, и список прокрутится вместе с миром за ним.
            result.WantsMouse = true;
        }
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
