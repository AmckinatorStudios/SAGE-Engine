#include "InspectorControls.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

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

// Кнопка −/+ с повтором при удержании; после секунды удержания шаг
// вдесятеро — длинный путь не проходят по единице.
bool StepButton(EditorHost& host, const char* text, float* v, float dir, const NumOpts& o, float side) {
    ImGui::PushItemFlag(ImGuiItemFlags_ButtonRepeat, true);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, ImGui::GetStyle().FramePadding.y));
    const bool hit = ImGui::Button(text, ImVec2(side, 0.0f));
    ImGui::PopStyleVar();
    ImGui::PopItemFlag();
    // Снимок — один на всё удержание: на нажатии, а не на каждом повторе.
    if (ImGui::IsItemActivated()) host.PushUndoSnapshot();
    if (!hit) return false;
    const bool accelerate = ImGui::GetIO().MouseDownDuration[0] > 1.0f;
    const float nv = Clamp(*v + dir * o.Step * (accelerate ? 10.0f : 1.0f), o);
    if (nv == *v) return false;
    *v = nv;
    return true;
}

std::string PrefKey(const char* what, ImGuiID id) { return std::string("inspector.") + what + "." + std::to_string(id); }

// Небольшая подпись оси перед полем («X», «W», «Min»).
void Tag(const char* text, float width) {
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(EditorTheme::Role::TextDim));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::SameLine(0.0f, std::max(4.0f, width - ImGui::CalcTextSize(text).x));
}

// Значок справа в строке подписи группы (замок, связь сторон).
bool TitleIcon(const char* icon, const char* tip, bool active) {
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - ImGui::GetFrameHeight()));
    return EditorIcons::IconOnlyButton(icon, tip, active);
}

} // namespace

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
    // Колонка подписей — доля ширины панели, но не уже читаемого слова.
    return std::clamp(ImGui::GetContentRegionAvail().x * 0.36f, 90.0f, 150.0f);
}

void Label(const char* text) {
    const float x0 = ImGui::GetCursorPosX();
    const float w = LabelWidth();
    ImGui::AlignTextToFramePadding();
    // Подпись не обрезается посередине: длинная уходит в подсказку.
    const char* end = text + std::strlen(text);
    if (const char* hash = std::strstr(text, "##")) end = hash;
    const std::string shown(text, end);
    if (ImGui::CalcTextSize(shown.c_str()).x > w - 6.0f) {
        ImGui::PushClipRect(ImGui::GetCursorScreenPos(),
                            ImVec2(ImGui::GetCursorScreenPos().x + w - 6.0f, ImGui::GetCursorScreenPos().y + 100.0f), true);
        ImGui::TextUnformatted(shown.c_str());
        ImGui::PopClipRect();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", shown.c_str());
    } else {
        ImGui::TextUnformatted(shown.c_str());
    }
    ImGui::SameLine(x0 + w);
    ImGui::SetNextItemWidth(std::max(60.0f, ImGui::GetContentRegionAvail().x));
}

float NumberWidth() { return std::clamp(ImGui::GetContentRegionAvail().x, 110.0f, 170.0f); }

bool NumberCore(EditorHost& host, const char* id, float* v, const NumOpts& o, float width) {
    ImGui::PushID(id);
    const float side = ImGui::GetFrameHeight() * 0.8f;
    const float spacing = 1.0f;
    if (width <= 0.0f) width = NumberWidth();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(spacing, ImGui::GetStyle().ItemSpacing.y));
    bool changed = StepButton(host, "-", v, -1.0f, o, side);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Hold to repeat; after a second the step grows tenfold"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::max(36.0f, width - 2.0f * (side + spacing)));
    // Перетаскивание — по полю, двойной щелчок (или Ctrl+щелчок) — ввод числа.
    if (ImGui::DragFloat("##v", v, std::max(o.Step * 0.25f, 0.001f), o.Min, o.Max, o.Format,
                         ImGuiSliderFlags_AlwaysClamp)) {
        changed = true;
    }
    host.TrackLastImGuiItem();
    const ImGuiID fieldId = ImGui::GetItemID();
    // Колесо над полем меняет значение, а не листает инспектор.
    if (ImGui::IsItemHovered() && ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY)) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            if (WheelGestureStart(fieldId)) host.PushUndoSnapshot();
            const float nv = Clamp(*v + (wheel > 0.0f ? 1.0f : -1.0f) * o.Step * (ImGui::GetIO().KeyShift ? 10.0f : 1.0f), o);
            if (nv != *v) {
                *v = nv;
                changed = true;
            }
        }
    }
    if (o.HasReset && ImGui::IsItemClicked(ImGuiMouseButton_Right) && *v != o.Reset) {
        host.PushUndoSnapshot();
        *v = o.Reset;
        changed = true;
    }
    if (ImGui::IsItemHovered() && !ImGui::IsItemActive())
        ImGui::SetTooltip("%s", o.HasReset ? T("Drag, scroll or double-click to type. Right-click resets")
                                           : T("Drag, scroll or double-click to type"));
    ImGui::SameLine();
    changed |= StepButton(host, "+", v, 1.0f, o, side);
    ImGui::PopStyleVar();
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
    host.TrackLastImGuiItem();
    if (hasReset && ImGui::IsItemClicked(ImGuiMouseButton_Right) && *v != reset) {
        host.PushUndoSnapshot();
        *v = reset;
        changed = true;
    }
    ImGui::SameLine(0.0f, 4.0f);
    ImGui::SetNextItemWidth(numW);
    if (ImGui::InputFloat("##n", v, 0.0f, 0.0f, format)) {
        *v = std::clamp(*v, min, max);
        changed = true;
    }
    host.TrackLastImGuiItem();
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
        host.PushUndoSnapshot();
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
    Label(label);
    bool changed = false;
    // Образец — тот же, что во всём редакторе: щелчок открывает палитру.
    if (Sage::UI::ColorField4("##sw", &rgba->x, Sage::UI::ColorField_Compact)) changed = true;
    host.TrackLastImGuiItem();
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
    host.TrackLastImGuiItem();
    ImGui::SameLine(0.0f, 4.0f);
    // Прозрачность — процентами, тем же числом, что и альфа цвета (одна
    // величина, а не Alpha и Opacity порознь).
    float pct = rgba->a * 100.0f;
    ImGui::SetNextItemWidth(std::max(50.0f, ImGui::GetContentRegionAvail().x));
    if (ImGui::DragFloat("##a", &pct, 0.5f, 0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) {
        rgba->a = pct / 100.0f;
        changed = true;
    }
    host.TrackLastImGuiItem();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Opacity"));
    ImGui::PopID();
    return changed;
}

void GroupTitle(const char* title) {
    ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(EditorTheme::Role::TextDim));
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
}

bool Pair(EditorHost& host, const char* id, float* a, float* b, const char* tagA, const char* tagB,
          const NumOpts& o) {
    ImGui::PushID(id);
    const float tag = std::max(ImGui::CalcTextSize(tagA).x, ImGui::CalcTextSize(tagB).x) + 6.0f;
    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2.0f) * 0.5f;
    const float field = std::min(std::max(80.0f, half - tag), 170.0f);
    Tag(tagA, tag);
    bool changed = NumberCore(host, "a", a, o, field);
    ImGui::SameLine(0.0f, std::max(ImGui::GetStyle().ItemSpacing.x * 2.0f, half - tag - field + 12.0f));
    Tag(tagB, tag);
    changed |= NumberCore(host, "b", b, o, field);
    ImGui::PopID();
    return changed;
}

bool Axes(EditorHost& host, const char* id, float* v, int n, const char* const* axes, const NumOpts& o) {
    ImGui::PushID(id);
    bool changed = false;
    for (int i = 0; i + 1 < n; i += 2) {
        ImGui::PushID(i);
        changed |= Pair(host, "p", &v[i], &v[i + 1], axes[i], axes[i + 1], o);
        ImGui::PopID();
    }
    if (n % 2) {
        Tag(axes[n - 1], ImGui::CalcTextSize(axes[n - 1]).x + 6.0f);
        changed |= NumberCore(host, "last", &v[n - 1], o);
    }
    ImGui::PopID();
    return changed;
}

bool Position(EditorHost& host, const char* title, glm::vec2* v, const NumOpts& o) {
    GroupTitle(title);
    return Pair(host, title, &v->x, &v->y, "X", "Y", o);
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
    GroupTitle(title);
    if (TitleIcon(linked ? "lock" : "unlock",
                  linked ? T("Proportions linked: the height follows the width") : T("Link proportions"), linked)) {
        linked = !linked;
        prefs::SetBool(PrefKey("link", id), linked);
    }
    const glm::vec2 before = *v;
    const bool changed = Pair(host, "wh", &v->x, &v->y, "W", "H", o);
    // Связь пропорций: правили одну сторону — вторая следует за ней.
    if (changed && linked) *v = LinkedSize(before, *v, o);
    ImGui::PopID();
    return changed;
}

bool Range(EditorHost& host, const char* title, float* min, float* max, const NumOpts& o) {
    GroupTitle(title);
    return Pair(host, title, min, max, T("Min"), T("Max"), o);
}

bool Sides(EditorHost& host, const char* title, glm::vec4* v, const NumOpts& o) {
    ImGui::PushID(title);
    const ImGuiID id = ImGui::GetID("sides");
    bool linked = prefs::GetBool(PrefKey("link", id), false);
    GroupTitle(title);
    if (TitleIcon(linked ? "lock" : "unlock",
                  linked ? T("All sides together: one value for every side") : T("Link sides"), linked)) {
        linked = !linked;
        prefs::SetBool(PrefKey("link", id), linked);
        if (linked) *v = glm::vec4(v->x);
    }
    bool changed = false;
    if (linked) {
        float all = v->x;
        Tag("=", 14.0f);
        if (NumberCore(host, "all", &all, o)) {
            *v = glm::vec4(all);
            changed = true;
        }
    } else {
        // Лево и право — в строке, верх и низ — в строке: стороны парами.
        changed |= Pair(host, "lr", &v->x, &v->z, "L", "R", o);
        changed |= Pair(host, "tb", &v->y, &v->w, "T", "B", o);
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

bool SubSection(const char* id, const char* title, bool defaultOpen) {
    const std::string key = std::string("inspector.sub.") + id;
    ImGui::SetNextItemOpen(prefs::GetBool(key, defaultOpen), ImGuiCond_Once);
    const std::string label = std::string(title) + "###sub_" + id;
    ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(EditorTheme::Role::TextDim));
    const bool open = ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth);
    ImGui::PopStyleColor();
    if (ImGui::IsItemToggledOpen()) prefs::SetBool(key, open);
    return open;
}

void EndSubSection() { ImGui::TreePop(); }

} // namespace sage::editor::ictl
