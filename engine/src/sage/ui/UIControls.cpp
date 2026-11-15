#include "sage/ui/UIControls.h"

#include <algorithm>
#include <cmath>

#include "sage/assets/AssetDatabase.h"
#include "sage/scene/SceneComponents.h"
#include "sage/scene/Signals.h"
#include "sage/ui/UI.h"
#include "sage/ui/UIMasks.h"
#include "sage/ui/UIPart.h"
#include "UIRenderer.h"

namespace sage::ui {
namespace {

bool In(const UIRect& r, glm::vec2 p) { return p.x >= r.x && p.y >= r.y && p.x < r.x + r.w && p.y < r.y + r.h; }

int PrevChar(const std::string& s, int i) {
    if (i <= 0) return 0;
    --i;
    while (i > 0 && ((unsigned char)s[(size_t)i] & 0xC0) == 0x80) --i;
    return i;
}
int NextChar(const std::string& s, int i) {
    const int n = (int)s.size();
    if (i >= n) return n;
    ++i;
    while (i < n && ((unsigned char)s[(size_t)i] & 0xC0) == 0x80) ++i;
    return i;
}

State* StateOf(entt::registry& reg, entt::entity e) {
    Interactable* act = reg.try_get<Interactable>(e);
    return act ? &act->Runtime : nullptr;
}
bool Usable(const entt::registry& reg, entt::entity e) {
    const Interactable* act = reg.try_get<Interactable>(e);
    return act && act->Enabled;
}

sage::vars::Value RangeValue(const Range& r) {
    if (r.Toggle) return sage::vars::Value(r.Value >= (r.Min + r.Max) * 0.5f);
    return sage::vars::Value(r.Value);
}

void SetRange(entt::registry& reg, entt::entity e, Range& range, float v, const SignalFn& fire) {
    if (range.Step > 0.0f) v = range.Min + std::round((v - range.Min) / range.Step) * range.Step;
    v = glm::clamp(v, glm::min(range.Min, range.Max), glm::max(range.Min, range.Max));
    if (v == range.Value) return;
    range.Value = v;
    if (State* st = StateOf(reg, e)) st->Changed = true;
    fire(e, sage::signals::kValueChanged, RangeValue(range));
}

void SelectOption(entt::registry& reg, entt::entity e, Dropdown& d, int index, const SignalFn& fire) {
    d.Open = false;
    if (index == d.Selected || DropdownDisabled(d, index)) return;
    d.Selected = index;
    if (State* st = StateOf(reg, e)) st->Changed = true;
    fire(e, sage::signals::kValueChanged, sage::vars::Value(index));
}

// Следующая доступная строка в сторону dir (или та же, если некуда).
int StepOption(const Dropdown& d, int from, int dir, int count) {
    for (int i = from + dir; i >= 0 && i < count; i += dir)
        if (!DropdownDisabled(d, i)) return i;
    return from;
}

void KeepVisible(Dropdown& d, int visible, int count) {
    if (d.Hover >= 0) {
        if (d.Hover < d.First) d.First = d.Hover;
        if (d.Hover >= d.First + visible) d.First = d.Hover - visible + 1;
    }
    d.First = std::clamp(d.First, 0, std::max(0, count - visible));
}

bool IsNavigable(const entt::registry& reg, const Solved& it) {
    const Interactable* act = reg.try_get<Interactable>(it.Entity);
    return act && act->Enabled && act->Navigable && it.Interactive;
}

} // namespace

bool DropdownListLayout(const Dropdown& d, const UIRect& field, glm::vec2 screen, DropdownLayout& out) {
    const int count = (int)DropdownOptions(d).size();
    if (count == 0 || field.h <= 0.0f) return false;
    out.Row = field.h;
    int visible = std::min(count, std::max(1, d.MaxVisible));
    const float below = screen.y - (field.y + field.h);
    const float above = field.y;
    // Снизу не помещается — над полем; нигде целиком — туда, где больше места,
    // и строк столько, сколько влезло.
    bool up = below < out.Row * (float)visible && above > below;
    const float room = up ? above : below;
    visible = std::clamp((int)std::floor(room / out.Row), 1, visible);
    const float h = out.Row * (float)visible;
    const float x = std::clamp(field.x, 0.0f, std::max(0.0f, screen.x - field.w));
    out.List = {x, up ? field.y - h : field.y + field.h, field.w, h};
    out.Visible = visible;
    return true;
}

bool UpdateDropdowns(entt::registry& reg, const std::vector<Solved>& items, entt::entity hovered,
                     const UIInputState& input, glm::vec2 screen, const SignalFn& fire) {
    const Solved* openIt = nullptr;
    for (const Solved& it : items) {
        Dropdown* d = reg.try_get<Dropdown>(it.Entity);
        if (!d || !d->Open) continue;
        if (openIt || !Usable(reg, it.Entity)) d->Open = false;   // открыт один, и только доступный
        else openIt = &it;
    }
    // Список, которого больше нет на экране (спрятали панель), закрыт.
    reg.view<Dropdown>().each([&](entt::entity e, Dropdown& d) {
        if (d.Open && (!openIt || openIt->Entity != e)) d.Open = false;
    });

    if (!openIt) {
        entt::entity target = entt::null;
        if (input.MousePressed && hovered != entt::null && reg.all_of<Dropdown>(hovered)) target = hovered;
        // С клавиатуры — Enter на списке в фокусе.
        if (input.Enter && target == entt::null)
            for (const Solved& it : items)
                if (const State* st = StateOf(reg, it.Entity);
                    st && st->Focused && reg.all_of<Dropdown>(it.Entity) && Usable(reg, it.Entity))
                    target = it.Entity;
        if (target == entt::null) return false;
        Dropdown& d = reg.get<Dropdown>(target);
        if (DropdownOptions(d).empty()) return true;
        d.Open = true;
        d.Hover = d.Selected;
        d.First = std::max(0, d.Selected);
        return true;
    }

    Dropdown& d = reg.get<Dropdown>(openIt->Entity);
    DropdownLayout lay;
    const int count = (int)DropdownOptions(d).size();
    if (!DropdownListLayout(d, openIt->Rect, screen, lay)) {
        d.Open = false;
        return true;
    }
    KeepVisible(d, lay.Visible, count);
    const bool inside = In(lay.List, input.Mouse);
    if (inside) {
        if (input.Wheel != 0.0f) {
            d.First = std::clamp(d.First - (int)std::round(input.Wheel), 0, std::max(0, count - lay.Visible));
        }
        d.Hover = std::min(count - 1, d.First + (int)((input.Mouse.y - lay.List.y) / lay.Row));
    }
    if (input.Down) d.Hover = StepOption(d, d.Hover, 1, count);
    if (input.Up) d.Hover = StepOption(d, d.Hover < 0 ? count : d.Hover, -1, count);
    if (input.Up || input.Down) KeepVisible(d, lay.Visible, count);
    if (input.Escape) d.Open = false;
    else if (input.Enter && d.Hover >= 0) SelectOption(reg, openIt->Entity, d, d.Hover, fire);
    else if (input.MousePressed) {
        // Щелчок по недоступной строке ничего не делает; мимо списка — закрывает.
        if (!inside) d.Open = false;
        else if (!DropdownDisabled(d, d.Hover)) SelectOption(reg, openIt->Entity, d, d.Hover, fire);
    }
    return true;
}

void DrawDropdownOverlays(const entt::registry& reg, const std::vector<Solved>& items, glm::vec2 screen,
                          UIRenderer& ui) {
    for (const Solved& it : items) {
        const Dropdown* d = reg.try_get<Dropdown>(it.Entity);
        if (!d || !d->Open || !it.Visible) continue;
        DropdownLayout lay;
        if (!DropdownListLayout(*d, it.Rect, screen, lay)) continue;
        const std::vector<std::string> options = DropdownOptions(*d);
        const float radius = 4.0f * it.Scale;
        ui.RoundedRect(lay.List.x, lay.List.y, lay.List.w, lay.List.h, glm::vec3(d->ListColor),
                       d->ListColor.a * it.Alpha, radius);
        const Label* label = reg.try_get<Label>(it.Entity);
        const float scale = (label ? label->Scale : Label{}.Scale) * it.Scale;
        const float pad = (label ? label->PadX : 10.0f) * it.Scale;
        ui.PushClipRect(lay.List.x, lay.List.y, lay.List.w, lay.List.h);
        const int first = std::clamp(d->First, 0, std::max(0, (int)options.size() - lay.Visible));
        for (int row = 0; row < lay.Visible && first + row < (int)options.size(); ++row) {
            const int i = first + row;
            const float y = lay.List.y + lay.Row * (float)row;
            if (i == d->Hover && !DropdownDisabled(*d, i))
                ui.RoundedRect(lay.List.x, y, lay.List.w, lay.Row, glm::vec3(d->HighlightColor),
                               d->HighlightColor.a * it.Alpha, radius);
            const float a = DropdownDisabled(*d, i) ? 0.35f : (i == d->Selected ? 1.0f : 0.85f);
            const float ty = y + (lay.Row - ui.LineHeight(scale)) * 0.5f;
            ui.Text(lay.List.x + pad, ty, scale, glm::vec3(d->TextColor), options[(size_t)i],
                    d->TextColor.a * a * it.Alpha);
        }
        // Длинный список — тонкая отметка, где ты в нём.
        if ((int)options.size() > lay.Visible) {
            const float share = (float)lay.Visible / (float)options.size();
            const float h = lay.List.h * share;
            const float y = lay.List.y + (lay.List.h - h) * ((float)first / (float)(options.size() - lay.Visible));
            ui.RoundedRect(lay.List.x + lay.List.w - 4.0f * it.Scale, y, 3.0f * it.Scale, h,
                           glm::vec3(d->TextColor), 0.35f * it.Alpha, 1.5f * it.Scale);
        }
        ui.PopClipRect();
    }
}

void UpdateSliders(entt::registry& reg, const std::vector<Solved>& items, const UIInputState& input,
                   UIInputResult& result, const SignalFn& fire) {
    // Тянется, пока кнопка удерживается, даже если курсор ушёл с дорожки —
    // иначе значение срывается от малейшего движения вбок.
    for (const Solved& it : items) {
        Range* range = reg.try_get<Range>(it.Entity);
        State* st = StateOf(reg, it.Entity);
        if (!range || range->Toggle || !st || !Usable(reg, it.Entity)) continue;
        if (!st->Pressed || !input.MouseDown) continue;
        const float t = range->Vertical
                            ? glm::clamp((it.Rect.y + it.Rect.h - input.Mouse.y) / glm::max(it.Rect.h, 1.0f), 0.0f, 1.0f)
                            : glm::clamp((input.Mouse.x - it.Rect.x) / glm::max(it.Rect.w, 1.0f), 0.0f, 1.0f);
        SetRange(reg, it.Entity, *range, range->Min + t * (range->Max - range->Min), fire);
        result.WantsMouse = true;
    }
}

void UpdateTextInputs(entt::registry& reg, const std::vector<Solved>& items, const UIInputState& input,
                      UIInputResult& result, const SignalFn& fire) {
    for (const Solved& it : items) {
        const TextInput* field = reg.try_get<TextInput>(it.Entity);
        State* st = StateOf(reg, it.Entity);
        Label* lbl = reg.try_get<Label>(it.Entity);
        if (!field || !st || !lbl || !st->Focused || !Usable(reg, it.Entity)) continue;
        result.WantsKeyboard = true;
        st->CaretBlink += input.DeltaTime;
        st->Caret = glm::clamp(st->Caret, 0, (int)lbl->Text.size());
        if (field->ReadOnly) {
            if (input.Escape || (input.Enter && !field->Multiline)) st->Focused = false;
            continue;   // показывать можно, править нельзя
        }
        const std::string before = lbl->Text;
        // Проверка — посимвольно: из вставленной строки проходит годное. Предел
        // длины — на всё набранное разом: не влезло целиком — не принято вовсе,
        // иначе вставка молча обрезалась бы на середине слова.
        std::string accepted;
        for (int i = 0; i < (int)input.TypedText.size();) {
            const int next = NextChar(input.TypedText, i);
            const std::string c = input.TypedText.substr((size_t)i, (size_t)(next - i));
            if (InputAccepts(field->Validation, lbl->Text + accepted, c)) accepted += c;
            i = next;
        }
        if (!accepted.empty() &&
            (field->MaxLength <= 0 || Utf8Length(lbl->Text) + Utf8Length(accepted) <= field->MaxLength)) {
            lbl->Text.insert((size_t)st->Caret, accepted);
            st->Caret += (int)accepted.size();
        }
        if (input.Backspace && st->Caret > 0) {
            const int prev = PrevChar(lbl->Text, st->Caret);
            lbl->Text.erase((size_t)prev, (size_t)(st->Caret - prev));
            st->Caret = prev;
        }
        if (input.Delete && st->Caret < (int)lbl->Text.size())
            lbl->Text.erase((size_t)st->Caret, (size_t)(NextChar(lbl->Text, st->Caret) - st->Caret));
        if (input.Left) st->Caret = PrevChar(lbl->Text, st->Caret);
        if (input.Right) st->Caret = NextChar(lbl->Text, st->Caret);
        const size_t lineStart = st->Caret == 0 ? 0 : lbl->Text.rfind('\n', (size_t)st->Caret - 1) + 1;
        size_t lineEnd = lbl->Text.find('\n', (size_t)st->Caret);
        if (lineEnd == std::string::npos) lineEnd = lbl->Text.size();
        if (input.Home) st->Caret = field->Multiline ? (int)lineStart : 0;
        if (input.End) st->Caret = field->Multiline ? (int)lineEnd : (int)lbl->Text.size();
        if (field->Multiline && (input.Up || input.Down)) {
            // Строкой выше/ниже — на ту же колонку (или конец более короткой).
            const size_t col = (size_t)st->Caret - lineStart;
            if (input.Up && lineStart > 0) {
                const size_t prevEnd = lineStart - 1;
                const size_t prevStart = prevEnd == 0 ? 0 : lbl->Text.rfind('\n', prevEnd - 1) + 1;
                st->Caret = (int)std::min(prevStart + col, prevEnd);
            } else if (input.Down && lineEnd < lbl->Text.size()) {
                const size_t nextStart = lineEnd + 1;
                size_t nextEnd = lbl->Text.find('\n', nextStart);
                if (nextEnd == std::string::npos) nextEnd = lbl->Text.size();
                st->Caret = (int)std::min(nextStart + col, nextEnd);
            }
            // Колонка в байтах могла попасть внутрь символа — назад к его началу.
            while (st->Caret > 0 && ((unsigned char)lbl->Text[(size_t)st->Caret] & 0xC0) == 0x80) --st->Caret;
        }
        if (input.Enter) {
            if (field->Multiline) {
                if (field->MaxLength <= 0 || Utf8Length(lbl->Text) < field->MaxLength) {
                    lbl->Text.insert((size_t)st->Caret, "\n");
                    ++st->Caret;
                }
            } else {
                // ПОДТВЕРЖДЕНИЕ: «ввёл имя и нажал Enter» — отдельное событие,
                // а не каждая буква (text_changed).
                fire(it.Entity, sage::signals::kSubmitted, sage::vars::Value(lbl->Text));
                st->Focused = false;
            }
        }
        if (input.Escape) st->Focused = false;
        if (input.Left || input.Right || input.Home || input.End || input.Up || input.Down ||
            lbl->Text != before)
            st->CaretBlink = 0.0f;
        if (lbl->Text != before) {
            st->Changed = true;
            fire(it.Entity, sage::signals::kTextChanged, sage::vars::Value(lbl->Text));
        }
    }
}

void FlipToggle(entt::registry& reg, entt::entity e, const SignalFn& fire) {
    Range* range = reg.try_get<Range>(e);
    if (!range || !range->Toggle) return;
    const float mid = (range->Min + range->Max) * 0.5f;
    range->Value = range->Value >= mid ? range->Min : range->Max;
    if (State* st = StateOf(reg, e)) st->Changed = true;
    fire(e, sage::signals::kValueChanged, RangeValue(*range));
}

bool UpdateNavigation(entt::registry& reg, const std::vector<Solved>& items, const UIInputState& input,
                      UIInputResult& result, const SignalFn& fire) {
    if (!(input.Tab || input.Up || input.Down || input.Left || input.Right || input.Enter)) return false;
    std::vector<const Solved*> nav;
    const Solved* focused = nullptr;
    for (const Solved& it : items) {
        if (!IsNavigable(reg, it)) continue;
        nav.push_back(&it);
        if (const State* st = StateOf(reg, it.Entity); st && st->Focused) focused = &it;
    }
    if (nav.empty()) return false;
    // Поле с фокусом забирает стрелки и Enter себе; Tab — уводит дальше.
    const bool typing = focused && reg.all_of<TextInput>(focused->Entity);
    if (typing && !input.Tab) return false;

    auto focus = [&](const Solved* to) {
        for (const Solved* n : nav)
            if (State* st = StateOf(reg, n->Entity)) st->Focused = n == to;
        if (State* st = StateOf(reg, to->Entity)) {
            st->CaretBlink = 0.0f;
            if (const Label* l = reg.try_get<Label>(to->Entity)) st->Caret = (int)l->Text.size();
        }
        result.WantsKeyboard = true;
    };

    if (input.Tab) {
        // Tab — по порядку интерфейса; Shift+Tab — назад; по кругу.
        const int n = (int)nav.size();
        int at = -1;
        for (int i = 0; i < n; ++i)
            if (nav[(size_t)i] == focused) at = i;
        const int next = at < 0 ? (input.Shift ? n - 1 : 0) : (at + (input.Shift ? n - 1 : 1)) % n;
        focus(nav[(size_t)next]);
        return true;
    }
    if (!focused) {
        // Первое нажатие стрелки только показывает, где фокус.
        if (input.Enter) return false;
        focus(nav.front());
        return true;
    }
    if (input.Enter) {
        // Enter — «нажать»: кнопка щёлкает, переключатель переключается.
        // Список открывает UpdateDropdowns.
        if (reg.all_of<Dropdown>(focused->Entity)) return false;
        State* st = StateOf(reg, focused->Entity);
        if (Range* r = reg.try_get<Range>(focused->Entity); r && !r->Toggle) return true;
        if (st) st->Clicked = true;
        if (const IdComponent* id = reg.try_get<IdComponent>(focused->Entity)) result.ClickedId = id->Id;
        result.ClickedAction = reg.get<Interactable>(focused->Entity).Action;
        fire(focused->Entity, sage::signals::kClicked, {});
        FlipToggle(reg, focused->Entity, fire);
        if (const Interactable* act = reg.try_get<Interactable>(focused->Entity); act && !act->ClickSound.empty())
            result.Sounds.push_back(act->ClickSound);
        result.WantsKeyboard = true;
        return true;
    }
    // Ползунок в фокусе забирает стрелки своей оси: шаг — его Step или 5%.
    if (Range* r = reg.try_get<Range>(focused->Entity); r && !r->Toggle) {
        const int dir = r->Vertical ? (input.Up ? 1 : input.Down ? -1 : 0) : (input.Right ? 1 : input.Left ? -1 : 0);
        if (dir != 0) {
            const float step = r->Step > 0.0f ? r->Step : (r->Max - r->Min) * 0.05f;
            SetRange(reg, focused->Entity, *r, r->Value + step * (float)dir, fire);
            result.WantsKeyboard = true;
            return true;
        }
    }
    // Стрелки — к БЛИЖАЙШЕМУ в ту сторону: расстояние вдоль направления плюс
    // утроенный уход вбок, чтобы «вниз» не прыгало в соседний столбец.
    const glm::vec2 dir = input.Up ? glm::vec2(0, -1) : input.Down ? glm::vec2(0, 1)
                        : input.Left ? glm::vec2(-1, 0) : glm::vec2(1, 0);
    const glm::vec2 from{focused->Rect.x + focused->Rect.w * 0.5f, focused->Rect.y + focused->Rect.h * 0.5f};
    const Solved* best = nullptr;
    float bestScore = 0.0f;
    for (const Solved* n : nav) {
        if (n == focused) continue;
        const glm::vec2 to{n->Rect.x + n->Rect.w * 0.5f, n->Rect.y + n->Rect.h * 0.5f};
        const glm::vec2 d = to - from;
        const float along = glm::dot(d, dir);
        if (along <= 1.0f) continue;
        const float side = std::fabs(glm::dot(d, glm::vec2(-dir.y, dir.x)));
        const float score = along + side * 3.0f;
        if (!best || score < bestScore) {
            best = n;
            bestScore = score;
        }
    }
    if (best) focus(best);
    result.WantsKeyboard = true;
    return true;
}

void AnimateToggles(entt::registry& reg, const std::vector<Solved>& items, float dt) {
    for (const Solved& it : items) {
        Range* r = reg.try_get<Range>(it.Entity);
        if (!r || !r->Toggle) continue;
        const float target = r->Value >= (r->Min + r->Max) * 0.5f ? 1.0f : 0.0f;
        if (r->Shown < 0.0f || r->AnimationTime <= 0.0f) {
            r->Shown = target;
            continue;
        }
        const float step = dt / r->AnimationTime;
        r->Shown += glm::clamp(target - r->Shown, -step, step);
    }
}

void UpdateImageSequences(entt::registry& reg, const std::vector<Solved>& items, float dt) {
    for (const Solved& it : items) {
        Image* im = reg.try_get<Image>(it.Entity);
        if (!im || im->Sequence.empty() || im->Path.empty()) continue;
        if (im->SheetFor != im->Path) {
            // Лист — один раз на путь: ошибка чтения не повторяется каждый кадр.
            im->SheetFor = im->Path;
            auto sheet = std::make_shared<SpriteSheet>();
            std::string err;
            const std::string real = sage::AssetDatabase::Instance().LocatePath(im->Path);
            if (SpriteSheet::LoadFile(SpriteSheet::SidecarPath(real), *sheet, err)) im->Sheet = sheet;
            else im->Sheet.reset();
        }
        if (!im->Sheet) continue;
        const SpriteSequence* seq = im->Sheet->FindSequence(im->Sequence);
        if (!seq) continue;
        im->SequenceTime += dt;
        if (const SpriteRegion* r = im->Sheet->Find(seq->FrameAt(im->SequenceTime)))
            im->Sprite = glm::vec4(r->Rect);
    }
}

void UpdateProgress(entt::registry& reg, const std::vector<Solved>& items, float dt) {
    for (const Solved& it : items) {
        Bar* bar = reg.try_get<Bar>(it.Entity);
        if (!bar) continue;
        // Бег отрезка: круг за 1.4 секунды — заметно, но не мельтешит.
        if (bar->Indeterminate) bar->Phase = std::fmod(bar->Phase + dt / 1.4f, 1.0f);
        if (!bar->HasDisplayed || (bar->Smoothing <= 0.0f && bar->Easing <= 0.0f)) {
            bar->Displayed = bar->Value;
            bar->HasDisplayed = true;
            continue;
        }
        const float diff = bar->Value - bar->Displayed;
        // Смягчение: за Easing секунд остаётся около 5 % пути (e^-3).
        float step = bar->Easing > 0.0f ? diff * (1.0f - std::exp(-3.0f * dt / bar->Easing)) : diff;
        if (bar->Smoothing > 0.0f) step = glm::clamp(step, -bar->Smoothing * dt, bar->Smoothing * dt);
        bar->Displayed += step;
        if (std::fabs(bar->Value - bar->Displayed) < 1e-4f * std::max(1.0f, std::fabs(bar->Max - bar->Min)))
            bar->Displayed = bar->Value;
    }
}

} // namespace sage::ui
