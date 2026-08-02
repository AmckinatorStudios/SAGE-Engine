#include "UICanvas.h"

#include <algorithm>
#include <cmath>

#include "EditorHost.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "sage/ui/UISceneSystem.h"

namespace {

using sage::ui::UIRect;

constexpr float kHandle = 5.0f;      // половина стороны ручки в пикселях панели
constexpr float kHandleGrab = 7.0f;  // радиус захвата — больше рисунка: попасть в
                                     // квадратик 10x10 мышью тяжело, и это чувствуется

// Прямоугольник родителя: либо элемент-родитель, либо весь кадр. Рекурсия по
// иерархии — та же, по которой раскладку считает сама система интерфейса.
UIRect ParentRect(Scene& scene, entt::entity e, const UIRect& screen) {
    entt::registry& reg = scene.Registry();
    const entt::entity parent = scene.ParentOf(e);
    if (parent == entt::null || !reg.valid(parent)) return screen;
    const UIElementComponent* pu = reg.try_get<UIElementComponent>(parent);
    if (!pu) return screen;
    return sage::ui::ResolveElementRect(*pu, ParentRect(scene, parent, screen));
}

} // namespace

void UICanvas::Draw(EditorHost& host, ImDrawList* dl, ImVec2 imgPos, ImVec2 imgSize, int frameW,
                    int frameH, bool hovered) {
    if (!dl || frameW <= 0 || frameH <= 0) return;
    Scene& scene = host.CurrentScene();
    entt::registry& reg = scene.Registry();

    const UIRect screen{0.0f, 0.0f, (float)frameW, (float)frameH};
    const float sx = imgSize.x / (float)frameW;
    const float sy = imgSize.y / (float)frameH;
    auto toScreen = [&](float x, float y) {
        return ImVec2(imgPos.x + x * sx, imgPos.y + y * sy);
    };
    auto toUI = [&](ImVec2 p) {
        return glm::vec2((p.x - imgPos.x) / std::max(sx, 1e-6f),
                         (p.y - imgPos.y) / std::max(sy, 1e-6f));
    };

    const int selectedId = host.SelectedId();
    ImVec2 mouse = ImGui::GetMousePos();

    // --- Рамки всех элементов + ручки у выделенного --------------------------
    struct Handle { ImVec2 Pos; Drag Kind; };
    Handle handles[8];
    int handleCount = 0;
    UIRect selRect{};
    bool haveSel = false;

    auto view = reg.view<UIElementComponent, IdComponent>();
    for (auto e : view) {
        const UIElementComponent& u = view.get<UIElementComponent>(e);
        const int id = view.get<IdComponent>(e).Id;
        const UIRect r = sage::ui::ResolveElementRect(u, ParentRect(scene, e, screen));
        const ImVec2 a = toScreen(r.x, r.y);
        const ImVec2 b = toScreen(r.x + r.w, r.y + r.h);
        const bool sel = (id == selectedId);

        // Невидимые элементы показываются пунктиром-подсказкой, а не прячутся:
        // «элемента нет» и «элемент выключен» — разные вещи, и второе надо
        // видеть, иначе выключенный элемент невозможно найти и включить обратно.
        ImU32 col = sel ? IM_COL32(255, 170, 60, 255)
                        : (u.Visible ? IM_COL32(120, 190, 255, 110)
                                     : IM_COL32(150, 150, 160, 70));
        dl->AddRect(a, b, col, 0.0f, 0, sel ? 2.0f : 1.0f);

        if (sel) {
            haveSel = true;
            selRect = r;
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
            const UIRect pr = ParentRect(scene, e, screen);
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
            dl->AddLine(anchorPt, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f),
                        IM_COL32(255, 170, 60, 120), 1.0f);
            dl->AddCircleFilled(anchorPt, 4.0f, IM_COL32(255, 170, 60, 220), 8);

            // Размер числом рядом с рамкой: верстают по числам, и лезть за ними
            // в инспектор посреди перетаскивания — лишний разрыв внимания.
            char label[64];
            std::snprintf(label, sizeof(label), "%.0f x %.0f", r.w, r.h);
            dl->AddText(ImVec2(a.x, a.y - ImGui::GetTextLineHeight() - 2.0f),
                        IM_COL32(255, 200, 110, 230), label);
        }
    }

    // --- Мышь ---------------------------------------------------------------
    if (m_drag == Drag::None) {
        if (!hovered) return;
        if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return;

        // Сначала ручки выделенного: они лежат ПОВЕРХ соседних элементов, и
        // попадание по ручке должно выигрывать у попадания по тому, что под ней.
        for (int i = 0; i < handleCount; ++i) {
            const ImVec2 d(mouse.x - handles[i].Pos.x, mouse.y - handles[i].Pos.y);
            if (std::fabs(d.x) <= kHandleGrab && std::fabs(d.y) <= kHandleGrab) {
                if (GameObject sel = scene.Get(selectedId); sel.Valid()) {
                    if (UIElementComponent* u = reg.try_get<UIElementComponent>(sel.Entity())) {
                        m_drag = handles[i].Kind;
                        m_dragId = selectedId;
                        m_dragStartMouse = mouse;
                        m_dragStartOffset = ImVec2(u->Offset.x, u->Offset.y);
                        m_dragStartSize = ImVec2(selRect.w, selRect.h);
                        m_pushedUndo = false;
                        return;
                    }
                }
            }
        }

        // Иначе — выбор элемента под курсором тем же попаданием, каким его
        // считает игра: одно правило на редактор и на рантайм.
        const glm::vec2 ui = toUI(mouse);
        const int hit = sage::ui::HitTest(scene, ui.x, ui.y, frameW, frameH);
        if (hit >= 0) {
            host.SetSelectedId(hit);
            if (GameObject sel = scene.Get(hit); sel.Valid()) {
                if (UIElementComponent* u = reg.try_get<UIElementComponent>(sel.Entity())) {
                    m_drag = Drag::Move;
                    m_dragId = hit;
                    m_dragStartMouse = mouse;
                    m_dragStartOffset = ImVec2(u->Offset.x, u->Offset.y);
                    const UIRect r =
                        sage::ui::ResolveElementRect(*u, ParentRect(scene, sel.Entity(), screen));
                    m_dragStartSize = ImVec2(r.w, r.h);
                    m_pushedUndo = false;
                }
            }
        } else {
            host.SetSelectedId(-1);
        }
        return;
    }

    // --- Перетаскивание ------------------------------------------------------
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        m_drag = Drag::None;
        m_dragId = -1;
        return;
    }
    GameObject obj = scene.Get(m_dragId);
    if (!obj.Valid()) { m_drag = Drag::None; return; }
    UIElementComponent* u = reg.try_get<UIElementComponent>(obj.Entity());
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
    if (host.GizmoSnap()) {
        const float step = std::max(1.0f, host.SnapMove());
        d.x = std::round(d.x / step) * step;
        d.y = std::round(d.y / step) * step;
    }

    const UIRect parent = ParentRect(scene, obj.Entity(), screen);
    // Прямоугольник, каким он был в начале перетаскивания.
    const glm::vec2 startSize(m_dragStartSize.x, m_dragStartSize.y);
    const glm::vec2 startTopLeft =
        sage::ui::ResolveAnchored(u->Anchor, glm::vec2(m_dragStartOffset.x, m_dragStartOffset.y),
                                  startSize, parent);

    glm::vec2 tl = startTopLeft;
    glm::vec2 br = startTopLeft + startSize;
    switch (m_drag) {
        case Drag::Move: tl += d; br += d; break;
        case Drag::N:  tl.y += d.y; break;
        case Drag::S:  br.y += d.y; break;
        case Drag::W:  tl.x += d.x; break;
        case Drag::E:  br.x += d.x; break;
        case Drag::NW: tl += d; break;
        case Drag::NE: tl.y += d.y; br.x += d.x; break;
        case Drag::SW: tl.x += d.x; br.y += d.y; break;
        case Drag::SE: br += d; break;
        default: break;
    }
    // Не даём вывернуть прямоугольник наизнанку: отрицательный размер — это не
    // «зеркально», а сломанная раскладка и невидимый элемент.
    const glm::vec2 newSize(std::max(4.0f, br.x - tl.x), std::max(4.0f, br.y - tl.y));

    u->Size = newSize;
    u->Offset = sage::ui::OffsetForTopLeft(u->Anchor, tl, newSize, parent);
    // AutoWidth и ручной размер противоречат друг другу: пока ширину считает
    // содержимое, тянуть её мышью бессмысленно — молча снимаем флаг, раз человек
    // явно взялся за ручку.
    if (m_drag != Drag::Move && u->AutoWidth) u->AutoWidth = false;
}
