#include "ParticleUi.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "imgui.h"

#include "Localization.h"
#include "ui/ColorPicker.h"

using namespace sage::fx;

namespace sage::editor {

namespace {

void Before(const ParticleUiHooks& h) {
    if (h.BeforeEdit) h.BeforeEdit();
}
void Track(const ParticleUiHooks& h) {
    if (h.TrackItem) h.TrackItem();
}

// Перетаскиваемые значения: отмену ведёт TrackItem (снимок — при захвате).
bool DragF(const char* label, float* v, float speed, float lo, float hi, const char* fmt,
           const ParticleUiHooks& h) {
    const bool c = ImGui::DragFloat(label, v, speed, lo, hi, fmt);
    Track(h);
    return c;
}
bool DragI(const char* label, int* v, float speed, int lo, int hi, const ParticleUiHooks& h) {
    const bool c = ImGui::DragInt(label, v, speed, lo, hi);
    Track(h);
    return c;
}
bool Drag3(const char* label, glm::vec3& v, float speed, const ParticleUiHooks& h) {
    const bool c = ImGui::DragFloat3(label, &v.x, speed);
    Track(h);
    return c;
}
bool RangeF(const char* label, Range& r, float speed, float lo, float hi, const char* fmt,
            const ParticleUiHooks& h) {
    const bool c = ImGui::DragFloatRange2(label, &r.Min, &r.Max, speed, lo, hi, fmt, fmt);
    Track(h);
    return c;
}
// Дискретные правки: снимок ДО изменения.
bool Check(const char* label, bool& v, const ParticleUiHooks& h) {
    bool tmp = v;
    if (!ImGui::Checkbox(label, &tmp)) return false;
    Before(h);
    v = tmp;
    return true;
}
template <typename E>
bool Combo(const char* label, E& v, const char* const* items, int count, const ParticleUiHooks& h) {
    int i = (int)v;
    if (!ImGui::Combo(label, &i, items, count)) return false;
    Before(h);
    v = (E)i;
    return true;
}
bool Color4(const char* label, glm::vec4& c, const ParticleUiHooks& h) {
    const bool changed = Sage::UI::ColorField4(label, &c.x);
    Track(h);
    return changed;
}

// Раздел-модуль: заголовок и, если модуль выключаемый, галка на нём справа.
bool Section(const char* label, bool* enabled, bool& changed, const ParticleUiHooks& h,
             bool defaultOpen = false) {
    ImGui::PushID(label);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_AllowOverlap;
    if (defaultOpen) flags |= ImGuiTreeNodeFlags_DefaultOpen;
    const bool open = ImGui::CollapsingHeader(label, flags);
    if (enabled) {
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::GetFrameHeight() - 4.0f);
        bool on = *enabled;
        if (ImGui::Checkbox("##on", &on)) {
            Before(h);
            *enabled = on;
            changed = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Module on / off"));
    }
    ImGui::PopID();
    // Выключенный модуль виден (чтобы его настроить заранее), но приглушён.
    if (open && enabled && !*enabled) ImGui::TextDisabled("%s", T("Off: settings are kept but do not act"));
    return open;
}

// Форма кривой одним щелчком — стартовая точка, дальше ключи правятся мышью.
void CurveShape(Curve& c, int shape) {
    switch (shape) {
        case 0: c.Keys = {{0.0f, 1.0f}, {1.0f, 1.0f}}; break;                         // ровно
        case 1: c.Keys = {{0.0f, 0.0f}, {1.0f, 1.0f}}; break;                         // рост
        case 2: c.Keys = {{0.0f, 1.0f}, {1.0f, 0.0f}}; break;                         // спад
        case 3: c.Keys = {{0.0f, 0.0f}, {0.5f, 1.0f}, {1.0f, 0.0f}}; break;           // горб
        case 4: c.Keys = {{0.0f, 0.0f}, {0.15f, 1.0f}, {1.0f, 0.0f}}; break;          // вспышка
        case 5: c.Keys = {{0.0f, 0.3f}, {0.3f, 1.0f}, {1.0f, 1.2f}}; break;           // разбухание
    }
}

} // namespace

// --- Кривая ----------------------------------------------------------------------

bool DrawCurve(const char* label, Curve& curve, float lo, float hi, const ParticleUiHooks& h) {
    bool changed = false;
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);

    for (const glm::vec2& k : curve.Keys) {
        lo = std::min(lo, k.y);
        hi = std::max(hi, k.y);
    }
    if (hi - lo < 1e-4f) hi = lo + 1.0f;

    const float width = std::max(120.0f, ImGui::GetContentRegionAvail().x);
    const float height = 70.0f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + width, p0.y + height);
    ImGui::InvisibleButton("##curve", ImVec2(width, height),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    const bool activated = ImGui::IsItemActivated();
    const bool deactivated = ImGui::IsItemDeactivated();

    auto toScreen = [&](const glm::vec2& k) {
        return ImVec2(p0.x + k.x * width, p1.y - (k.y - lo) / (hi - lo) * height);
    };
    auto fromScreen = [&](const ImVec2& s) {
        return glm::vec2(std::clamp((s.x - p0.x) / width, 0.0f, 1.0f),
                         lo + (p1.y - s.y) / height * (hi - lo));
    };

    ImGuiStorage* st = ImGui::GetStateStorage();
    const ImGuiID selId = ImGui::GetID("##sel");
    int sel = st->GetInt(selId, -1);
    if (sel >= (int)curve.Keys.size()) sel = -1;

    const ImVec2 mouse = ImGui::GetIO().MousePos;
    int near = -1;
    float best = 7.0f * 7.0f;
    for (int i = 0; i < (int)curve.Keys.size(); ++i) {
        const ImVec2 s = toScreen(curve.Keys[(size_t)i]);
        const float d = (s.x - mouse.x) * (s.x - mouse.x) + (s.y - mouse.y) * (s.y - mouse.y);
        if (d < best) { best = d; near = i; }
    }

    // Пустая кривая — «единица»; первая правка делает её явной.
    auto materialize = [&] {
        if (curve.Keys.empty()) curve.Keys = {{0.0f, 1.0f}, {1.0f, 1.0f}};
    };

    if (activated && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && near >= 0) {
        Before(h);
        sel = near;
    }
    if (active && sel >= 0 && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
        glm::vec2 k = fromScreen(mouse);
        // Ключ не перепрыгивает соседей: иначе кривая «переворачивается» под
        // мышью, и выбранный ключ оказывается другим.
        if (sel > 0) k.x = std::max(k.x, curve.Keys[(size_t)sel - 1].x);
        if (sel + 1 < (int)curve.Keys.size()) k.x = std::min(k.x, curve.Keys[(size_t)sel + 1].x);
        if (k != curve.Keys[(size_t)sel]) {
            curve.Keys[(size_t)sel] = k;
            changed = true;
        }
    }
    if (deactivated) curve.Sort();
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && near < 0) {
        Before(h);
        materialize();
        const glm::vec2 k = fromScreen(mouse);
        curve.Keys.push_back(k);
        curve.Sort();
        changed = true;
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && near >= 0 && curve.Keys.size() > 1) {
        Before(h);
        curve.Keys.erase(curve.Keys.begin() + near);
        sel = -1;
        changed = true;
    }
    st->SetInt(selId, sel);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ImGuiCol_FrameBg), 3.0f);
    for (int i = 1; i < 4; ++i) {
        const float x = p0.x + width * i / 4.0f;
        dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), ImGui::GetColorU32(ImGuiCol_Border, 0.5f));
    }
    if (lo < 0.0f && hi > 0.0f) {
        const float y = toScreen({0.0f, 0.0f}).y;
        dl->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), ImGui::GetColorU32(ImGuiCol_Border));
    }
    const ImU32 lineCol = ImGui::GetColorU32(ImVec4(0.45f, 0.78f, 1.0f, 1.0f));
    ImVec2 prev;
    for (int x = 0; x <= (int)width; x += 2) {
        const float t = (float)x / width;
        const ImVec2 s = toScreen({t, curve.Evaluate(t)});
        if (x > 0) dl->AddLine(prev, s, lineCol, 2.0f);
        prev = s;
    }
    for (int i = 0; i < (int)curve.Keys.size(); ++i) {
        const ImVec2 s = toScreen(curve.Keys[(size_t)i]);
        const bool on = i == sel || i == near;
        dl->AddCircleFilled(s, on ? 5.0f : 4.0f,
                            on ? ImGui::GetColorU32(ImVec4(1.0f, 0.75f, 0.2f, 1.0f)) : lineCol);
    }
    char range[64];
    std::snprintf(range, sizeof(range), "%.2g", hi);
    dl->AddText(ImVec2(p0.x + 3.0f, p0.y + 1.0f), ImGui::GetColorU32(ImGuiCol_TextDisabled), range);
    std::snprintf(range, sizeof(range), "%.2g", lo);
    dl->AddText(ImVec2(p0.x + 3.0f, p1.y - ImGui::GetTextLineHeight() - 1.0f),
                ImGui::GetColorU32(ImGuiCol_TextDisabled), range);
    if (hovered) ImGui::SetTooltip("%s", T("Double-click: add a key. Right-click a key: remove. Drag: move."));

    // Выбранный ключ — числами: мышью точно не попасть в 0.35.
    if (sel >= 0 && sel < (int)curve.Keys.size()) {
        glm::vec2 k = curve.Keys[(size_t)sel];
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
        if (ImGui::DragFloat2(T("Key (time, value)"), &k.x, 0.01f)) {
            k.x = std::clamp(k.x, 0.0f, 1.0f);
            curve.Keys[(size_t)sel] = k;
            changed = true;
        }
        Track(h);
        if (ImGui::IsItemDeactivatedAfterEdit()) curve.Sort();
    }
    const char* shapes[] = {T("Flat"), T("Rise"), T("Fall"), T("Hump"), T("Flash"), T("Swell")};
    int shape = -1;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
    if (ImGui::Combo(T("Start from"), &shape, shapes, 6) && shape >= 0) {
        Before(h);
        CurveShape(curve, shape);
        changed = true;
    }
    ImGui::PopID();
    return changed;
}

// --- Градиент ----------------------------------------------------------------------

bool DrawGradient(const char* label, Gradient& g, const ParticleUiHooks& h) {
    bool changed = false;
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    if (g.Colors.empty()) g.Colors = {{0.0f, glm::vec3(1.0f)}};
    if (g.Alphas.empty()) g.Alphas = {{0.0f, 1.0f}};

    const float width = std::max(120.0f, ImGui::GetContentRegionAvail().x);
    const float marker = 10.0f, bar = 20.0f;
    const float height = marker * 2.0f + bar;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float barTop = p0.y + marker, barBottom = barTop + bar;
    ImGui::InvisibleButton("##gradient", ImVec2(width, height),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    const bool activated = ImGui::IsItemActivated();
    const bool deactivated = ImGui::IsItemDeactivated();

    ImGuiStorage* st = ImGui::GetStateStorage();
    const ImGuiID typeId = ImGui::GetID("##seltype"), idxId = ImGui::GetID("##selidx");
    int selType = st->GetInt(typeId, 1);   // 1 — цвет, 2 — прозрачность
    int selIdx = st->GetInt(idxId, 0);

    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool alphaZone = mouse.y < barTop;
    const bool colorZone = mouse.y > barBottom;
    auto nearest = [&](bool alpha) {
        int best = -1;
        float bd = 7.0f;
        const size_t n = alpha ? g.Alphas.size() : g.Colors.size();
        for (size_t i = 0; i < n; ++i) {
            const float t = alpha ? g.Alphas[i].T : g.Colors[i].T;
            const float d = std::abs(p0.x + t * width - mouse.x);
            if (d < bd) { bd = d; best = (int)i; }
        }
        return best;
    };
    const int nearAlpha = alphaZone ? nearest(true) : -1;
    const int nearColor = colorZone ? nearest(false) : -1;
    const float mouseT = std::clamp((mouse.x - p0.x) / width, 0.0f, 1.0f);

    if (activated && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (nearAlpha >= 0) { Before(h); selType = 2; selIdx = nearAlpha; }
        else if (nearColor >= 0) { Before(h); selType = 1; selIdx = nearColor; }
    }
    if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
        if (selType == 2 && selIdx < (int)g.Alphas.size()) { g.Alphas[(size_t)selIdx].T = mouseT; changed = true; }
        if (selType == 1 && selIdx < (int)g.Colors.size()) { g.Colors[(size_t)selIdx].T = mouseT; changed = true; }
    }
    if (deactivated) {
        // После сортировки выбранный ключ ищется заново по месту — иначе
        // выделение прыгнуло бы на соседа.
        const float t = selType == 2 && selIdx < (int)g.Alphas.size() ? g.Alphas[(size_t)selIdx].T
                        : selType == 1 && selIdx < (int)g.Colors.size() ? g.Colors[(size_t)selIdx].T : -1.0f;
        g.Sort();
        if (t >= 0.0f) {
            if (selType == 2)
                for (size_t i = 0; i < g.Alphas.size(); ++i) if (g.Alphas[i].T == t) selIdx = (int)i;
            if (selType == 1)
                for (size_t i = 0; i < g.Colors.size(); ++i) if (g.Colors[i].T == t) selIdx = (int)i;
        }
    }
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && nearAlpha < 0 && nearColor < 0) {
        Before(h);
        const glm::vec4 v = g.Evaluate(mouseT);
        if (alphaZone) {
            g.Alphas.push_back({mouseT, v.a});
            selType = 2;
        } else {
            g.Colors.push_back({mouseT, glm::vec3(v)});
            selType = 1;
        }
        g.Sort();
        selIdx = 0;
        if (selType == 2) for (size_t i = 0; i < g.Alphas.size(); ++i) { if (g.Alphas[i].T == mouseT) selIdx = (int)i; }
        else for (size_t i = 0; i < g.Colors.size(); ++i) { if (g.Colors[i].T == mouseT) selIdx = (int)i; }
        changed = true;
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        if (nearAlpha >= 0 && g.Alphas.size() > 1) {
            Before(h);
            g.Alphas.erase(g.Alphas.begin() + nearAlpha);
            selIdx = 0;
            changed = true;
        } else if (nearColor >= 0 && g.Colors.size() > 1) {
            Before(h);
            g.Colors.erase(g.Colors.begin() + nearColor);
            selIdx = 0;
            changed = true;
        }
    }
    st->SetInt(typeId, selType);
    st->SetInt(idxId, selIdx);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Шахматка под полосой: без неё прозрачность не отличить от тёмного цвета.
    const float cell = bar * 0.5f;
    for (float x = 0.0f; x < width; x += cell) {
        for (int row = 0; row < 2; ++row) {
            const bool dark = ((int)(x / cell) + row) % 2 == 0;
            dl->AddRectFilled(ImVec2(p0.x + x, barTop + row * cell),
                              ImVec2(p0.x + std::min(x + cell, width), barTop + (row + 1) * cell),
                              dark ? IM_COL32(90, 90, 90, 255) : IM_COL32(160, 160, 160, 255));
        }
    }
    for (int x = 0; x < (int)width; x += 2) {
        const glm::vec4 c = glm::clamp(g.Evaluate((float)x / width), 0.0f, 1.0f);
        dl->AddRectFilled(ImVec2(p0.x + x, barTop), ImVec2(p0.x + std::min((float)x + 2.0f, width), barBottom),
                          ImGui::GetColorU32(ImVec4(c.r, c.g, c.b, c.a)));
    }
    dl->AddRect(ImVec2(p0.x, barTop), ImVec2(p0.x + width, barBottom), ImGui::GetColorU32(ImGuiCol_Border));
    const ImU32 selCol = ImGui::GetColorU32(ImVec4(1.0f, 0.75f, 0.2f, 1.0f));
    for (size_t i = 0; i < g.Alphas.size(); ++i) {
        const float x = p0.x + g.Alphas[i].T * width;
        const int a = (int)(std::clamp(g.Alphas[i].Alpha, 0.0f, 1.0f) * 255.0f);
        dl->AddTriangleFilled(ImVec2(x - 5, p0.y), ImVec2(x + 5, p0.y), ImVec2(x, barTop), IM_COL32(a, a, a, 255));
        if (selType == 2 && (int)i == selIdx)
            dl->AddTriangle(ImVec2(x - 5, p0.y), ImVec2(x + 5, p0.y), ImVec2(x, barTop), selCol, 2.0f);
    }
    for (size_t i = 0; i < g.Colors.size(); ++i) {
        const float x = p0.x + g.Colors[i].T * width;
        const glm::vec3 c = glm::clamp(g.Colors[i].Color, 0.0f, 1.0f);
        dl->AddTriangleFilled(ImVec2(x, barBottom), ImVec2(x + 5, barBottom + marker), ImVec2(x - 5, barBottom + marker),
                              ImGui::GetColorU32(ImVec4(c.r, c.g, c.b, 1.0f)));
        if (selType == 1 && (int)i == selIdx)
            dl->AddTriangle(ImVec2(x, barBottom), ImVec2(x + 5, barBottom + marker), ImVec2(x - 5, barBottom + marker),
                            selCol, 2.0f);
    }
    if (hovered)
        ImGui::SetTooltip("%s", T("Top: transparency keys, bottom: colour keys. Double-click adds, right-click removes."));

    if (selType == 1 && selIdx < (int)g.Colors.size()) {
        Gradient::ColorKey& k = g.Colors[(size_t)selIdx];
        if (Sage::UI::ColorField3(T("Key colour"), &k.Color.x)) changed = true;
        Track(h);
        if (DragF(T("Key position"), &k.T, 0.005f, 0.0f, 1.0f, "%.2f", h)) changed = true;
        if (ImGui::IsItemDeactivatedAfterEdit()) g.Sort();
    } else if (selType == 2 && selIdx < (int)g.Alphas.size()) {
        Gradient::AlphaKey& k = g.Alphas[(size_t)selIdx];
        if (ImGui::SliderFloat(T("Key opacity"), &k.Alpha, 0.0f, 1.0f, "%.2f")) changed = true;
        Track(h);
        if (DragF(T("Key position"), &k.T, 0.005f, 0.0f, 1.0f, "%.2f", h)) changed = true;
        if (ImGui::IsItemDeactivatedAfterEdit()) g.Sort();
    }
    ImGui::PopID();
    return changed;
}

// --- Кадры по номеру ------------------------------------------------------------------

std::vector<std::string> NumberedSequence(const std::string& anyFrame,
                                          const std::function<bool(const std::string&)>& exists) {
    std::vector<std::string> out;
    const size_t dot = anyFrame.find_last_of('.');
    const size_t slash = anyFrame.find_last_of("/\\");
    const size_t stemEnd = (dot == std::string::npos || (slash != std::string::npos && dot < slash)) ? anyFrame.size() : dot;
    size_t digits = stemEnd;
    while (digits > 0 && std::isdigit((unsigned char)anyFrame[digits - 1])) --digits;
    if (digits == stemEnd || !exists) return out;
    const std::string prefix = anyFrame.substr(0, digits);
    const std::string suffix = anyFrame.substr(stemEnd);
    const size_t width = stemEnd - digits;
    auto name = [&](int i, bool padded) {
        std::string n = std::to_string(i);
        if (padded && n.size() < width) n = std::string(width - n.size(), '0') + n;
        return prefix + n + suffix;
    };
    // С ведущими нулями (smoke_03) или без (smoke_3): как записан данный кадр.
    const bool padded = width > 1 && anyFrame[digits] == '0';
    // Нумерация бывает с нуля или с единицы.
    int first = exists(name(0, padded)) ? 0 : (exists(name(1, padded)) ? 1 : -1);
    if (first < 0) return out;
    for (int i = first; i < first + 4096; ++i) {
        const std::string f = name(i, padded);
        if (!exists(f)) break;
        out.push_back(f);
    }
    return out;
}

// --- Весь эффект ------------------------------------------------------------------------

bool DrawParticleEffect(ParticleEffect& fx, const ParticleUiHooks& h) {
    bool changed = false;
    ImGui::PushID("particle-effect");

    // --- Основное ---
    if (Section(T("Main"), nullptr, changed, h, true)) {
        changed |= DragF(T("Duration"), &fx.Duration, 0.05f, 0.05f, 600.0f, "%.2f s", h);
        changed |= Check(T("Loop"), fx.Loop, h);
        ImGui::SameLine();
        changed |= Check(T("Prewarm"), fx.Prewarm, h);
        changed |= DragF(T("Start delay"), &fx.StartDelay, 0.05f, 0.0f, 600.0f, "%.2f s", h);
        changed |= RangeF(T("Lifetime"), fx.StartLifetime, 0.02f, 0.01f, 600.0f, "%.2f s", h);
        changed |= RangeF(T("Start speed"), fx.StartSpeed, 0.02f, -100.0f, 100.0f, "%.2f", h);
        changed |= RangeF(T("Start size"), fx.StartSize, 0.005f, 0.0f, 100.0f, "%.3f", h);
        changed |= RangeF(T("Start rotation"), fx.StartRotation, 1.0f, -360.0f, 360.0f, "%.0f°", h);
        changed |= Color4(T("Start colour A"), fx.StartColorA, h);
        changed |= Color4(T("Start colour B"), fx.StartColorB, h);
        ImGui::TextDisabled("%s", T("Each particle picks a colour between A and B."));
        changed |= DragF(T("Gravity"), &fx.GravityScale, 0.01f, -10.0f, 10.0f, "%.2f g", h);
        changed |= DragF(T("Inherit emitter velocity"), &fx.InheritVelocity, 0.01f, -2.0f, 2.0f, "%.2f", h);
        const char* spaces[] = {T("World"), T("Local (moves with the object)")};
        changed |= Combo(T("Simulation space"), fx.Space, spaces, 2, h);
        changed |= DragF(T("Simulation speed"), &fx.SimulationSpeed, 0.01f, 0.0f, 10.0f, "%.2fx", h);
        changed |= DragI(T("Max particles"), &fx.MaxParticles, 5.0f, 1, 65536, h);
    }

    // --- Испускание ---
    if (Section(T("Emission"), nullptr, changed, h, true)) {
        changed |= DragF(T("Rate over time"), &fx.RateOverTime, 0.2f, 0.0f, 10000.0f, "%.1f /s", h);
        changed |= DragF(T("Rate over distance"), &fx.RateOverDistance, 0.1f, 0.0f, 1000.0f, "%.1f /m", h);
        int remove = -1;
        for (size_t i = 0; i < fx.Bursts.size(); ++i) {
            Burst& b = fx.Bursts[i];
            ImGui::PushID((int)i);
            ImGui::Separator();
            ImGui::Text(T("Burst %d"), (int)i + 1);
            changed |= DragF(T("At"), &b.Time, 0.02f, 0.0f, 600.0f, "%.2f s", h);
            changed |= ImGui::DragIntRange2(T("Count"), &b.CountMin, &b.CountMax, 1.0f, 0, 10000);
            Track(h);
            changed |= DragI(T("Repeats (0 — endless)"), &b.Cycles, 0.1f, 0, 10000, h);
            changed |= DragF(T("Every"), &b.Interval, 0.01f, 0.01f, 600.0f, "%.2f s", h);
            if (ImGui::Button(T("Remove burst"))) remove = (int)i;
            ImGui::PopID();
        }
        if (remove >= 0) {
            Before(h);
            fx.Bursts.erase(fx.Bursts.begin() + remove);
            changed = true;
        }
        if (ImGui::Button(T("Add burst"))) {
            Before(h);
            fx.Bursts.push_back(Burst{});
            changed = true;
        }
    }

    // --- Форма ---
    if (Section(T("Shape"), nullptr, changed, h, true)) {
        const char* shapes[] = {T("Point"), T("Sphere"), T("Hemisphere"), T("Cone"), T("Box"), T("Circle"), T("Edge")};
        changed |= Combo(T("Shape##emit"), fx.Shape, shapes, 7, h);
        switch (fx.Shape) {
            case EmitShape::Point: break;
            case EmitShape::Box:
                changed |= Drag3(T("Box size"), fx.BoxSize, 0.02f, h);
                break;
            case EmitShape::Edge:
                changed |= DragF(T("Length"), &fx.EdgeLength, 0.02f, 0.0f, 1000.0f, "%.2f m", h);
                break;
            case EmitShape::Cone:
                changed |= DragF(T("Angle"), &fx.ConeAngle, 0.5f, 0.0f, 90.0f, "%.1f°", h);
                [[fallthrough]];
            default:
                changed |= DragF(T("Radius"), &fx.Radius, 0.01f, 0.0f, 1000.0f, "%.2f m", h);
                changed |= DragF(T("Arc"), &fx.Arc, 1.0f, 0.0f, 360.0f, "%.0f°", h);
                break;
        }
        if (fx.Shape != EmitShape::Point && fx.Shape != EmitShape::Edge)
            changed |= Check(T("From the surface only"), fx.FromShell, h);
        changed |= DragF(T("Randomize direction"), &fx.RandomizeDirection, 0.01f, 0.0f, 1.0f, "%.2f", h);
        changed |= Drag3(T("Offset"), fx.ShapeOffset, 0.02f, h);
        changed |= Drag3(T("Rotation"), fx.ShapeRotation, 0.5f, h);
    }

    // --- Скорость по времени жизни ---
    if (Section(T("Velocity over lifetime"), &fx.UseVelocity, changed, h)) {
        changed |= Drag3(T("Linear"), fx.LinearVelocity, 0.02f, h);
        changed |= DragF(T("Orbital"), &fx.Orbital, 0.5f, -3600.0f, 3600.0f, "%.0f°/s", h);
        changed |= DragF(T("Radial"), &fx.Radial, 0.02f, -100.0f, 100.0f, "%.2f m/s", h);
        changed |= DrawCurve(T("Speed multiplier"), fx.SpeedOverLifetime, 0.0f, 1.0f, h);
    }

    // --- Силы ---
    if (Section(T("Forces"), &fx.UseForces, changed, h)) {
        changed |= Drag3(T("Constant force"), fx.Force, 0.02f, h);
        ImGui::TextDisabled("%s", T("Up for buoyant smoke, sideways for a draft."));
        changed |= Drag3(T("Wind"), fx.Wind, 0.02f, h);
        changed |= DragF(T("Wind grip"), &fx.WindInfluence, 0.01f, 0.0f, 20.0f, "%.2f", h);
        changed |= DragF(T("Gusts"), &fx.Gustiness, 0.01f, 0.0f, 1.0f, "%.2f", h);
        changed |= DragF(T("Drag"), &fx.Drag, 0.01f, 0.0f, 50.0f, "%.2f", h);
        changed |= DragF(T("Speed limit"), &fx.MaxSpeed, 0.05f, 0.0f, 1000.0f, "%.2f m/s", h);
        changed |= DragF(T("Pull to centre"), &fx.Attraction, 0.02f, -100.0f, 100.0f, "%.2f", h);
    }

    // --- Турбулентность ---
    if (Section(T("Turbulence"), &fx.UseNoise, changed, h)) {
        changed |= DragF(T("Strength"), &fx.NoiseStrength, 0.02f, 0.0f, 100.0f, "%.2f", h);
        changed |= DragF(T("Frequency"), &fx.NoiseFrequency, 0.01f, 0.0f, 20.0f, "%.2f", h);
        changed |= DragF(T("Scroll speed"), &fx.NoiseScroll, 0.01f, 0.0f, 20.0f, "%.2f", h);
        changed |= DragI(T("Octaves"), &fx.NoiseOctaves, 0.05f, 1, 6, h);
    }

    // --- Размер ---
    if (Section(T("Size over lifetime"), &fx.UseSizeOverLifetime, changed, h))
        changed |= DrawCurve(T("Size multiplier"), fx.SizeOverLifetime, 0.0f, 1.0f, h);
    if (Section(T("Size by speed"), &fx.UseSizeBySpeed, changed, h)) {
        changed |= RangeF(T("Speed range"), fx.SizeBySpeedRange, 0.05f, 0.0f, 1000.0f, "%.2f", h);
        changed |= DrawCurve(T("Size multiplier##speed"), fx.SizeBySpeed, 0.0f, 1.0f, h);
    }

    // --- Цвет ---
    if (Section(T("Colour over lifetime"), &fx.UseColorOverLifetime, changed, h))
        changed |= DrawGradient(T("Colour##life"), fx.ColorOverLifetime, h);
    if (Section(T("Colour by speed"), &fx.UseColorBySpeed, changed, h)) {
        changed |= RangeF(T("Speed range##colour"), fx.ColorBySpeedRange, 0.05f, 0.0f, 1000.0f, "%.2f", h);
        changed |= DrawGradient(T("Colour##speed"), fx.ColorBySpeed, h);
    }

    // --- Вращение ---
    if (Section(T("Rotation"), &fx.UseRotation, changed, h)) {
        changed |= RangeF(T("Angular velocity"), fx.AngularVelocity, 1.0f, -3600.0f, 3600.0f, "%.0f°/s", h);
        changed |= DrawCurve(T("Spin multiplier"), fx.RotationOverLifetime, 0.0f, 1.0f, h);
    }
    changed |= Check(T("Face the direction of motion"), fx.AlignToVelocity, h);

    // --- Столкновения ---
    if (Section(T("Collision"), &fx.UseCollision, changed, h)) {
        const char* modes[] = {T("Plane"), T("World (physics)")};
        changed |= Combo(T("Collide with"), fx.Collision, modes, 2, h);
        if (fx.Collision == CollisionMode::Plane)
            changed |= DragF(T("Plane height"), &fx.PlaneHeight, 0.02f, -10000.0f, 10000.0f, "%.2f m", h);
        changed |= DragF(T("Bounce"), &fx.Bounce, 0.01f, 0.0f, 2.0f, "%.2f", h);
        changed |= DragF(T("Friction"), &fx.Friction, 0.01f, 0.0f, 1.0f, "%.2f", h);
        changed |= DragF(T("Lifetime lost on hit"), &fx.LifetimeLoss, 0.01f, 0.0f, 1.0f, "%.2f", h);
        changed |= DragF(T("Particle radius"), &fx.CollisionRadius, 0.005f, 0.0f, 10.0f, "%.3f m", h);
    }

    // --- Текстура ---
    if (Section(T("Texture and frames"), nullptr, changed, h, true)) {
        if (h.TextureSlot && h.TextureSlot("fxtex", fx.Texture)) changed = true;
        if (!fx.Texture.empty() && fx.Frames.empty()) {
            changed |= DragI(T("Frames across"), &fx.TilesX, 0.05f, 1, 64, h);
            changed |= DragI(T("Frames down"), &fx.TilesY, 0.05f, 1, 64, h);
        }
        // Кадры отдельными файлами — движок склеит их в лист сам.
        ImGui::TextDisabled(T("Frames as separate files: %d"), (int)fx.Frames.size());
        int remove = -1;
        for (size_t i = 0; i < fx.Frames.size(); ++i) {
            ImGui::PushID((int)i + 1000);
            char id[32];
            std::snprintf(id, sizeof(id), "frame%zu", i);
            if (h.TextureSlot && h.TextureSlot(id, fx.Frames[i])) changed = true;
            if (ImGui::Button(T("Remove frame"))) remove = (int)i;
            ImGui::PopID();
        }
        if (remove >= 0) {
            Before(h);
            fx.Frames.erase(fx.Frames.begin() + remove);
            changed = true;
        }
        if (ImGui::Button(T("Add frame"))) {
            Before(h);
            fx.Frames.push_back(fx.Frames.empty() ? fx.Texture : fx.Frames.back());
            changed = true;
        }
        ImGui::SameLine();
        // «smoke_3.png» → smoke_0 … smoke_11: набор кадров одной кнопкой.
        const std::string seed = !fx.Frames.empty() ? fx.Frames.front() : fx.Texture;
        ImGui::BeginDisabled(seed.empty() || !h.FileExists);
        if (ImGui::Button(T("Collect numbered frames"))) {
            std::vector<std::string> seq = NumberedSequence(seed, h.FileExists);
            if (!seq.empty()) {
                Before(h);
                fx.Frames = seq;
                changed = true;
            }
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", T("Finds name_0, name_1, … next to the picked picture"));
        if (!fx.Frames.empty() && ImGui::Button(T("Clear frames"))) {
            Before(h);
            fx.Frames.clear();
            changed = true;
        }
        const char* flip[] = {T("Over lifetime"), T("By frame rate"), T("Random frame"), T("One frame")};
        changed |= Combo(T("Frame playback"), fx.Flipbook, flip, 4, h);
        if (fx.Flipbook == FlipbookMode::OverLifetime)
            changed |= DragF(T("Cycles"), &fx.Cycles, 0.05f, 0.0f, 100.0f, "%.2f", h);
        if (fx.Flipbook == FlipbookMode::Speed)
            changed |= DragF(T("Frame rate"), &fx.FrameRate, 0.2f, 0.0f, 240.0f, "%.1f fps", h);
        changed |= DragI(T("Start frame"), &fx.StartFrame, 0.1f, 0, 4096, h);
        changed |= Check(T("Random start frame"), fx.RandomStartFrame, h);
        changed |= Check(T("Pixel art (no smoothing)"), fx.PixelArt, h);
    }

    // --- Отрисовка ---
    if (Section(T("Rendering"), nullptr, changed, h, true)) {
        const char* modes[] = {T("Billboard"), T("Stretched along motion"), T("Horizontal"), T("Vertical"),
                               T("Mesh"), T("Trail")};
        changed |= Combo(T("Draw as"), fx.Render, modes, 6, h);
        const char* blends[] = {T("Alpha"), T("Additive (glows)"), T("Premultiplied")};
        changed |= Combo(T("Blending"), fx.Blend, blends, 3, h);
        if (fx.Texture.empty() && fx.Frames.empty()) {
            const char* sprites[] = {T("Soft circle"), T("Circle"), T("Square")};
            changed |= Combo(T("Shape without texture"), fx.Sprite, sprites, 3, h);
        }
        changed |= DragF(T("Intensity"), &fx.Intensity, 0.02f, 0.0f, 100.0f, "%.2f", h);
        changed |= Check(T("Sort far to near"), fx.SortByDistance, h);
        if (fx.Render == RenderMode::Stretched) {
            changed |= DragF(T("Length"), &fx.StretchLength, 0.02f, 0.0f, 100.0f, "%.2f", h);
            changed |= DragF(T("Length per speed"), &fx.StretchBySpeed, 0.005f, 0.0f, 10.0f, "%.3f", h);
        }
        if (fx.Render == RenderMode::Mesh) {
            const char* meshes[] = {T("Cube"), T("Sphere"), T("Tetrahedron"), T("Quad")};
            changed |= Combo(T("Mesh##shape"), fx.Mesh, meshes, 4, h);
            changed |= Drag3(T("Mesh scale"), fx.MeshScale, 0.01f, h);
        }
        if (fx.Render == RenderMode::Trail) {
            changed |= DragF(T("Trail lifetime"), &fx.TrailLifetime, 0.01f, 0.0f, 30.0f, "%.2f s", h);
            changed |= DragF(T("Point spacing"), &fx.TrailMinDistance, 0.005f, 0.0f, 10.0f, "%.3f m", h);
            changed |= DragI(T("Max points"), &fx.TrailMaxPoints, 0.2f, 2, 256, h);
            changed |= Check(T("Draw the particle too"), fx.TrailHead, h);
            changed |= DrawCurve(T("Width along the trail"), fx.TrailWidth, 0.0f, 1.0f, h);
            changed |= DrawGradient(T("Colour along the trail"), fx.TrailColor, h);
        }
    }

    // --- Дочерние эффекты ---
    if (Section(T("Sub-emitters"), nullptr, changed, h)) {
        ImGui::TextDisabled("%s", T("Another effect file born where a particle is born, dies or hits."));
        int remove = -1;
        for (size_t i = 0; i < fx.SubEmitters.size(); ++i) {
            SubEmitter& s = fx.SubEmitters[i];
            ImGui::PushID((int)i + 2000);
            ImGui::Separator();
            const char* when[] = {T("On birth"), T("On death"), T("On collision")};
            changed |= Combo(T("When"), s.When, when, 3, h);
            char id[32];
            std::snprintf(id, sizeof(id), "subfx%zu", i);
            if (h.EffectSlot && h.EffectSlot(id, s.Effect)) changed = true;
            changed |= DragI(T("Particles"), &s.Count, 0.2f, 0, 10000, h);
            changed |= Check(T("Inherit colour"), s.InheritColor, h);
            changed |= DragF(T("Inherit velocity"), &s.InheritVelocity, 0.01f, -2.0f, 2.0f, "%.2f", h);
            if (ImGui::Button(T("Remove sub-emitter"))) remove = (int)i;
            ImGui::PopID();
        }
        if (remove >= 0) {
            Before(h);
            fx.SubEmitters.erase(fx.SubEmitters.begin() + remove);
            changed = true;
        }
        if (ImGui::Button(T("Add sub-emitter"))) {
            Before(h);
            fx.SubEmitters.push_back(SubEmitter{});
            changed = true;
        }
    }

    ImGui::PopID();
    return changed;
}

} // namespace sage::editor
