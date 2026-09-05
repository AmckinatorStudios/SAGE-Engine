#include "sage/ui/input/UIInput.h"

#include <algorithm>
#include <cmath>

#include "sage/ui/core/UIDocument.h"
#include "sage/ui/core/UINode.h"
#include "sage/ui/input/UIHitTest.h"
#include "sage/ui/input/UIInteraction.h"
#include "sage/ui/visual/UITextLayout.h"
#include "sage/ui/widgets/UIWidgets.h"

namespace sage::ui {

namespace {

UIInteraction* InteractionOf(UIDocument& doc, UINodeId id) {
    UINode* n = doc.Find(id);
    return n ? n->Get<UIInteraction>() : nullptr;
}

void SetFlag(UIInteraction& ia, uint32_t flag, bool on) {
    if (on) ia.Runtime.Flags |= flag;
    else ia.Runtime.Flags &= ~flag;
}

// Клавиши GLFW, которые нужны полю ввода. Числа, а не заголовок GLFW: система
// ввода интерфейса не должна тянуть оконную библиотеку — она получает коды
// снаружи и не обязана знать, кто их прислал.
constexpr int kKeyBackspace = 259;
constexpr int kKeyDelete = 261;
constexpr int kKeyLeft = 263;
constexpr int kKeyRight = 262;
constexpr int kKeyHome = 268;
constexpr int kKeyEnd = 269;
constexpr int kKeyEnter = 257;
constexpr int kKeyEscape = 256;

} // namespace

void UIInputRouter::Reset() {
    m_hovered = m_pressed = m_focused = m_capture = m_dragging = kUIInvalidNode;
    m_clickCount = 0;
    m_lastClickNode = kUIInvalidNode;
    for (bool& b : m_prevButtons) b = false;
}

void UIInputRouter::Emit(UIDocument& doc, UIEventBus& bus, UIEvent& e,
                         const std::vector<UINodeId>& path) const {
    // Три фазы (§51). Без них «модальное окно», «перетаскивание» и «список
    // знает о нажатии на строку» решаются глобальными флагами, а два таких
    // флага уже не уживаются.
    e.Phase = UIEventPhase::Capture;
    for (UINodeId id : path) {
        if (id == e.Target) break;
        e.Current = id;
        bus.Dispatch(e);
        if (e.Handled) return;
    }
    e.Phase = UIEventPhase::Target;
    e.Current = e.Target;
    bus.Dispatch(e);
    if (e.Handled) return;

    e.Phase = UIEventPhase::Bubble;
    for (auto it = path.rbegin(); it != path.rend(); ++it) {
        if (*it == e.Target) continue;
        e.Current = *it;
        bus.Dispatch(e);
        if (e.Handled) return;
    }
    (void)doc;
}

UIInputResult UIInputRouter::Update(UIDocument& doc, const UILayoutSolver& layout,
                                    const UIContext& ctx, const UIInputFrame& input,
                                    UIEventBus& bus) {
    UIInputResult result;

    // Сбросить однокадровые пометки: система ставит, игра читает, следующий шаг
    // гасит. Иначе «нажали» остаётся истиной навсегда.
    for (const UIResolvedNode& r : layout.Nodes()) {
        if (UIInteraction* ia = InteractionOf(doc, r.Id)) {
            ia->Runtime.Clicked = false;
            ia->Runtime.Changed = false;
        }
    }

    const UIHitResult hit = input.PointerInside
                                ? UIHitTest(doc, layout, ctx, input.Pointer)
                                : UIHitResult{};
    // Захват указателя: пока кнопку держат, события идут ей, даже если курсор
    // ушёл в сторону. Без этого ползунок «отваливается» на первом же резком
    // движении мыши.
    const UINodeId target = m_capture != kUIInvalidNode ? m_capture : hit.Node;
    UIHitPath(doc, layout, ctx, input.Pointer, m_path);
    if (m_capture != kUIInvalidNode) {
        m_path.clear();
        const UINode* n = doc.Find(m_capture);
        while (n) { m_path.push_back(n->Id); n = n->Parent == kUIInvalidNode ? nullptr : doc.Find(n->Parent); }
        std::reverse(m_path.begin(), m_path.end());
    }

    auto localOf = [&](UINodeId id) {
        const UIResolvedNode* r = layout.Get(id);
        return r ? input.Pointer - UIPos(r->Rect) : glm::vec2(0.0f);
    };

    auto makeEvent = [&](UIEventType type, UINodeId node) {
        UIEvent e;
        e.Type = type;
        e.Target = node;
        e.Current = node;
        e.Pointer = input.Pointer;
        e.LocalPointer = localOf(node);
        e.Shift = input.Shift;
        e.Ctrl = input.Ctrl;
        e.Alt = input.Alt;
        if (UIInteraction* ia = InteractionOf(doc, node)) e.Command = ia->Command;
        return e;
    };

    // --- Наведение ----------------------------------------------------------
    if (hit.Node != m_hovered) {
        if (m_hovered != kUIInvalidNode) {
            if (UIInteraction* ia = InteractionOf(doc, m_hovered)) {
                SetFlag(*ia, UIState_Hovered, false);
                ia->Runtime.HoverTime = 0.0f;
            }
            UIEvent e = makeEvent(UIEventType::PointerExit, m_hovered);
            std::vector<UINodeId> path;
            const UINode* n = doc.Find(m_hovered);
            while (n) { path.push_back(n->Id); n = n->Parent == kUIInvalidNode ? nullptr : doc.Find(n->Parent); }
            std::reverse(path.begin(), path.end());
            Emit(doc, bus, e, path);
        }
        m_hovered = hit.Node;
        if (m_hovered != kUIInvalidNode) {
            if (UIInteraction* ia = InteractionOf(doc, m_hovered))
                SetFlag(*ia, UIState_Hovered, true);
            UIEvent e = makeEvent(UIEventType::PointerEnter, m_hovered);
            Emit(doc, bus, e, m_path);
        }
    } else if (m_hovered != kUIInvalidNode) {
        if (UIInteraction* ia = InteractionOf(doc, m_hovered))
            ia->Runtime.HoverTime += ctx.DeltaTime;
    }

    // --- Кнопки -------------------------------------------------------------
    for (int b = 0; b < 3; ++b) {
        const bool down = input.Buttons[b];
        const bool was = m_prevButtons[b];
        if (down && !was) {
            if (target != kUIInvalidNode) {
                m_pressed = target;
                m_capture = target;
                m_pressPoint = input.Pointer;
                if (UIInteraction* ia = InteractionOf(doc, target)) {
                    SetFlag(*ia, UIState_Pressed, true);
                    ia->Runtime.PressTime = 0.0f;
                    ia->Runtime.PressPoint = localOf(target);
                    if (ia->Focusable) SetFocus(doc, target, &bus);
                    else ClearFocus(doc, &bus);
                }
                UIEvent e = makeEvent(UIEventType::PointerDown, target);
                e.Button = b;
                Emit(doc, bus, e, m_path);
            } else {
                // Клик мимо интерфейса снимает фокус: поле ввода, которое
                // держит клавиатуру после клика по сцене, ломает управление
                // игрой.
                ClearFocus(doc, &bus);
            }
        } else if (!down && was) {
            const UINodeId released = m_pressed;
            if (released != kUIInvalidNode) {
                if (UIInteraction* ia = InteractionOf(doc, released))
                    SetFlag(*ia, UIState_Pressed, false);
                UIEvent up = makeEvent(UIEventType::PointerUp, released);
                up.Button = b;
                Emit(doc, bus, up, m_path);

                if (m_dragging == released) {
                    UIEvent de = makeEvent(UIEventType::DragEnd, released);
                    Emit(doc, bus, de, m_path);
                    if (UIInteraction* ia = InteractionOf(doc, released))
                        ia->Runtime.Dragging = false;
                    m_dragging = kUIInvalidNode;
                } else if (hit.Node == released && b == 0) {
                    // Щелчок — отпустили ТАМ ЖЕ, где нажали. Иначе «нажал,
                    // передумал, увёл курсор» всё равно считалось бы нажатием.
                    const double now = ctx.Time;
                    m_clickCount = (m_lastClickNode == released && now - m_lastClickTime < 0.35)
                                       ? m_clickCount + 1
                                       : 1;
                    m_lastClickTime = now;
                    m_lastClickNode = released;

                    if (UIInteraction* ia = InteractionOf(doc, released)) {
                        ia->Runtime.Clicked = true;
                        if (!ia->Command.empty()) result.Commands.push_back(ia->Command);
                    }
                    UIEvent click = makeEvent(m_clickCount >= 2 ? UIEventType::DoubleClick
                                                                : UIEventType::Click,
                                              released);
                    click.Clicks = m_clickCount;
                    Emit(doc, bus, click, m_path);
                }
            }
            m_pressed = kUIInvalidNode;
            m_capture = kUIInvalidNode;
        }
        m_prevButtons[b] = down;
    }

    // --- Перемещение и перетаскивание ---------------------------------------
    if (m_pressed != kUIInvalidNode) {
        if (UIInteraction* ia = InteractionOf(doc, m_pressed)) {
            ia->Runtime.PressTime += ctx.DeltaTime;
            const float moved = glm::length(input.Pointer - m_pressPoint);
            // Порог в несколько пикселей: без него любой щелчок дрожащей рукой
            // превращается в перетаскивание, и клик не срабатывает.
            if (ia->Draggable && !ia->Runtime.Dragging && moved > 4.0f) {
                ia->Runtime.Dragging = true;
                m_dragging = m_pressed;
                UIEvent e = makeEvent(UIEventType::DragStart, m_pressed);
                Emit(doc, bus, e, m_path);
            }
            if (ia->Runtime.Dragging) {
                UIEvent e = makeEvent(UIEventType::Drag, m_pressed);
                e.Delta = input.Pointer - m_pressPoint;
                Emit(doc, bus, e, m_path);
            }
        }
        // Ползунок тянется, пока держат кнопку, — это его штатная работа, а не
        // «перетаскивание».
        if (UINode* node = doc.Find(m_pressed)) {
            if (UIRangeValue* range = node->Get<UIRangeValue>()) {
                const UIResolvedNode* r = layout.Get(m_pressed);
                if (r && UIRectValid(r->Rect)) {
                    const float t = range->Vertical
                                        ? 1.0f - (input.Pointer.y - r->Rect.y) / r->Rect.h
                                        : (input.Pointer.x - r->Rect.x) / r->Rect.w;
                    const float before = range->Value;
                    range->SetNormalized(UIClamp01(t));
                    if (range->Value != before) {
                        if (UIInteraction* ia = node->Get<UIInteraction>())
                            ia->Runtime.Changed = true;
                        UIEvent e = makeEvent(UIEventType::ValueChanged, m_pressed);
                        e.Value = range->Value;
                        Emit(doc, bus, e, m_path);
                        doc.MarkDirty(UIDirty_Visual);
                    }
                }
            }
        }
    }

    // --- Прокрутка ----------------------------------------------------------
    if ((input.Scroll.x != 0.0f || input.Scroll.y != 0.0f) && hit.Node != kUIInvalidNode) {
        // Прокрутка ищет ближайший вверх по дереву узел, который её принимает:
        // колесо над строкой списка обязано крутить список, а не строку.
        UINodeId scrollNode = hit.Node;
        while (scrollNode != kUIInvalidNode) {
            UINode* n = doc.Find(scrollNode);
            if (!n) break;
            if (UIScrollView* sv = n->Get<UIScrollView>()) {
                sv->Velocity += glm::vec2(input.Scroll.x, input.Scroll.y) * -sv->Speed;
                doc.MarkDirty(UIDirty_Layout);
                UIEvent e = makeEvent(UIEventType::Scroll, scrollNode);
                e.Delta = input.Scroll;
                Emit(doc, bus, e, m_path);
                break;
            }
            scrollNode = n->Parent;
        }
    }

    // --- Клавиатура ---------------------------------------------------------
    if (m_focused != kUIInvalidNode) {
        UINode* node = doc.Find(m_focused);
        UITextField* field = node ? node->Get<UITextField>() : nullptr;
        for (int key : input.KeysDown) {
            UIEvent e = makeEvent(UIEventType::KeyDown, m_focused);
            e.Key = key;
            Emit(doc, bus, e, m_path);
            if (!field || field->ReadOnly) continue;

            if (key == kKeyBackspace && field->Caret > 0) {
                const int prev = UIUtf8Prev(field->Value, field->Caret);
                field->Value.erase((size_t)prev, (size_t)(field->Caret - prev));
                field->Caret = prev;
                doc.MarkDirty(UIDirty_Text | UIDirty_Layout);
            } else if (key == kKeyDelete && field->Caret < (int)field->Value.size()) {
                int next = field->Caret;
                UIUtf8Next(field->Value, next);
                field->Value.erase((size_t)field->Caret, (size_t)(next - field->Caret));
                doc.MarkDirty(UIDirty_Text | UIDirty_Layout);
            } else if (key == kKeyLeft) {
                field->Caret = UIUtf8Prev(field->Value, field->Caret);
            } else if (key == kKeyRight) {
                int next = field->Caret;
                UIUtf8Next(field->Value, next);
                field->Caret = std::min(next, (int)field->Value.size());
            } else if (key == kKeyHome) {
                field->Caret = 0;
            } else if (key == kKeyEnd) {
                field->Caret = (int)field->Value.size();
            } else if (key == kKeyEnter && !field->Multiline) {
                UIEvent submit = makeEvent(UIEventType::Submit, m_focused);
                Emit(doc, bus, submit, m_path);
            } else if (key == kKeyEscape) {
                UIEvent cancel = makeEvent(UIEventType::Cancel, m_focused);
                Emit(doc, bus, cancel, m_path);
                ClearFocus(doc, &bus);
            }
        }
        for (int key : input.KeysUp) {
            UIEvent e = makeEvent(UIEventType::KeyUp, m_focused);
            e.Key = key;
            Emit(doc, bus, e, m_path);
        }
        if (!input.TextInput.empty() && field && !field->ReadOnly) {
            // Предел длины считается в СИМВОЛАХ, а не в байтах: иначе русское
            // имя обрезается вдвое раньше английского.
            const bool room = field->MaxLength <= 0 ||
                              UIUtf8Length(field->Value) < field->MaxLength;
            if (room) {
                field->Value.insert((size_t)field->Caret, input.TextInput);
                field->Caret += (int)input.TextInput.size();
                if (UIInteraction* ia = node->Get<UIInteraction>()) ia->Runtime.Changed = true;
                UIEvent e = makeEvent(UIEventType::TextInput, m_focused);
                Emit(doc, bus, e, m_path);
                doc.MarkDirty(UIDirty_Text | UIDirty_Layout);
            }
        }
        if (field) result.KeyboardCaptured = true;
    }

    // --- Навигация ----------------------------------------------------------
    if (input.NavX != 0 || input.NavY != 0) {
        const UINodeId next = FindNeighbour(doc, layout, m_focused, input.NavX, input.NavY);
        if (next != kUIInvalidNode) SetFocus(doc, next, &bus);
    }
    if (input.NavNext || input.NavPrev) {
        const UINodeId next = FindTabTarget(doc, layout, m_focused, input.NavNext);
        if (next != kUIInvalidNode) SetFocus(doc, next, &bus);
    }
    if (input.NavSubmit && m_focused != kUIInvalidNode) {
        if (UIInteraction* ia = InteractionOf(doc, m_focused)) {
            ia->Runtime.Clicked = true;
            if (!ia->Command.empty()) result.Commands.push_back(ia->Command);
        }
        UIEvent e = makeEvent(UIEventType::Click, m_focused);
        Emit(doc, bus, e, m_path);
    }
    if (input.NavCancel && m_focused != kUIInvalidNode) {
        UIEvent e = makeEvent(UIEventType::Cancel, m_focused);
        Emit(doc, bus, e, m_path);
    }

    result.PointerOverUI = hit.Node != kUIInvalidNode;
    result.PointerCaptured = m_capture != kUIInvalidNode;
    result.Hovered = m_hovered;
    result.Focused = m_focused;
    if (m_hovered != kUIInvalidNode) {
        if (UIInteraction* ia = InteractionOf(doc, m_hovered)) result.Cursor = ia->Cursor;
    }
    return result;
}

void UIInputRouter::SetFocus(UIDocument& doc, UINodeId id, UIEventBus* bus) {
    if (m_focused == id) return;
    if (m_focused != kUIInvalidNode) {
        if (UIInteraction* ia = InteractionOf(doc, m_focused)) SetFlag(*ia, UIState_Focused, false);
        if (bus) {
            UIEvent e;
            e.Type = UIEventType::Blur;
            e.Target = e.Current = m_focused;
            bus->Dispatch(e);
        }
    }
    m_focused = id;
    if (m_focused != kUIInvalidNode) {
        if (UIInteraction* ia = InteractionOf(doc, m_focused)) SetFlag(*ia, UIState_Focused, true);
        if (UINode* n = doc.Find(m_focused)) {
            if (UITextField* f = n->Get<UITextField>()) {
                if (f->SelectAllOnFocus) f->Caret = (int)f->Value.size();
            }
        }
        if (bus) {
            UIEvent e;
            e.Type = UIEventType::Focus;
            e.Target = e.Current = m_focused;
            bus->Dispatch(e);
        }
    }
}

void UIInputRouter::ClearFocus(UIDocument& doc, UIEventBus* bus) {
    SetFocus(doc, kUIInvalidNode, bus);
}

UINodeId UIInputRouter::FindNeighbour(UIDocument& doc, const UILayoutSolver& layout,
                                      UINodeId from, int dx, int dy) const {
    // Явные NavUp/NavDown/... перекрывают геометрию: сложное меню невозможно
    // провести по задуманному маршруту одной геометрией (§56).
    if (UINode* n = doc.Find(from)) {
        if (const UIInteraction* ia = n->Get<UIInteraction>()) {
            const std::string& explicitName = dy < 0   ? ia->NavUp
                                              : dy > 0 ? ia->NavDown
                                              : dx < 0 ? ia->NavLeft
                                                       : ia->NavRight;
            if (!explicitName.empty()) {
                if (UINode* target = doc.FindByName(explicitName)) return target->Id;
            }
        }
    }

    const UIResolvedNode* origin = layout.Get(from);
    UINodeId best = kUIInvalidNode;
    float bestScore = 1e30f;
    const glm::vec2 fromCentre = origin ? UICenter(origin->Rect) : glm::vec2(0.0f);

    for (const UIResolvedNode& r : layout.Nodes()) {
        if (r.Id == from || !r.Visible || r.Culled) continue;
        UINode* n = doc.Find(r.Id);
        if (!n) continue;
        const UIInteraction* ia = n->Get<UIInteraction>();
        if (!ia || !ia->Focusable || !ia->Enabled) continue;
        if (!origin) return r.Id; // фокуса не было — берём первый подходящий

        const glm::vec2 c = UICenter(r.Rect);
        const glm::vec2 d = c - fromCentre;
        const float along = d.x * (float)dx + d.y * (float)dy;
        if (along <= 1.0f) continue; // не в ту сторону
        const float across = std::fabs(d.x * (float)dy - d.y * (float)dx);
        // Вдоль — важнее, поперёк — штраф: иначе «вправо» уводит по диагонали
        // в дальний угол, и навигация ощущается случайной.
        const float score = along + across * 2.0f;
        if (score < bestScore) { bestScore = score; best = r.Id; }
    }
    return best;
}

UINodeId UIInputRouter::FindTabTarget(UIDocument& doc, const UILayoutSolver& layout,
                                      UINodeId from, bool forward) const {
    std::vector<std::pair<int, UINodeId>> order;
    for (const UIResolvedNode& r : layout.Nodes()) {
        UINode* n = doc.Find(r.Id);
        if (!n || !r.Visible) continue;
        const UIInteraction* ia = n->Get<UIInteraction>();
        if (!ia || !ia->Focusable || !ia->Enabled) continue;
        order.emplace_back(ia->TabIndex, r.Id);
    }
    if (order.empty()) return kUIInvalidNode;
    // Стабильная сортировка: при равном TabIndex порядок обхода дерева и есть
    // порядок табуляции — то, что человек видит в редакторе.
    std::stable_sort(order.begin(), order.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });

    int current = -1;
    for (int i = 0; i < (int)order.size(); ++i)
        if (order[(size_t)i].second == from) { current = i; break; }
    if (current < 0) return order.front().second;
    const int n = (int)order.size();
    const int next = ((current + (forward ? 1 : -1)) % n + n) % n;
    return order[(size_t)next].second;
}

} // namespace sage::ui
