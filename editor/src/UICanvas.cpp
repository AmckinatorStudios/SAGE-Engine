#include "UICanvas.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "EditorHost.h"
#include "Localization.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "sage/ui/UI.h"
#include "sage/ui/UILayoutTools.h"
#include "sage/ui/UISceneSystem.h"

namespace {

using sage::ui::UIRect;

constexpr float kHandle = 5.0f;      // половина стороны ручки в пикселях панели
constexpr float kRotateArm = 26.0f;  // длина ножки ручки поворота в пикселях панели
constexpr float kHandleGrab = 7.0f;  // радиус захвата — больше рисунка: попасть в
                                     // квадратик 10x10 мышью тяжело, и это чувствуется

// ПОВОРОТ ТОЧКИ ВОКРУГ ЦЕНТРА ЭЛЕМЕНТА.
//
// Рамка выделения, ручки размера и ручка поворота обязаны стоять ТАМ ЖЕ, ГДЕ
// САМ ЭЛЕМЕНТ. Пока их рисовали по неповёрнутому прямоугольнику, повёрнутая
// кнопка выглядела так: рисунок наклонён, а рамка вокруг него стоит прямо, и
// ручка «правый край» тянет не ту сторону — потому что «правый край» у
// повёрнутого элемента не справа.
//
// Угол берётся из самого элемента (Element::Rotation), и поворот здесь —
// ровно тот же, что делает отрисовка (UIRenderer::PushRotation вокруг центра
// прямоугольника). Две разные формулы поворота однажды разойдутся, и рамка
// поедет от интерфейса тем сильнее, чем больше угол.
// Сама формула — в движке (sage::ui::RotatePoint): её проверяют тесты, и она
// же считает углы рамки. Здесь только перевод ImVec2 <-> glm::vec2.
ImVec2 RotateAround(ImVec2 p, ImVec2 centre, float angleDeg) {
    const glm::vec2 r = sage::ui::RotatePoint({p.x, p.y}, {centre.x, centre.y}, angleDeg);
    return ImVec2(r.x, r.y);
}

bool Overlaps(const UIRect& a, const UIRect& b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

} // namespace

void UICanvas::Collect(Scene& scene, EditorHost& host, int frameW, int frameH) {
    m_items.clear();
    entt::registry& reg = scene.Registry();
    // Прямоугольники берём У САМОЙ СИСТЕМЫ ИНТЕРФЕЙСА, а не считаем своей
    // копией формул: копия отстаёт (см. sage::ui::SolveSceneRects). Именно так
    // и разъехались рамки с интерфейсом, когда раскладка научилась масштабу
    // холста, а редактор — нет.
    for (const sage::ui::ElementRect& e :
         sage::ui::SolveSceneRects(scene, frameW, frameH, /*includeHidden=*/true)) {
        const IdComponent* id = reg.try_get<IdComponent>(e.Entity);
        if (!id) continue;
        Item it;
        it.Entity = e.Entity;
        it.Id = id->Id;
        it.Rect = e.Rect;
        it.Parent = e.Parent;
        it.Scale = e.Scale;
        it.InLayout = e.InLayout;
        it.Visible = e.Visible;
        it.Selected = host.Selection().Contains(it.Id);
        m_items.push_back(it);
    }
}

void UICanvas::DrawGrid(ImDrawList* dl, ImVec2 imgPos, ImVec2 imgSize,
                        const UIToolSettings& tools, float sx, float sy) const {
    const float step = tools.Snap.GridStep;
    if (step <= 0.0f) return;
    // Сетку с шагом мельче трёх пикселей на экране рисовать бессмысленно: она
    // сливается в заливку и мешает видеть сам интерфейс. Не ошибка человека —
    // просто масштаб панели такой; поэтому молча загрубляем показ, а притяжка
    // остаётся на своём шаге.
    float showStep = step;
    while (showStep * sx < 4.0f) showStep *= 2.0f;

    const ImU32 thin = IM_COL32(255, 255, 255, 18);
    const ImU32 thick = IM_COL32(255, 255, 255, 40);
    // Каждая пятая линия ярче: считать «сколько тут клеток» по одинаковым
    // линиям невозможно, а по пятёркам — с одного взгляда.
    int index = 0;
    for (float x = 0.0f; x * sx <= imgSize.x; x += showStep, ++index) {
        const float px = imgPos.x + x * sx;
        dl->AddLine(ImVec2(px, imgPos.y), ImVec2(px, imgPos.y + imgSize.y),
                    (index % 5 == 0) ? thick : thin, 1.0f);
    }
    index = 0;
    for (float y = 0.0f; y * sy <= imgSize.y; y += showStep, ++index) {
        const float py = imgPos.y + y * sy;
        dl->AddLine(ImVec2(imgPos.x, py), ImVec2(imgPos.x + imgSize.x, py),
                    (index % 5 == 0) ? thick : thin, 1.0f);
    }
}

void UICanvas::Apply(Scene& scene, const Item& item, glm::vec2 topLeft, glm::vec2 size,
                     bool sizeChanged) {
    entt::registry& reg = scene.Registry();
    sage::ui::Element* u = reg.try_get<sage::ui::Element>(item.Entity);
    if (!u) return;
    // Мышь работает в ПИКСЕЛЯХ ЭКРАНА, а Offset и Size хранятся в ОПОРНЫХ
    // единицах холста. Обратный ход — деление на масштаб холста; без него
    // вёрстка на экране, отличном от опорного, уезжала бы на каждом
    // перетаскивании (и тем сильнее, чем дальше масштаб от единицы).
    const float k = item.Scale > 0.0f ? item.Scale : 1.0f;
    const glm::vec2 tl = topLeft / k;
    const glm::vec2 sz = size / k;
    const UIRect parent{item.Parent.x / k, item.Parent.y / k, item.Parent.w / k,
                        item.Parent.h / k};
    if (sizeChanged) u->Size = sz;
    u->Position = sage::ui::OffsetForTopLeft(u->Anchor, tl, sz, parent);
}

void UICanvas::MoveSelection(EditorHost& host, Scene& scene, glm::vec2 delta) {
    (void)host;
    if (delta.x == 0.0f && delta.y == 0.0f) return;
    for (const Item& it : m_items) {
        if (!it.Selected || it.InLayout) continue;
        Apply(scene, it, glm::vec2(it.Rect.x + delta.x, it.Rect.y + delta.y),
              glm::vec2(it.Rect.w, it.Rect.h), false);
    }
}

sage::ui::UIRect UICanvas::ContentBounds(int frameW, int frameH) const {
    // Экран плюс всё, что за него вышло. Нужен окну для «вписать»: элемент,
    // уехавший за границу, иначе не найти — его не видно, значит не выделить.
    sage::ui::UIRect all{0.0f, 0.0f, (float)frameW, (float)frameH};
    for (const Item& it : m_items) {
        const float x0 = std::min(all.x, it.Rect.x);
        const float y0 = std::min(all.y, it.Rect.y);
        const float x1 = std::max(all.x + all.w, it.Rect.x + it.Rect.w);
        const float y1 = std::max(all.y + all.h, it.Rect.y + it.Rect.h);
        all = sage::ui::UIRect{x0, y0, x1 - x0, y1 - y0};
    }
    return all;
}

// ЯКОРЬ МЕНЯЮТ НА ХОЛСТЕ, А НЕ СПИСКОМ В ИНСПЕКТОРЕ.
//
// Якорь — это место на родителе, ОТ КОТОРОГО считается положение элемента, и
// список из девяти слов заставляет каждый раз переводить «снизу справа» в
// место на экране. Девять точек по краям родителя говорят то же самое прямо:
// щёлкнул в угол — элемент держится за угол.
//
// ЭЛЕМЕНТ ПРИ ЭТОМ НЕ ДВИГАЕТСЯ. Смена якоря без пересчёта отступа — это
// прыжок через весь экран, и выглядит он как поломка: человек просил
// «держись за правый край», а получил «уехал за правый край». Положение
// пересчитывается через OffsetForTopLeft, то есть новый отступ берётся от
// нового якоря к тому же самому месту.
bool UICanvas::DrawAnchorHandles(EditorHost& host, Scene& scene, ImDrawList* dl,
                                 const Item& primary,
                                 const std::function<ImVec2(float, float)>& toScreen, ImVec2 mouse,
                                 bool hovered) {
    entt::registry& reg = scene.Registry();
    sage::ui::Element* u = reg.try_get<sage::ui::Element>(primary.Entity);
    if (!u) return false;

    const UIRect& pr = primary.Parent;
    bool changed = false;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            const UIAnchor anchor = (UIAnchor)(row * 3 + col);
            const float x = pr.x + pr.w * (0.5f * (float)col);
            const float y = pr.y + pr.h * (0.5f * (float)row);
            const ImVec2 p = toScreen(x, y);
            const bool active = u->Anchor == anchor;
            const bool hot = hovered && std::fabs(mouse.x - p.x) <= kHandleGrab &&
                             std::fabs(mouse.y - p.y) <= kHandleGrab;

            // Ромб, а не квадрат: ручки размера уже квадратные, и второй
            // квадрат рядом читался бы как ещё одна ручка размера.
            const float r = active ? 6.0f : (hot ? 6.0f : 4.0f);
            const ImU32 col32 = active ? IM_COL32(255, 170, 60, 255)
                                : hot  ? IM_COL32(255, 220, 160, 255)
                                       : IM_COL32(180, 190, 210, 150);
            const ImVec2 quad[4] = {{p.x, p.y - r}, {p.x + r, p.y}, {p.x, p.y + r}, {p.x - r, p.y}};
            dl->AddConvexPolyFilled(quad, 4, col32);
            dl->AddPolyline(quad, 4, IM_COL32(25, 25, 30, 200), ImDrawFlags_Closed, 1.0f);

            if (hot && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !active) {
                host.PushUndoSnapshot();
                const float k = primary.Scale > 0.0f ? primary.Scale : 1.0f;
                const UIRect parentUnits{pr.x / k, pr.y / k, pr.w / k, pr.h / k};
                u->Anchor = anchor;
                u->Position = sage::ui::OffsetForTopLeft(
                    anchor, glm::vec2(primary.Rect.x / k, primary.Rect.y / k),
                    glm::vec2(primary.Rect.w / k, primary.Rect.h / k), parentUnits);
                changed = true;
            }
            if (hot) ImGui::SetTooltip("%s", T("Anchor: what the element holds on to"));
        }
    }
    return changed;
}

void UICanvas::Draw(EditorHost& host, ImDrawList* dl, ImVec2 imgPos, ImVec2 imgSize, int frameW,
                    int frameH, bool hovered) {
    if (!dl || frameW <= 0 || frameH <= 0) return;
    Scene& scene = host.CurrentScene();
    entt::registry& reg = scene.Registry();
    UIToolSettings& tools = host.Tools().UI;

    // --- Где на экране лежит холст ------------------------------------------
    //
    // ОДНО преобразование, и оно здесь: прямоугольник imgPos/imgSize — это
    // ровно то место, куда панель вывела игровой кадр, а frameW/frameH — размер
    // этого кадра в пикселях игры. Масштаб показа и панорама уже учтены в
    // imgSize/imgPos самой панелью.
    //
    // Раньше формула была записана ДВАЖДЫ: здесь и в отрисовке интерфейса
    // (EditorSceneRenderer масштабировал холст сам), и обе половины надо было
    // держать в согласии — разойдись они на пиксель, мышь ловила бы не то, что
    // видно. Холст переехал в своё окно, кадр рисуется один к одному, и второй
    // половины больше нет.
    const int screenW = frameW, screenH = frameH;
    const float sx = imgSize.x / (float)frameW;
    const float sy = imgSize.y / (float)frameH;
    auto toScreen = [&](float x, float y) {
        return ImVec2(imgPos.x + x * sx, imgPos.y + y * sy);
    };
    auto toUI = [&](ImVec2 p) {
        return glm::vec2((p.x - imgPos.x) / std::max(sx, 1e-6f),
                         (p.y - imgPos.y) / std::max(sy, 1e-6f));
    };

    // Панель «Вёрстка» считает выравнивание в ТЕХ ЖЕ пикселях (см. UIToolSettings).
    tools.FrameSize = glm::vec2((float)screenW, (float)screenH);

    Collect(scene, host, screenW, screenH);

    // --- Границы холста ------------------------------------------------------
    //
    // Экран игры — это ПРЯМОУГОЛЬНИК, и его надо видеть. Пока холст занимал
    // весь вьюпорт, границы не было вовсе: непонятно, что попадёт в кадр, а что
    // уже за ним. Всё, что снаружи, приглушено, а сама граница — сплошная линия.
    const ImVec2 canvasA = toScreen(0.0f, 0.0f);
    const ImVec2 canvasB = toScreen((float)screenW, (float)screenH);

    if (tools.ShowGrid) DrawGrid(dl, canvasA, ImVec2(canvasB.x - canvasA.x, canvasB.y - canvasA.y),
                                 tools, sx, sy);
    dl->AddRect(canvasA, canvasB, IM_COL32(255, 255, 255, 120), 0.0f, 0, 1.0f);

    // --- Безопасная область --------------------------------------------------
    //
    // Пунктир внутри кадра: всё за ним может быть срезано вырезом камеры,
    // скруглением корпуса или «overscan» телевизора. Рисуется ПОД элементами,
    // а не поверх: это разметка холста, и закрывать ею то, что верстают, — то
    // же самое, что верстать по линейке, положенной на чертёж.
    if (tools.ShowSafeArea) {
        const float m = std::clamp(tools.SafeAreaPercent, 0.0f, 25.0f) * 0.01f;
        const ImVec2 sa = toScreen((float)screenW * m, (float)screenH * m);
        const ImVec2 sb = toScreen((float)screenW * (1.0f - m), (float)screenH * (1.0f - m));
        const ImU32 col = IM_COL32(255, 210, 90, 150);
        // Пунктир вручную: ImGui рисует только сплошные линии, а сплошная
        // жёлтая рамка внутри кадра читается как ещё один элемент.
        auto dash = [&](ImVec2 a, ImVec2 b) {
            const float len = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
            if (len < 1.0f) return;
            const float dx = (b.x - a.x) / len, dy = (b.y - a.y) / len;
            for (float t = 0.0f; t < len; t += 10.0f) {
                const float e = std::min(t + 5.0f, len);
                dl->AddLine(ImVec2(a.x + dx * t, a.y + dy * t),
                            ImVec2(a.x + dx * e, a.y + dy * e), col, 1.0f);
            }
        };
        dash(ImVec2(sa.x, sa.y), ImVec2(sb.x, sa.y));
        dash(ImVec2(sb.x, sa.y), ImVec2(sb.x, sb.y));
        dash(ImVec2(sb.x, sb.y), ImVec2(sa.x, sb.y));
        dash(ImVec2(sa.x, sb.y), ImVec2(sa.x, sa.y));
    }

    const int selectedId = host.Selection().Primary();
    const ImVec2 mouse = ImGui::GetMousePos();

    // --- Рамки элементов + ручки у первичного выделенного --------------------
    struct Handle { ImVec2 Pos; Drag Kind; };
    Handle handles[8];
    int handleCount = 0;
    UIRect primaryRect{};
    UIRect primaryParent{};
    bool havePrimary = false;
    ImVec2 rotateHandle{0.0f, 0.0f};
    bool haveRotate = false;
    const Item* primaryItem = nullptr;

    for (const Item& it : m_items) {
        const ImVec2 a = toScreen(it.Rect.x, it.Rect.y);
        const ImVec2 b = toScreen(it.Rect.x + it.Rect.w, it.Rect.y + it.Rect.h);
        const bool primary = (it.Id == selectedId);
        const sage::ui::Element& u = reg.get<sage::ui::Element>(it.Entity);

        if (!it.Selected && !tools.ShowAllOutlines) continue;

        // Невидимые элементы показываются приглушённо, а не прячутся:
        // «элемента нет» и «элемент выключен» — разные вещи, и второе надо
        // видеть, иначе выключенный элемент невозможно найти и включить обратно.
        // Элемент, целиком вышедший за экран, помечается ОТДЕЛЬНЫМ цветом: он
        // существует, но в игре его не увидят. Раньше он просто пропадал, и
        // единственным следом оставалась строка в иерархии.
        const bool offscreen = it.Rect.x + it.Rect.w <= 0.0f || it.Rect.y + it.Rect.h <= 0.0f ||
                               it.Rect.x >= (float)screenW || it.Rect.y >= (float)screenH;
        ImU32 col = primary   ? IM_COL32(255, 170, 60, 255)
                    : it.Selected ? IM_COL32(255, 200, 120, 190)
                    : offscreen   ? IM_COL32(255, 90, 90, 170)
                    : it.Visible  ? IM_COL32(120, 190, 255, 110)
                                  : IM_COL32(150, 150, 160, 70);
        // Рамка рисуется ЧЕТЫРЬМЯ УГЛАМИ, а не AddRect: у повёрнутого элемента
        // прямоугольник по осям экрана — это не он, а коробка вокруг него.
        const ImVec2 centre((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
        const ImVec2 corner[4] = {RotateAround(ImVec2(a.x, a.y), centre, u.Rotation),
                                  RotateAround(ImVec2(b.x, a.y), centre, u.Rotation),
                                  RotateAround(ImVec2(b.x, b.y), centre, u.Rotation),
                                  RotateAround(ImVec2(a.x, b.y), centre, u.Rotation)};
        dl->AddPolyline(corner, 4, col, ImDrawFlags_Closed, it.Selected ? 2.0f : 1.0f);
        if (offscreen) {
            dl->AddLine(corner[0], corner[2], IM_COL32(255, 90, 90, 110), 1.0f);
            dl->AddLine(corner[1], corner[3], IM_COL32(255, 90, 90, 110), 1.0f);
        }

        if (!primary) continue;
        havePrimary = true;
        primaryRect = it.Rect;
        primaryParent = it.Parent;
        primaryItem = &it;

        const float mx = (a.x + b.x) * 0.5f, my = (a.y + b.y) * 0.5f;
        // Ручка поворота едет вместе с верхним краем: у элемента, повёрнутого
        // на 90°, «над верхним краем» — это сбоку, и оставлять её висеть над
        // экранным верхом значит показывать ручку не от того элемента.
        rotateHandle = RotateAround(ImVec2(mx, a.y - kRotateArm), centre, u.Rotation);
        haveRotate = true;
        // ПОВОРОТ — ОТДЕЛЬНОЙ РУЧКОЙ НАД ВЕРХНИМ КРАЕМ, на ножке. Ставить его
        // в угол, как делают некоторые редакторы, нельзя: в углу уже сидит
        // ручка размера, и «повернуть» превращается в лотерею с попаданием
        // мышью. Кружок на ножке ни с чем не спутать.
        dl->AddLine(RotateAround(ImVec2(mx, a.y), centre, u.Rotation), rotateHandle,
                    IM_COL32(255, 200, 110, 180), 1.0f);
        dl->AddCircleFilled(rotateHandle, kHandle + 1.0f, IM_COL32(255, 200, 110, 255), 12);
        dl->AddCircle(rotateHandle, kHandle + 1.0f, IM_COL32(40, 30, 10, 200), 12, 1.0f);

        const Handle hs[8] = {
            {RotateAround(ImVec2(a.x, a.y), centre, u.Rotation), Drag::NW},
            {RotateAround(ImVec2(mx, a.y), centre, u.Rotation), Drag::N},
            {RotateAround(ImVec2(b.x, a.y), centre, u.Rotation), Drag::NE},
            {RotateAround(ImVec2(b.x, my), centre, u.Rotation), Drag::E},
            {RotateAround(ImVec2(b.x, b.y), centre, u.Rotation), Drag::SE},
            {RotateAround(ImVec2(mx, b.y), centre, u.Rotation), Drag::S},
            {RotateAround(ImVec2(a.x, b.y), centre, u.Rotation), Drag::SW},
            {RotateAround(ImVec2(a.x, my), centre, u.Rotation), Drag::W}};
        for (const Handle& h : hs) handles[handleCount++] = h;
        // Квадратики ручек ТОЖЕ наклонены: прямые квадраты на наклонённой рамке
        // читаются как чужие, прилетевшие от другого элемента.
        for (const Handle& h : hs) {
            const ImVec2 q[4] = {
                RotateAround(ImVec2(h.Pos.x - kHandle, h.Pos.y - kHandle), h.Pos, u.Rotation),
                RotateAround(ImVec2(h.Pos.x + kHandle, h.Pos.y - kHandle), h.Pos, u.Rotation),
                RotateAround(ImVec2(h.Pos.x + kHandle, h.Pos.y + kHandle), h.Pos, u.Rotation),
                RotateAround(ImVec2(h.Pos.x - kHandle, h.Pos.y + kHandle), h.Pos, u.Rotation)};
            dl->AddConvexPolyFilled(q, 4, IM_COL32(255, 200, 110, 255));
            dl->AddPolyline(q, 4, IM_COL32(40, 30, 10, 200), ImDrawFlags_Closed, 1.0f);
        }

        // Точка якоря и линия до неё: без этого непонятно, ОТ ЧЕГО считается
        // положение, и элемент «уезжает» при смене разрешения неожиданно.
        const UIRect& pr = it.Parent;
        float ax = pr.x, ay = pr.y;
        switch (u.Anchor) {
            case UIAnchor::TopCenter: case UIAnchor::Center: case UIAnchor::BottomCenter:
                ax = pr.x + pr.w * 0.5f; break;
            case UIAnchor::TopRight: case UIAnchor::CenterRight: case UIAnchor::BottomRight:
                ax = pr.x + pr.w; break;
            default: break;
        }
        switch (u.Anchor) {
            case UIAnchor::CenterLeft: case UIAnchor::Center: case UIAnchor::CenterRight:
                ay = pr.y + pr.h * 0.5f; break;
            case UIAnchor::BottomLeft: case UIAnchor::BottomCenter: case UIAnchor::BottomRight:
                ay = pr.y + pr.h; break;
            default: break;
        }
        const ImVec2 anchorPt = toScreen(ax, ay);
        dl->AddLine(anchorPt, ImVec2(mx, my), IM_COL32(255, 170, 60, 120), 1.0f);
        dl->AddCircleFilled(anchorPt, 4.0f, IM_COL32(255, 170, 60, 220), 8);

        // Размер числом рядом с рамкой: верстают по числам, и лезть за ними
        // в инспектор посреди перетаскивания — лишний разрыв внимания.
        char label[64];
        // Угол дописывается ТОЛЬКО когда он не нулевой: строка «200 x 56, 0°»
        // у каждого второго элемента — это шум, из-за которого перестают
        // читать и размер.
        if (std::fabs(u.Rotation) > 0.01f)
            std::snprintf(label, sizeof(label), "%.0f x %.0f, %.0f\xc2\xb0", it.Rect.w, it.Rect.h,
                          u.Rotation);
        else
            std::snprintf(label, sizeof(label), "%.0f x %.0f", it.Rect.w, it.Rect.h);
        dl->AddText(ImVec2(a.x, a.y - ImGui::GetTextLineHeight() - 2.0f),
                    IM_COL32(255, 200, 110, 230), label);
    }

    // --- Отступы до краёв родителя ------------------------------------------
    //
    // Показываются, пока элемент тянут: верстают «16 от края», а не «x = 1264»,
    // и считать этот отступ в уме из двух чисел инспектора — лишняя работа
    // ровно в тот момент, когда руки заняты мышью.
    if (havePrimary && m_drag != Drag::None && tools.ShowDistances) {
        const UIRect& r = primaryRect;
        const UIRect& p = primaryParent;
        struct Line { ImVec2 A, B; float Value; };
        const float cy = r.y + r.h * 0.5f, cx = r.x + r.w * 0.5f;
        const Line lines[4] = {
            {toScreen(p.x, cy), toScreen(r.x, cy), r.x - p.x},
            {toScreen(r.x + r.w, cy), toScreen(p.x + p.w, cy), (p.x + p.w) - (r.x + r.w)},
            {toScreen(cx, p.y), toScreen(cx, r.y), r.y - p.y},
            {toScreen(cx, r.y + r.h), toScreen(cx, p.y + p.h), (p.y + p.h) - (r.y + r.h)},
        };
        for (const Line& l : lines) {
            dl->AddLine(l.A, l.B, IM_COL32(120, 230, 180, 160), 1.0f);
            char t[32];
            std::snprintf(t, sizeof(t), "%.0f", l.Value);
            dl->AddText(ImVec2((l.A.x + l.B.x) * 0.5f + 2.0f, (l.A.y + l.B.y) * 0.5f - 14.0f),
                        IM_COL32(150, 240, 200, 230), t);
        }
    }

    // --- Направляющие привязки ----------------------------------------------
    for (const sage::ui::SnapGuide& g : m_guides) {
        const ImU32 col = g.From == sage::ui::SnapGuide::Source::Parent
                              ? IM_COL32(120, 220, 255, 220)
                              : IM_COL32(255, 90, 200, 230);
        if (g.Along == sage::ui::SnapGuide::Axis::X)
            dl->AddLine(toScreen(g.Position, g.Begin), toScreen(g.Position, g.End), col, 1.0f);
        else
            dl->AddLine(toScreen(g.Begin, g.Position), toScreen(g.End, g.Position), col, 1.0f);
    }
    m_guides.clear();

    // Вид холста (масштаб, панорама, «вписать») — НЕ ЗДЕСЬ. Он принадлежит
    // окну, которое показывает кадр: это свойство показа, а не вёрстки.
    // Раньше холст жил во вьюпорте и масштабировал себя сам, отчего одна и та
    // же формула была записана в двух подсистемах сразу (см. выше).

    // --- Клавиатура: точная доводка -----------------------------------------
    //
    // Мышью попасть в пиксель нельзя в принципе, а именно пиксель и решает,
    // ровно ли стоит подпись в кнопке. Shift — шаг сетки: так двигают целыми
    // клетками, не считая нажатия.
    if (hovered && !ImGui::GetIO().WantTextInput && !host.Selection().Empty()) {
        const float step = ImGui::GetIO().KeyShift ? std::max(1.0f, tools.Snap.GridStep) : 1.0f;
        glm::vec2 nudge(0.0f);
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) nudge.x -= step;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) nudge.x += step;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) nudge.y -= step;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) nudge.y += step;
        if (nudge.x != 0.0f || nudge.y != 0.0f) {
            host.PushUndoSnapshot();
            MoveSelection(host, scene, nudge);
            return;
        }
    }

    // --- Рамка выделения -----------------------------------------------------
    if (m_marquee) {
        const ImVec2 a(std::min(m_marqueeStart.x, mouse.x), std::min(m_marqueeStart.y, mouse.y));
        const ImVec2 b(std::max(m_marqueeStart.x, mouse.x), std::max(m_marqueeStart.y, mouse.y));
        dl->AddRectFilled(a, b, IM_COL32(120, 190, 255, 30));
        dl->AddRect(a, b, IM_COL32(160, 210, 255, 200));
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            m_marquee = false;
            const glm::vec2 ua = toUI(a), ub = toUI(b);
            const UIRect box{ua.x, ua.y, ub.x - ua.x, ub.y - ua.y};
            // Задел — значит выбран. Требовать полного накрытия неудобно:
            // растянутые на весь экран панели тогда не выбрать вовсе.
            bool first = true;
            for (const Item& it : m_items) {
                if (!it.Visible || !Overlaps(it.Rect, box)) continue;
                if (first) { host.Selection().SetPrimary(it.Id); first = false; }
                else if (!host.Selection().Contains(it.Id)) host.Selection().Toggle(it.Id);
            }
            if (first) host.Selection().SetPrimary(-1);
        }
        return;
    }

    // --- Ручки якоря ---------------------------------------------------------
    //
    // ПОСЛЕ рамок и ДО разбора щелчка: точки лежат на краях родителя, то есть
    // часто поверх соседних элементов, и щелчок по точке обязан выигрывать у
    // щелчка по тому, что под ней.
    if (havePrimary && primaryItem && tools.EditAnchors && m_drag == Drag::None) {
        if (DrawAnchorHandles(host, scene, dl, *primaryItem, toScreen, mouse, hovered)) return;
    }

    // --- Начало действия -----------------------------------------------------
    if (m_drag == Drag::None) {
        if (!hovered) return;
        if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return;

        // Ручка поворота — первой: она висит НАД элементом, вне его
        // прямоугольника, и под ней обычно пусто, но попасть в неё должно быть
        // надёжно.
        if (haveRotate && std::fabs(mouse.x - rotateHandle.x) <= kHandleGrab + 2.0f &&
            std::fabs(mouse.y - rotateHandle.y) <= kHandleGrab + 2.0f) {
            m_drag = Drag::Rotate;
            m_dragId = selectedId;
            m_dragStartMouse = mouse;
            m_dragStartRect = primaryRect;
            m_dragStartUnion = primaryRect;
            m_dragStart.clear();
            m_pushedUndo = false;
            m_rotateStart = primaryItem ? reg.get<sage::ui::Element>(primaryItem->Entity).Rotation
                                        : 0.0f;
            const ImVec2 c = toScreen(primaryRect.x + primaryRect.w * 0.5f,
                                      primaryRect.y + primaryRect.h * 0.5f);
            m_rotateGrab = std::atan2(mouse.y - c.y, mouse.x - c.x);
            return;
        }

        // Сначала ручки первичного: они лежат ПОВЕРХ соседних элементов, и
        // попадание по ручке должно выигрывать у попадания по тому, что под ней.
        for (int i = 0; i < handleCount; ++i) {
            const ImVec2 d(mouse.x - handles[i].Pos.x, mouse.y - handles[i].Pos.y);
            if (std::fabs(d.x) > kHandleGrab || std::fabs(d.y) > kHandleGrab) continue;
            m_drag = handles[i].Kind;
            m_dragId = selectedId;
            m_dragStartMouse = mouse;
            m_dragStartRect = primaryRect;
            m_dragStartUnion = primaryRect;
            m_dragStart.clear();
            m_pushedUndo = false;
            return;
        }

        // Иначе — элемент под курсором тем же попаданием, каким его считает
        // игра: одно правило на редактор и на рантайм.
        const glm::vec2 ui = toUI(mouse);
        const int hit = sage::ui::HitTest(scene, ui.x, ui.y, screenW, screenH);
        if (hit < 0) {
            // Пустое место — рамка выделения. Прежде здесь просто сбрасывался
            // выбор, и выбрать десяток элементов можно было только кликами.
            m_marquee = true;
            m_marqueeStart = mouse;
            return;
        }

        // Ctrl — добавить/убрать из набора, как везде в редакторе.
        if (ImGui::GetIO().KeyCtrl) {
            host.Selection().Toggle(hit);
            return;
        }
        // Клик по тому, что уже в наборе, набор НЕ сбрасывает: иначе набор
        // нельзя было бы утащить целиком — первый же клик оставлял бы один.
        if (!host.Selection().Contains(hit)) host.Selection().SetPrimary(hit);

        m_drag = Drag::Move;
        m_dragId = hit;
        m_dragStartMouse = mouse;
        m_dragStart.clear();
        std::vector<UIRect> selected;
        for (const Item& it : m_items) {
            if (!host.Selection().Contains(it.Id)) continue;
            m_dragStart.emplace_back(it.Id, it.Rect);
            selected.push_back(it.Rect);
            if (it.Id == hit) m_dragStartRect = it.Rect;
        }
        m_dragStartUnion = sage::ui::Union(selected);
        m_pushedUndo = false;
        return;
    }

    // --- Перетаскивание ------------------------------------------------------
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        m_drag = Drag::None;
        m_dragId = -1;
        m_dragStart.clear();
        return;
    }
    GameObject obj = scene.Get(m_dragId);
    if (!obj.Valid()) { m_drag = Drag::None; return; }
    sage::ui::Element* u = reg.try_get<sage::ui::Element>(obj.Entity());
    if (!u) { m_drag = Drag::None; return; }

    // Одна запись undo на всё перетаскивание, и берётся она в первый кадр
    // движения: снимок ПОСЛЕ первого изменения уже бесполезен.
    if (!m_pushedUndo) {
        host.PushUndoSnapshot();
        m_pushedUndo = true;
    }

    // --- Поворот -------------------------------------------------------------
    //
    // Угол считается от ЦЕНТРА элемента к мыши и прибавляется к тому, что было
    // на момент нажатия: так ручка не «прыгает» под курсор в первый же кадр.
    if (m_drag == Drag::Rotate) {
        const ImVec2 c = toScreen(m_dragStartRect.x + m_dragStartRect.w * 0.5f,
                                  m_dragStartRect.y + m_dragStartRect.h * 0.5f);
        const float now = std::atan2(mouse.y - c.y, mouse.x - c.x);
        float deg = m_rotateStart + (now - m_rotateGrab) * 57.2957795f;
        // Shift — по 15°: ровные углы (45°, 90°) мышью не поймать, а именно они
        // и нужны чаще всего. Alt снимает притяжку, как и у перемещения.
        if (ImGui::GetIO().KeyShift && !ImGui::GetIO().KeyAlt)
            deg = std::round(deg / 15.0f) * 15.0f;
        // Приводим к (-180, 180]: 370° и 10° — один и тот же поворот, но в поле
        // инспектора первое читается как ошибка.
        while (deg > 180.0f) deg -= 360.0f;
        while (deg <= -180.0f) deg += 360.0f;
        u->Rotation = deg;
        return;
    }

    const glm::vec2 startUI = toUI(m_dragStartMouse);
    const glm::vec2 nowUI = toUI(mouse);
    glm::vec2 d = nowUI - startUI;
    // Shift — движение по одной оси: выровнять элемент по соседу мышью иначе
    // невозможно, всегда съезжает на пиксель по второй координате.
    if (ImGui::GetIO().KeyShift && m_drag == Drag::Move) {
        if (std::fabs(d.x) > std::fabs(d.y)) d.y = 0.0f;
        else d.x = 0.0f;
    }

    // ТЯНЕМ ПО ОСЯМ ЭЛЕМЕНТА, А НЕ ЭКРАНА. Прямоугольник хранится в раскладке
    // без поворота — угол применяется при отрисовке, вокруг центра, — поэтому
    // сдвиг мыши надо перевести в систему координат самого элемента. Без этого
    // у наклонённой кнопки ручка «правый край» тянет вбок и вверх сразу:
    // человек ведёт вдоль её края, а растёт она по экранному X.
    if (m_drag != Drag::Move && std::fabs(u->Rotation) > 0.01f) {
        d = sage::ui::UnrotateDelta(d, u->Rotation);
    }

    // Какие рёбра ведёт мышь — от этого зависит, что притягивать.
    sage::ui::SnapEdges edges;
    switch (m_drag) {
        case Drag::Move: edges = sage::ui::SnapEdges::Whole(); break;
        case Drag::N:  edges.Top = true; break;
        case Drag::S:  edges.Bottom = true; break;
        case Drag::W:  edges.Left = true; break;
        case Drag::E:  edges.Right = true; break;
        case Drag::NW: edges.Top = true; edges.Left = true; break;
        case Drag::NE: edges.Top = true; edges.Right = true; break;
        case Drag::SW: edges.Bottom = true; edges.Left = true; break;
        case Drag::SE: edges.Bottom = true; edges.Right = true; break;
        default: break;
    }

    // Alt отключает привязку на время — без этого поставить элемент НЕ на
    // круглое число и не в ряд с соседом невозможно, а иногда надо именно так.
    const bool snapOff = ImGui::GetIO().KeyAlt;
    if (!snapOff) {
        // Соседи — все элементы, которых не двигают. Двигаемый в списке
        // означал бы притяжку к самому себе, то есть намертво прилипший
        // элемент.
        std::vector<UIRect> siblings;
        siblings.reserve(m_items.size());
        for (const Item& it : m_items) {
            if (it.Id == m_dragId) continue;
            if (m_drag == Drag::Move && host.Selection().Contains(it.Id)) continue;
            siblings.push_back(it.Rect);
        }
        // Привязка считается по прямоугольнику НАБОРА (при перемещении) или по
        // самому элементу (при растягивании): набор ведёт себя как одно целое.
        const UIRect base = (m_drag == Drag::Move) ? m_dragStartUnion : m_dragStartRect;
        UIRect free = base;
        if (m_drag == Drag::Move) { free.x += d.x; free.y += d.y; }
        else {
            if (edges.Left)   { free.x += d.x; free.w -= d.x; }
            if (edges.Right)  { free.w += d.x; }
            if (edges.Top)    { free.y += d.y; free.h -= d.y; }
            if (edges.Bottom) { free.h += d.y; }
        }
        const sage::ui::SnapResult snap =
            sage::ui::Snap(free, edges, primaryParent, siblings, tools.Snap);
        d += snap.Delta;
        m_guides = snap.Guides;
    }

    if (m_drag == Drag::Move) {
        // Двигаем ОТ СТАРТОВЫХ прямоугольников: накопление покадровой дельты
        // уводит набор тем сильнее, чем дольше тянут.
        for (const auto& [id, start] : m_dragStart) {
            const Item* item = nullptr;
            for (const Item& it : m_items)
                if (it.Id == id) { item = &it; break; }
            if (!item) continue;
            if (item->InLayout) continue;
            Apply(scene, *item, glm::vec2(start.x + d.x, start.y + d.y),
                  glm::vec2(start.w, start.h), false);
        }
        return;
    }

    // Растягивание — только у первичного элемента.
    glm::vec2 tl(m_dragStartRect.x, m_dragStartRect.y);
    glm::vec2 br(m_dragStartRect.x + m_dragStartRect.w, m_dragStartRect.y + m_dragStartRect.h);
    if (edges.Left) tl.x += d.x;
    if (edges.Right) br.x += d.x;
    if (edges.Top) tl.y += d.y;
    if (edges.Bottom) br.y += d.y;
    // Не даём вывернуть прямоугольник наизнанку: отрицательный размер — это не
    // «зеркально», а сломанная раскладка и невидимый элемент.
    const glm::vec2 newSize(std::max(4.0f, br.x - tl.x), std::max(4.0f, br.y - tl.y));
    for (const Item& it : m_items)
        if (it.Entity == obj.Entity()) { Apply(scene, it, tl, newSize, true); break; }

    // Авто-ширина живёт у надписи (sage::ui::Label): её считает содержимое,
    // и пока она включена, тянуть ширину мышью бессмысленно. Раз человек
    // явно взялся за ручку — снимаем.
    if (sage::ui::Label* label = reg.try_get<sage::ui::Label>(obj.Entity()))
        label->AutoWidth = false;
}
