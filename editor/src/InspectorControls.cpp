#include "InspectorControls.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"

#include "EditorHost.h"
#include "EditorIcons.h"
#include "EditorPrefs.h"
#include "EditorTheme.h"
#include "Localization.h"
#include "ui/ColorPicker.h"

namespace sage::editor::ictl {
namespace {

std::function<void()>& Sink() {
    static std::function<void()> s;
    return s;
}

// Снимок перед правкой — в историю инструмента, если он её подменил.
void Snapshot(EditorHost& host) {
    if (Sink()) Sink()();
    else host.PushUndoSnapshot();
}

// Перетаскивание и ввод: один шаг истории на всё касание.
void Track(EditorHost& host) {
    if (Sink()) {
        if (ImGui::IsItemActivated()) Sink()();
    } else {
        host.TrackLastImGuiItem();
    }
}

// Колесо над полем: одна правка — один шаг истории, пока колесо крутят без
// пауз (иначе каждый щелчок колеса стал бы отдельной отменой).
bool WheelGestureStart(ImGuiID id) {
    static ImGuiID s_last = 0;
    static double s_time = -10.0;
    const double now = ImGui::GetTime();
    const bool fresh = s_last != id || now - s_time > 0.6;
    s_last = id;
    s_time = now;
    return fresh;
}

float Clamp(float v, const NumOpts& o) { return std::clamp(v, o.Min, o.Max); }

// ЕДИНЫЕ ОТСТУПЫ ИНСПЕКТОРА — одна таблица, а не поправки по месту.
constexpr float kLabelGap = 8.0f;   // подпись → поле
constexpr float kTagGap = 4.0f;     // «X» → его поле; поле → значок замка
constexpr float kPairGap = 10.0f;   // первое поле пары → подпись второго
constexpr float kIndent = 14.0f;    // содержимое вложенной группы
constexpr float kGroupGap = 6.0f;   // между независимыми группами
constexpr float kFieldMin = 58.0f;  // самое узкое [−] число [+], где число ещё читается

// Кнопка −/+ числового поля: квадрат высотой с поле, значок Tabler по
// центру, повтор при удержании; после секунды удержания шаг вдесятеро.
bool StepButton(EditorHost& host, const char* id, const char* icon, float* v, float dir, const NumOpts& o, ImVec2 at,
                float side, ImDrawFlags corners) {
    const float h = ImGui::GetFrameHeight();
    ImGui::SetCursorScreenPos(at);
    ImGui::PushItemFlag(ImGuiItemFlags_ButtonRepeat, true);
    const bool hit = ImGui::InvisibleButton(id, ImVec2(side, h));
    ImGui::PopItemFlag();
    const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hovered || held)
        dl->AddRectFilled(at, ImVec2(at.x + side, at.y + h),
                          EditorTheme::Color32(held ? EditorTheme::Role::AccentMuted : EditorTheme::Role::Hover),
                          ImGui::GetStyle().FrameRounding, corners);
    // Оба значка одного размера и строго по центру квадрата.
    const float is = std::floor(h * 0.56f);
    EditorIcons::Overlay(dl, std::floor(at.x + (side - is) * 0.5f), std::floor(at.y + (h - is) * 0.5f), is, icon,
                         hovered || held ? EditorIcons::kThemeColor : glm::vec3(0.62f, 0.64f, 0.70f));
    if (hovered) ImGui::SetTooltip("%s", T("Hold to repeat; after a second the step grows tenfold"));
    // Снимок — один на всё удержание: на нажатии, а не на каждом повторе.
    if (ImGui::IsItemActivated()) Snapshot(host);
    if (!hit) return false;
    const bool accelerate = ImGui::GetIO().MouseDownDuration[0] > 1.0f;
    const float nv = Clamp(*v + dir * o.Step * (accelerate ? 10.0f : 1.0f), o);
    if (nv == *v) return false;
    *v = nv;
    return true;
}

std::string PrefKey(const char* what, ImGuiID id) { return std::string("inspector.") + what + "." + std::to_string(id); }

// Небольшая подпись оси перед полем («X», «W», «Min»): колонка шириной width,
// поле — через kTagGap.
void Tag(const char* text, float width) {
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(EditorTheme::Role::TextDim));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::SameLine(0.0f, kTagGap + std::max(0.0f, width - ImGui::CalcTextSize(text).x));
}

// Пара полей ровно в width: «X [..]  Y [..]».
bool PairIn(EditorHost& host, float* a, float* b, const char* tagA, const char* tagB, const NumOpts& o, float width) {
    const float tag = std::max(ImGui::CalcTextSize(tagA).x, ImGui::CalcTextSize(tagB).x);
    const float field = std::max(kFieldMin, std::floor((width - 2.0f * (tag + kTagGap) - kPairGap) * 0.5f));
    Tag(tagA, tag);
    bool changed = NumberCore(host, "a", a, o, field);
    ImGui::SameLine(0.0f, kPairGap);
    Tag(tagB, tag);
    changed |= NumberCore(host, "b", b, o, field);
    return changed;
}

// Значок-переключатель связи (замок) в конце строки.
bool LinkIcon(bool linked, const char* onTip, const char* offTip) {
    ImGui::SameLine(0.0f, kTagGap);
    return EditorIcons::IconOnlyButton(linked ? "lock" : "unlock", linked ? onTip : offTip, linked);
}


// Ширина пары, ниже которой поля сжались бы: тогда подпись — над ними.
float PairNeed(const char* tagA, const char* tagB) {
    const float tag = std::max(ImGui::CalcTextSize(tagA).x, ImGui::CalcTextSize(tagB).x);
    return 2.0f * (tag + kTagGap + kFieldMin) + kPairGap;
}

} // namespace

UndoScope::UndoScope(std::function<void()> push) : m_prev(std::move(Sink())) { Sink() = std::move(push); }
UndoScope::~UndoScope() { Sink() = std::move(m_prev); }

NumOpts OptsForRange(float min, float max) {
    NumOpts o;
    o.Min = min;
    o.Max = max;
    const float span = (max > min && max < FLT_MAX * 0.5f && min > -FLT_MAX * 0.5f) ? max - min : 1e9f;
    if (span <= 2.0f) {
        o.Step = 0.01f;
        o.Format = "%.2f";
    } else if (span <= 20.0f) {
        o.Step = 0.1f;
        o.Format = "%.1f";
    } else if (span <= 10000.0f) {
        o.Step = 1.0f;
        o.Format = "%.0f";
    } else {
        // Почти без границ (значение шкалы, смещение): число может быть и
        // долей, и тысячами — показываем как есть, без округления до целых.
        o.Step = 1.0f;
        o.Format = "%.6g";
    }
    return o;
}

float LabelWidth() {
    // Колонка подписей — доля ширины панели: одинаковая у всех строк уровня.
    return std::clamp(ImGui::GetContentRegionAvail().x * 0.42f, 92.0f, 180.0f);
}

void Label(const char* text, float minField) {
    const float x0 = ImGui::GetCursorPosX();
    const float avail = ImGui::GetContentRegionAvail().x;
    const float w = LabelWidth();
    // «Имя:» — подпись свойства с двоеточием; часть после «##» — не подпись.
    const char* end = text + std::strlen(text);
    if (const char* hash = std::strstr(text, "##")) end = hash;
    std::string shown(text, end);
    if (!shown.empty() && shown.back() != ':') shown += ':';
    // Длинная подпись (или узкое поле) — НАД полем, целиком: обрезанная
    // посередине подпись не читается, а подсказку никто не ищет.
    if (ImGui::CalcTextSize(shown.c_str()).x > w - kLabelGap || avail - w < std::max(60.0f, minField)) {
        ImGui::TextUnformatted(shown.c_str());
        ImGui::SetNextItemWidth(avail);
        return;
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(shown.c_str());
    ImGui::SameLine(x0 + w);
    ImGui::SetNextItemWidth(std::max(60.0f, ImGui::GetContentRegionAvail().x));
}

float NumberWidth() { return std::clamp(ImGui::GetContentRegionAvail().x, 110.0f, 170.0f); }

bool NumberCore(EditorHost& host, const char* id, float* v, const NumOpts& o, float width) {
    ImGui::PushID(id);
    if (width <= 0.0f) width = NumberWidth();
    // ОДИН КОНТРОЛ: [−|значение|+] общей рамкой высотой с поле; кнопки —
    // квадраты со значками Tabler, отделены тонкими линиями.
    const float h = ImGui::GetFrameHeight();
    // Кнопка чуть уже высоты: числу в узкой паре нужно место больше, чем
    // квадратной кнопке.
    const float side = std::floor(std::clamp(width * 0.22f, h * 0.62f, h * 0.8f));
    width = std::max(width, side * 2.0f + 28.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float rounding = ImGui::GetStyle().FrameRounding;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + h), ImGui::GetColorU32(ImGuiCol_FrameBg), rounding);
    bool changed = StepButton(host, "##dec", "minus", v, -1.0f, o, p, side, ImDrawFlags_RoundCornersLeft);
    ImGui::SetCursorScreenPos(ImVec2(p.x + side + 1.0f, p.y));
    ImGui::SetNextItemWidth(width - 2.0f * side - 2.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    // Перетаскивание — по полю, двойной щелчок (или Ctrl+щелчок) — ввод числа.
    if (ImGui::DragFloat("##v", v, std::max(o.Step * 0.25f, 0.001f), o.Min, o.Max, o.Format,
                         ImGuiSliderFlags_AlwaysClamp)) {
        changed = true;
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    Track(host);
    const ImGuiID fieldId = ImGui::GetItemID();
    const bool fieldHovered = ImGui::IsItemHovered();
    // Колесо над полем меняет значение, а не листает инспектор.
    if (fieldHovered && ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY)) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            if (WheelGestureStart(fieldId)) Snapshot(host);
            const float nv = Clamp(*v + (wheel > 0.0f ? 1.0f : -1.0f) * o.Step * (ImGui::GetIO().KeyShift ? 10.0f : 1.0f), o);
            if (nv != *v) {
                *v = nv;
                changed = true;
            }
        }
    }
    if (o.HasReset && ImGui::IsItemClicked(ImGuiMouseButton_Right) && *v != o.Reset) {
        Snapshot(host);
        *v = o.Reset;
        changed = true;
    }
    if (fieldHovered && !ImGui::IsItemActive())
        ImGui::SetTooltip("%s", o.HasReset ? T("Drag, scroll or double-click to type. Right-click resets")
                                           : T("Drag, scroll or double-click to type"));
    const ImU32 line = EditorTheme::Color32(EditorTheme::Role::Line);
    dl->AddLine(ImVec2(p.x + side + 0.5f, p.y + 3.0f), ImVec2(p.x + side + 0.5f, p.y + h - 3.0f), line);
    dl->AddLine(ImVec2(p.x + width - side - 0.5f, p.y + 3.0f), ImVec2(p.x + width - side - 0.5f, p.y + h - 3.0f), line);
    changed |= StepButton(host, "##inc", "plus", v, 1.0f, o, ImVec2(p.x + width - side, p.y), side,
                          ImDrawFlags_RoundCornersRight);
    if (ImGui::IsItemActive() || ImGui::GetActiveID() == fieldId)
        dl->AddRect(p, ImVec2(p.x + width, p.y + h), EditorTheme::Color32(EditorTheme::Role::Accent), rounding);
    ImGui::PopID();
    return changed;
}

bool Number(EditorHost& host, const char* label, float* v, const NumOpts& o) {
    ImGui::PushID(label);
    Label(label);
    const bool changed = NumberCore(host, "n", v, o);
    ImGui::PopID();
    return changed;
}

bool Integer(EditorHost& host, const char* label, int* v, int min, int max, bool hasReset, int reset) {
    NumOpts o;
    o.Min = (float)min;
    o.Max = (float)max;
    o.HasReset = hasReset;
    o.Reset = (float)reset;
    float f = (float)*v;
    if (!Number(host, label, &f, o)) return false;
    *v = (int)std::lround(f);
    return true;
}

bool SliderNumber(EditorHost& host, const char* label, float* v, float min, float max, const char* format,
                  bool hasReset, float reset) {
    ImGui::PushID(label);
    Label(label);
    const float numW = 58.0f;
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(std::max(40.0f, w - numW - 4.0f));
    bool changed = ImGui::SliderFloat("##s", v, min, max, "", ImGuiSliderFlags_AlwaysClamp);
    Track(host);
    if (hasReset && ImGui::IsItemClicked(ImGuiMouseButton_Right) && *v != reset) {
        Snapshot(host);
        *v = reset;
        changed = true;
    }
    ImGui::SameLine(0.0f, 4.0f);
    ImGui::SetNextItemWidth(numW);
    if (ImGui::InputFloat("##n", v, 0.0f, 0.0f, format)) {
        *v = std::clamp(*v, min, max);
        changed = true;
    }
    Track(host);
    ImGui::PopID();
    return changed;
}

bool Switch(EditorHost& host, const char* label, bool* v) {
    ImGui::PushID(label);
    Label(label);
    const float h = ImGui::GetFrameHeight() * 0.8f;
    const float w = h * 1.9f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float y = p.y + (ImGui::GetFrameHeight() - h) * 0.5f;
    const bool hit = ImGui::InvisibleButton("##sw", ImVec2(w + 34.0f, ImGui::GetFrameHeight()));
    if (hit) {
        Snapshot(host);
        *v = !*v;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec4 on = EditorTheme::Color(EditorTheme::Role::Accent);
    const ImVec4 off = EditorTheme::Color(EditorTheme::Role::Input);
    dl->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + w, y + h), ImGui::GetColorU32(*v ? on : off), h * 0.5f);
    const float k = h - 4.0f;
    const float kx = *v ? p.x + w - 2.0f - k : p.x + 2.0f;
    dl->AddCircleFilled(ImVec2(kx + k * 0.5f, y + h * 0.5f), k * 0.5f,
                        ImGui::GetColorU32(*v ? EditorTheme::Color(EditorTheme::Role::TextOnAccent)
                                              : EditorTheme::Color(EditorTheme::Role::TextDim)));
    dl->AddText(ImVec2(p.x + w + 8.0f, p.y + ImGui::GetStyle().FramePadding.y),
                ImGui::GetColorU32(EditorTheme::Color(*v ? EditorTheme::Role::Text : EditorTheme::Role::TextDim)),
                *v ? T("On") : T("Off"));
    ImGui::PopID();
    return hit;
}

bool Color(EditorHost& host, const char* label, glm::vec4* rgba) {
    ImGui::PushID(label);
    // [■][#HEX][%] целиком: уже — подпись над строкой, а не обрезанный процент.
    Label(label, 176.0f);
    bool changed = false;
    // Образец — тот же, что во всём редакторе: щелчок открывает палитру.
    if (Sage::UI::ColorField4("##sw", &rgba->x, Sage::UI::ColorField_Compact)) changed = true;
    Track(host);
    ImGui::SameLine(0.0f, 4.0f);
    char hex[16];
    std::snprintf(hex, sizeof(hex), "%s", Sage::UI::color::ToHex(&rgba->x).c_str());
    ImGui::SetNextItemWidth(std::max(70.0f, ImGui::GetContentRegionAvail().x - 70.0f));
    if (ImGui::InputText("##hex", hex, sizeof(hex), ImGuiInputTextFlags_CharsUppercase)) {
        float rgb[3];
        if (Sage::UI::color::ParseHex(hex, rgb)) {
            rgba->r = rgb[0];
            rgba->g = rgb[1];
            rgba->b = rgb[2];
            changed = true;
        }
    }
    Track(host);
    ImGui::SameLine(0.0f, 4.0f);
    // Прозрачность — процентами, тем же числом, что и альфа цвета (одна
    // величина, а не Alpha и Opacity порознь).
    float pct = rgba->a * 100.0f;
    ImGui::SetNextItemWidth(std::max(50.0f, ImGui::GetContentRegionAvail().x));
    if (ImGui::DragFloat("##a", &pct, 0.5f, 0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) {
        rgba->a = pct / 100.0f;
        changed = true;
    }
    Track(host);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Opacity"));
    ImGui::PopID();
    return changed;
}

void GroupTitle(const char* title) {
    // Подпись группы над её строками: «Картинка:», «Фон:». Небольшой отступ
    // сверху отделяет группу от предыдущей; до содержимого — обычный шаг.
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
    std::string shown = title;
    if (!shown.empty() && shown.back() != ':') shown += ':';
    ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(EditorTheme::Role::TextDim));
    ImGui::TextUnformatted(shown.c_str());
    ImGui::PopStyleColor();
}

bool Pair(EditorHost& host, const char* id, float* a, float* b, const char* tagA, const char* tagB,
          const NumOpts& o) {
    ImGui::PushID(id);
    const bool changed = PairIn(host, a, b, tagA, tagB, o, ImGui::GetContentRegionAvail().x);
    ImGui::PopID();
    return changed;
}

bool PairRow(EditorHost& host, const char* label, float* a, float* b, const char* tagA, const char* tagB,
             const NumOpts& o) {
    ImGui::PushID(label);
    Label(label, PairNeed(tagA, tagB));
    const bool changed = PairIn(host, a, b, tagA, tagB, o, ImGui::GetContentRegionAvail().x);
    ImGui::PopID();
    return changed;
}

bool Axes(EditorHost& host, const char* id, float* v, int n, const char* const* axes, const NumOpts& o) {
    ImGui::PushID(id);
    bool changed = false;
    for (int i = 0; i + 1 < n; i += 2) {
        ImGui::PushID(i);
        changed |= PairIn(host, &v[i], &v[i + 1], axes[i], axes[i + 1], o, ImGui::GetContentRegionAvail().x);
        ImGui::PopID();
    }
    if (n % 2) {
        Tag(axes[n - 1], ImGui::CalcTextSize(axes[n - 1]).x);
        changed |= NumberCore(host, "last", &v[n - 1], o);
    }
    ImGui::PopID();
    return changed;
}

bool Position(EditorHost& host, const char* title, glm::vec2* v, const NumOpts& o) {
    return PairRow(host, title, &v->x, &v->y, "X", "Y", o);
}

glm::vec2 LinkedSize(glm::vec2 before, glm::vec2 now, const NumOpts& o) {
    if (before.x <= 0.0f || before.y <= 0.0f) return now;
    const float ratio = before.y / before.x;
    if (now.x != before.x) now.y = std::clamp(now.x * ratio, o.Min, o.Max);
    else if (now.y != before.y) now.x = std::clamp(now.y / ratio, o.Min, o.Max);
    return now;
}

bool Size(EditorHost& host, const char* title, glm::vec2* v, const NumOpts& o) {
    ImGui::PushID(title);
    const ImGuiID id = ImGui::GetID("size");
    bool linked = prefs::GetBool(PrefKey("link", id), false);
    const float lock = ImGui::GetFrameHeight();
    // «Размер: W [..] H [..] [замок]» — замок в той же строке, справа.
    Label(title, PairNeed("W", "H") + lock + kTagGap);
    const float width = ImGui::GetContentRegionAvail().x - lock - kTagGap;
    const glm::vec2 before = *v;
    bool changed = false;
    if (!linked) {
        changed = PairIn(host, &v->x, &v->y, "W", "H", o, width);
    } else {
        // Связаны: одно составное поле «W 220 × 56 H» — оба числа правятся,
        // второе следует за первым в прежнем соотношении.
        const float h = ImGui::GetFrameHeight();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + width, p.y + h), ImGui::GetColorU32(ImGuiCol_FrameBg),
                          ImGui::GetStyle().FrameRounding);
        const float tagW = ImGui::CalcTextSize("W").x + 8.0f;
        const float crossW = ImGui::CalcTextSize("\xc3\x97").x + 12.0f;
        const float field = std::max(30.0f, (width - 2.0f * tagW - crossW) * 0.5f);
        const ImU32 dim = EditorTheme::Color32(EditorTheme::Role::TextDim);
        const float ty = p.y + ImGui::GetStyle().FramePadding.y;
        dl->AddText(ImVec2(p.x + 6.0f, ty), dim, "W");
        dl->AddText(ImVec2(p.x + tagW + field + (crossW - ImGui::CalcTextSize("\xc3\x97").x) * 0.5f, ty), dim,
                    "\xc3\x97");
        dl->AddText(ImVec2(p.x + width - tagW + 2.0f, ty), dim, "H");
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
        const float speed = std::max(o.Step * 0.25f, 0.001f);
        ImGui::SetCursorScreenPos(ImVec2(p.x + tagW, p.y));
        ImGui::SetNextItemWidth(field);
        changed |= ImGui::DragFloat("##w", &v->x, speed, o.Min, o.Max, o.Format, ImGuiSliderFlags_AlwaysClamp);
        Track(host);
        ImGui::SetCursorScreenPos(ImVec2(p.x + tagW + field + crossW, p.y));
        ImGui::SetNextItemWidth(field);
        changed |= ImGui::DragFloat("##h", &v->y, speed, o.Min, o.Max, o.Format, ImGuiSliderFlags_AlwaysClamp);
        Track(host);
        ImGui::PopStyleColor();
        if (changed) *v = LinkedSize(before, *v, o);
        // Строка занята составным полем целиком: замок встаёт после «H».
        ImGui::SetCursorScreenPos(p);
        ImGui::Dummy(ImVec2(width, h));
    }
    if (LinkIcon(linked, T("Proportions linked: the height follows the width"), T("Link proportions"))) {
        linked = !linked;
        prefs::SetBool(PrefKey("link", id), linked);
    }
    // Связь пропорций: правили одну сторону — вторая следует за ней.
    if (changed && linked && !(before == *v)) *v = LinkedSize(before, *v, o);
    ImGui::PopID();
    return changed;
}

bool Range(EditorHost& host, const char* title, float* min, float* max, const NumOpts& o) {
    // Min и Max — короткими подписями оси, как X и Y: длинные «Минимум /
    // Максимум» съедали половину ширины полей.
    return PairRow(host, title, min, max, "Min", "Max", o);
}

bool Sides(EditorHost& host, const char* title, glm::vec4* v, const NumOpts& o) {
    ImGui::PushID(title);
    const ImGuiID id = ImGui::GetID("sides");
    bool linked = prefs::GetBool(PrefKey("link", id), false);
    const float lock = ImGui::GetFrameHeight();
    Label(title, PairNeed("L", "R") + lock + kTagGap);
    const float x = ImGui::GetCursorPosX();
    const float width = ImGui::GetContentRegionAvail().x - lock - kTagGap;
    bool changed = false;
    bool toggle = false;
    if (linked) {
        float all = v->x;
        Tag("=", ImGui::CalcTextSize("=").x);
        if (NumberCore(host, "all", &all, o, width - ImGui::CalcTextSize("=").x - kTagGap)) {
            *v = glm::vec4(all);
            changed = true;
        }
        toggle = LinkIcon(true, T("All sides together: one value for every side"), T("Link sides"));
    } else {
        // Лево и право — в строке, верх и низ — в строке под ними.
        ImGui::PushID("lr");
        changed |= PairIn(host, &v->x, &v->z, "L", "R", o, width);
        ImGui::PopID();
        toggle = LinkIcon(false, T("All sides together: one value for every side"), T("Link sides"));
        ImGui::SetCursorPosX(x);
        ImGui::PushID("tb");
        changed |= PairIn(host, &v->y, &v->w, "T", "B", o, width);
        ImGui::PopID();
    }
    if (toggle) {
        linked = !linked;
        prefs::SetBool(PrefKey("link", id), linked);
        if (linked) *v = glm::vec4(v->x);
    }
    ImGui::PopID();
    return changed;
}

bool BeginSection(const char* id, const char* title, bool defaultOpen) {
    const std::string key = std::string("inspector.section.") + id;
    bool open = prefs::GetBool(key, defaultOpen);
    ImGui::Spacing();
    ImGui::PushID(id);
    // ШАПКА РАЗДЕЛА: акцентная метка слева, ЗАГЛАВНЫЕ буквы, стрелка справа —
    // разделы отличаются от групп и полей видом, а не только текстом.
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = ImGui::GetFrameHeight() + 4.0f;
    if (ImGui::InvisibleButton("##head", ImVec2(w, h))) {
        open = !open;
        prefs::SetBool(key, open);
    }
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h),
                      ImGui::GetColorU32(EditorTheme::Color(hovered ? EditorTheme::Role::Hover
                                                                    : EditorTheme::Role::SurfaceAlt)),
                      4.0f, open ? ImDrawFlags_RoundCornersTop : ImDrawFlags_RoundCornersAll);
    dl->AddRectFilled(ImVec2(p.x, p.y + 4.0f), ImVec2(p.x + 3.0f, p.y + h - 4.0f),
                      ImGui::GetColorU32(EditorTheme::Color(EditorTheme::Role::Accent)), 1.5f);
    std::string upper;
    for (const char* c = title; *c; ++c) upper += (*c >= 'a' && *c <= 'z') ? (char)(*c - 32) : *c;
    // Кириллица — ЗАГЛАВНЫМИ тоже: строчные из UTF-8 по двум байтам.
    for (size_t i = 0; i + 1 < upper.size(); ++i) {
        unsigned char a = (unsigned char)upper[i], b = (unsigned char)upper[i + 1];
        if (a == 0xD0 && b >= 0xB0 && b <= 0xBF) { upper[i + 1] = (char)(b - 0x20); ++i; }
        else if (a == 0xD1 && b >= 0x80 && b <= 0x8F) { upper[i] = (char)0xD0; upper[i + 1] = (char)(b + 0x20); ++i; }
        else if (a == 0xD1 && b == 0x91) { upper[i] = (char)0xD0; upper[i + 1] = (char)0x81; ++i; }
        else if (a >= 0xC0) ++i;
    }
    const float ty = p.y + (h - ImGui::GetTextLineHeight()) * 0.5f;
    dl->AddText(ImVec2(p.x + 12.0f, ty), ImGui::GetColorU32(EditorTheme::Color(EditorTheme::Role::Text)), upper.c_str());
    // Стрелка: ⌄ раскрыт, › свёрнут.
    const float ax = p.x + w - 16.0f, ay = p.y + h * 0.5f, s = 4.0f;
    const ImU32 ac = ImGui::GetColorU32(EditorTheme::Color(EditorTheme::Role::TextDim));
    if (open) dl->AddTriangleFilled(ImVec2(ax - s, ay - s * 0.5f), ImVec2(ax + s, ay - s * 0.5f), ImVec2(ax, ay + s * 0.6f), ac);
    else dl->AddTriangleFilled(ImVec2(ax - s * 0.5f, ay - s), ImVec2(ax - s * 0.5f, ay + s), ImVec2(ax + s * 0.6f, ay), ac);
    ImGui::PopID();
    if (!open) return false;
    // ТЕЛО — карточкой: фон чуть светлее панели, поля с отступом.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, EditorTheme::Color(EditorTheme::Role::Surface));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y);
    ImGui::BeginChild((std::string("##sec_") + id).c_str(), ImVec2(0.0f, 0.0f),
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    return true;
}

void EndSection() {
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

// Вложенная группа: строка-заголовок (стрелка и подпись) и содержимое с
// отступом kIndent и тонкой линией слева — дочерние поля явно ВНУТРИ группы.
namespace {
struct GroupMark {
    ImVec2 Start;
};
std::vector<GroupMark>& Groups() {
    static std::vector<GroupMark> s;
    return s;
}
} // namespace

bool SubSection(const char* id, const char* title, bool defaultOpen) {
    const std::string key = std::string("inspector.sub.") + id;
    bool open = prefs::GetBool(key, defaultOpen);
    ImGui::PushID(id);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetFrameHeight();
    const float w = ImGui::GetContentRegionAvail().x;
    if (ImGui::InvisibleButton("##grp", ImVec2(w, h))) {
        open = !open;
        prefs::SetBool(key, open);
    }
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hovered) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), EditorTheme::Color32(EditorTheme::Role::Hover), 3.0f);
    const ImU32 c = EditorTheme::Color32(hovered ? EditorTheme::Role::Text : EditorTheme::Role::TextDim);
    // Стрелка в квадрате шириной kIndent: подпись группы начинается там же,
    // где начнётся её содержимое.
    const float ax = p.x + kIndent * 0.5f, ay = p.y + h * 0.5f, s = 3.5f;
    if (open) dl->AddTriangleFilled(ImVec2(ax - s, ay - s * 0.5f), ImVec2(ax + s, ay - s * 0.5f), ImVec2(ax, ay + s * 0.6f), c);
    else dl->AddTriangleFilled(ImVec2(ax - s * 0.5f, ay - s), ImVec2(ax - s * 0.5f, ay + s), ImVec2(ax + s * 0.6f, ay), c);
    dl->AddText(ImVec2(p.x + kIndent + 2.0f, p.y + ImGui::GetStyle().FramePadding.y), c, title);
    if (!open) return false;
    Groups().push_back({ImVec2(p.x + kIndent * 0.5f, p.y + h)});
    ImGui::Indent(kIndent);
    ImGui::Dummy(ImVec2(0.0f, 1.0f));
    return true;
}

void EndSubSection() {
    ImGui::Unindent(kIndent);
    if (Groups().empty()) return;
    const GroupMark m = Groups().back();
    Groups().pop_back();
    const float bottom = ImGui::GetCursorScreenPos().y - ImGui::GetStyle().ItemSpacing.y;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(m.Start.x, m.Start.y + 2.0f), ImVec2(m.Start.x, bottom),
                                        EditorTheme::Color32(EditorTheme::Role::Line), 1.0f);
    ImGui::Dummy(ImVec2(0.0f, kGroupGap * 0.5f));
}

} // namespace sage::editor::ictl
