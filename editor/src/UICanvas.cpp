#include "UICanvas.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "EditorHost.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "sage/ui/UI.h"
#include "sage/ui/UISceneSystem.h"

namespace {

using sage::ui::UIRect;

constexpr float kHandle = 5.0f;      // половина стороны ручки в пикселях панели
constexpr float kHandleGrab = 7.0f;  // радиус захвата — больше рисунка: попасть в
                                     // квадратик 10x10 мышью тяжело, и это чувствуется

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
        it.Selected = host.IsSelected(it.Id);
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
    sage::ui::Transform* u = reg.try_get<sage::ui::Transform>(item.Entity);
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
    u->Offset = sage::ui::OffsetForTopLeft(u->Anchor, tl, sz, parent);
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

void UICanvas::Draw(EditorHost& host, ImDrawList* dl, ImVec2 imgPos, ImVec2 imgSize, int frameW,
                    int frameH, bool hovered) {
    if (!dl || frameW <= 0 || frameH <= 0) return;
    Scene& scene = host.CurrentScene();
    entt::registry& reg = scene.Registry();
    UIToolSettings& tools = host.UITools();

    // --- Где на экране лежит холст ------------------------------------------
    //
    // ПОВТОРЯЕТ РАСЧЁТ ОТРИСОВКИ (EditorSceneRenderer::SetUICanvasView), и это
    // обязано быть так: рамки редактора и сам интерфейс рисуются разными
    // подсистемами, и разойдись они хоть на пиксель — мышь будет ловить не то,
    // что видно. Формула одна, записана дважды и помечена с обеих сторон.
    const float zoom = tools.Zoom > 0.0f ? tools.Zoom : 1.0f;
    // Экран игры не зависит от масштаба показа (см. EditorSceneRenderer).
    const int screenW = frameW, screenH = frameH;
    const glm::vec2 origin(((float)frameW - (float)frameW * zoom) * 0.5f + tools.Pan.x,
                           ((float)frameH - (float)frameH * zoom) * 0.5f + tools.Pan.y);

    // Пиксель кадра -> пиксель панели (кадр может быть показан не один к одному).
    const float sx = imgSize.x / (float)frameW;
    const float sy = imgSize.y / (float)frameH;
    auto toScreen = [&](float x, float y) {
        return ImVec2(imgPos.x + (origin.x + x * zoom) * sx,
                      imgPos.y + (origin.y + y * zoom) * sy);
    };
    auto toUI = [&](ImVec2 p) {
        return glm::vec2(((p.x - imgPos.x) / std::max(sx, 1e-6f) - origin.x) / zoom,
                         ((p.y - imgPos.y) / std::max(sy, 1e-6f) - origin.y) / zoom);
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
    if (zoom < 0.999f) {
        const ImU32 outside = IM_COL32(0, 0, 0, 90);
        const ImVec2 pa = imgPos, pb = ImVec2(imgPos.x + imgSize.x, imgPos.y + imgSize.y);
        dl->AddRectFilled(pa, ImVec2(pb.x, canvasA.y), outside);
        dl->AddRectFilled(ImVec2(pa.x, canvasB.y), pb, outside);
        dl->AddRectFilled(ImVec2(pa.x, canvasA.y), ImVec2(canvasA.x, canvasB.y), outside);
        dl->AddRectFilled(ImVec2(canvasB.x, canvasA.y), ImVec2(pb.x, canvasB.y), outside);
    }

    if (tools.ShowGrid) DrawGrid(dl, canvasA, ImVec2(canvasB.x - canvasA.x, canvasB.y - canvasA.y),
                                 tools, sx * zoom, sy * zoom);
    dl->AddRect(canvasA, canvasB, IM_COL32(255, 255, 255, 120), 0.0f, 0, 1.0f);

    const int selectedId = host.SelectedId();
    const ImVec2 mouse = ImGui::GetMousePos();

    // --- Рамки элементов + ручки у первичного выделенного --------------------
    struct Handle { ImVec2 Pos; Drag Kind; };
    Handle handles[8];
    int handleCount = 0;
    UIRect primaryRect{};
    UIRect primaryParent{};
    bool havePrimary = false;

    for (const Item& it : m_items) {
        const ImVec2 a = toScreen(it.Rect.x, it.Rect.y);
        const ImVec2 b = toScreen(it.Rect.x + it.Rect.w, it.Rect.y + it.Rect.h);
        const bool primary = (it.Id == selectedId);

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
        dl->AddRect(a, b, col, 0.0f, 0, it.Selected ? 2.0f : 1.0f);
        if (offscreen) {
            dl->AddLine(a, b, IM_COL32(255, 90, 90, 110), 1.0f);
            dl->AddLine(ImVec2(b.x, a.y), ImVec2(a.x, b.y), IM_COL32(255, 90, 90, 110), 1.0f);
        }

        if (!primary) continue;
        havePrimary = true;
        primaryRect = it.Rect;
        primaryParent = it.Parent;

        const float mx = (a.x + b.x) * 0.5f, my = (a.y + b.y) * 0.5f;
        const Handle hs[8] = {{{a.x, a.y}, Drag::NW}, {{mx, a.y}, Drag::N},
                              {{b.x, a.y}, Drag::NE}, {{b.x, my}, Drag::E},
                              {{b.x, b.y}, Drag::SE}, {{mx, b.y}, Drag::S},
                              {{a.x, b.y}, Drag::SW}, {{a.x, my}, Drag::W}};
        for (const Handle& h : hs) handles[handleCount++] = h;
        for (const Handle& h : hs) {
            dl->AddRectFilled(ImVec2(h.Pos.x - kHandle, h.Pos.y - kHandle),
                              ImVec2(h.Pos.x + kHandle, h.Pos.y + kHandle),
                              IM_COL32(255, 200, 110, 255), 2.0f);
            dl->AddRect(ImVec2(h.Pos.x - kHandle, h.Pos.y - kHandle),
                        ImVec2(h.Pos.x + kHandle, h.Pos.y + kHandle),
                        IM_COL32(40, 30, 10, 200), 2.0f);
        }

        // Точка якоря и линия до неё: без этого непонятно, ОТ ЧЕГО считается
        // положение, и элемент «уезжает» при смене разрешения неожиданно.
        const sage::ui::Transform& u = reg.get<sage::ui::Transform>(it.Entity);
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

    // --- Вид холста: колесо, панорама, «вписать» ----------------------------
    //
    // Без этого отдалять холст было бы нечем, кроме ползунка в панели, — а
    // отдаляют его именно тогда, когда что-то потерялось, то есть в спешке.
    if (hovered && m_drag == Drag::None && !m_marquee) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            // Приближаем К КУРСОРУ: точка под мышью остаётся на месте. Масштаб
            // «от центра» заставляет после каждого шага догонять панорамой то,
            // на что смотрел.
            const glm::vec2 before = toUI(mouse);
            const float next = std::clamp(zoom * (wheel > 0.0f ? 1.15f : 1.0f / 1.15f), 0.15f, 3.0f);
            tools.Zoom = next;
            // Пересчёт с новым масштабом — теми же формулами, что выше.
            const glm::vec2 base(((float)frameW - (float)frameW * next) * 0.5f,
                                 ((float)frameH - (float)frameH * next) * 0.5f);
            const glm::vec2 mouseFrame((mouse.x - imgPos.x) / std::max(sx, 1e-6f),
                                       (mouse.y - imgPos.y) / std::max(sy, 1e-6f));
            tools.Pan = mouseFrame - base - before * next;
        }
        // Панорама средней кнопкой — как в любом холсте.
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
            const ImVec2 d = ImGui::GetIO().MouseDelta;
            tools.Pan += glm::vec2(d.x / std::max(sx, 1e-6f), d.y / std::max(sy, 1e-6f));
        }
        // Home — вернуть холст на место: один к одному, без сдвига. Клавиша
        // нужна, потому что панорамой можно уехать так, что холста не видно.
        if (ImGui::IsKeyPressed(ImGuiKey_Home)) {
            tools.Zoom = 1.0f;
            tools.Pan = glm::vec2(0.0f);
        }
        // Shift+F — вписать в окно ВСЁ, включая то, что вышло за границы
        // экрана. Это и есть ответ на «элемент уехал, что теперь делать»:
        // одна клавиша показывает его вместе с экраном.
        if (ImGui::IsKeyPressed(ImGuiKey_F) && ImGui::GetIO().KeyShift) {
            UIRect all{0.0f, 0.0f, (float)screenW, (float)screenH};
            for (const Item& it : m_items) {
                const float x0 = std::min(all.x, it.Rect.x);
                const float y0 = std::min(all.y, it.Rect.y);
                const float x1 = std::max(all.x + all.w, it.Rect.x + it.Rect.w);
                const float y1 = std::max(all.y + all.h, it.Rect.y + it.Rect.h);
                all = UIRect{x0, y0, x1 - x0, y1 - y0};
            }
            // Запас по краям, чтобы рамки не упирались в границу панели.
            const float pad = 0.06f;
            const float fit = std::clamp(std::min((float)frameW / (all.w * (1.0f + pad)),
                                                  (float)frameH / (all.h * (1.0f + pad))),
                                         0.15f, 1.0f);
            tools.Zoom = fit;
            const glm::vec2 base(((float)frameW - (float)frameW * fit) * 0.5f,
                                 ((float)frameH - (float)frameH * fit) * 0.5f);
            const glm::vec2 centre(all.x + all.w * 0.5f, all.y + all.h * 0.5f);
            tools.Pan = glm::vec2((float)frameW * 0.5f, (float)frameH * 0.5f) - base - centre * fit;
        }
    }

    // --- Клавиатура: точная доводка -----------------------------------------
    //
    // Мышью попасть в пиксель нельзя в принципе, а именно пиксель и решает,
    // ровно ли стоит подпись в кнопке. Shift — шаг сетки: так двигают целыми
    // клетками, не считая нажатия.
    if (hovered && !ImGui::GetIO().WantTextInput && !host.Selection().empty()) {
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
                if (first) { host.SetSelectedId(it.Id); first = false; }
                else if (!host.IsSelected(it.Id)) host.ToggleSelection(it.Id);
            }
            if (first) host.SetSelectedId(-1);
        }
        return;
    }

    // --- Начало действия -----------------------------------------------------
    if (m_drag == Drag::None) {
        if (!hovered) return;
        if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return;

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
            host.ToggleSelection(hit);
            return;
        }
        // Клик по тому, что уже в наборе, набор НЕ сбрасывает: иначе набор
        // нельзя было бы утащить целиком — первый же клик оставлял бы один.
        if (!host.IsSelected(hit)) host.SetSelectedId(hit);

        m_drag = Drag::Move;
        m_dragId = hit;
        m_dragStartMouse = mouse;
        m_dragStart.clear();
        std::vector<UIRect> selected;
        for (const Item& it : m_items) {
            if (!host.IsSelected(it.Id)) continue;
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
    sage::ui::Transform* u = reg.try_get<sage::ui::Transform>(obj.Entity());
    if (!u) { m_drag = Drag::None; return; }

    // Одна запись undo на всё перетаскивание, и берётся она в первый кадр
    // движения: снимок ПОСЛЕ первого изменения уже бесполезен.
    if (!m_pushedUndo) {
        host.PushUndoSnapshot();
        m_pushedUndo = true;
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
            if (m_drag == Drag::Move && host.IsSelected(it.Id)) continue;
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
