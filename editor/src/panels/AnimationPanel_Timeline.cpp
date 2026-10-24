#include "AnimationPanel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

#include "../EditorHost.h"
#include "../EditorIcons.h"
#include "../HotkeyScope.h"
#include "../EditorTheme.h"
#include "../Localization.h"
#include "../PanelWindowId.h"
#include "../PanelWindows.h"
#include "../Project.h"
#include "../ui/UI.h"
#include "sage/anim/AnimProperty.h"
#include "sage/anim/ClipFile.h"
#include "sage/anim/PropertyAnimator.h"
#include "sage/core/Log.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "AnimationPanelDetail.h"
namespace anim = sage::anim;

namespace fs = std::filesystem;

// Шкала времени и редактор кривых окна «Анимация».
void AnimationPanel::DrawTimeline(EditorHost& host, ImVec2 size) {
    ImGui::BeginChild("##timeline", size, ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 32.0f || avail.y < 16.0f) { ImGui::EndChild(); return; }

    // ОДНО преобразование времени в пиксели и обратно, и оно здесь: две
    // формулы в разных местах разойдутся на пиксель, и ключ перестанет
    // попадать под курсор.
    auto toX = [&](float t) { return origin.x + (t - m_scroll) * m_zoom; };
    auto toTime = [&](float x) { return (x - origin.x) / std::max(m_zoom, 1e-3f) + m_scroll; };

    const ImVec2 mouse = ImGui::GetMousePos();
    const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);

    // --- Линейка ------------------------------------------------------------
    //
    // Подписи через «круглый» шаг, а не через фиксированное число делений:
    // шкала тянется и масштабируется, и десять делений на ней означали бы
    // подписи вида «0.37 с».
    // Шаг — из ряда 1, 2, 5 и их десятичных кратных: только такие числа
    // читаются на шкале без пересчёта в уме. Ряд «удваивать» дал бы 0.625 и
    // 1.25, по которым «где здесь полторы секунды» уже не видно.
    float step = 0.001f;
    const float kMults[3] = {1.0f, 2.0f, 5.0f};
    int mult = 0;
    while (step * kMults[mult] * m_zoom < 56.0f) {
        if (++mult == 3) { mult = 0; step *= 10.0f; }
        if (step > 1e4f) break;
    }
    step *= kMults[mult];
    dl->AddRectFilled(origin, ImVec2(origin.x + avail.x, origin.y + kRulerHeight),
                      ImGui::GetColorU32(ImGuiCol_FrameBg));
    for (float t = std::floor(m_scroll / step) * step; toX(t) < origin.x + avail.x; t += step) {
        const float x = toX(t);
        if (x < origin.x) continue;
        dl->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + avail.y),
                    IM_COL32(255, 255, 255, 18));
        char lab[32];
        std::snprintf(lab, sizeof(lab), "%.2f", t);
        dl->AddText(ImVec2(x + 3.0f, origin.y + 3.0f), IM_COL32(200, 205, 215, 200), lab);
    }

    // Конец клипа: за ним ключи ставить можно, но играться они не будут, и
    // видеть эту границу надо — иначе «поставил ключ, а он не срабатывает».
    const float endX = toX(m_clip.Duration);
    if (endX > origin.x && endX < origin.x + avail.x) {
        dl->AddRectFilled(ImVec2(endX, origin.y), ImVec2(origin.x + avail.x, origin.y + avail.y),
                          IM_COL32(0, 0, 0, 60));
        dl->AddLine(ImVec2(endX, origin.y), ImVec2(endX, origin.y + avail.y),
                    IM_COL32(255, 120, 120, 160), 1.0f);
    }

    // --- Метки --------------------------------------------------------------
    for (const anim::Marker& m : m_clip.Markers) {
        const float x = toX(m.Time);
        if (x < origin.x || x > origin.x + avail.x) continue;
        dl->AddLine(ImVec2(x, origin.y + kRulerHeight), ImVec2(x, origin.y + avail.y),
                    IM_COL32(120, 230, 180, 120), 1.0f);
        const ImVec2 tri[3] = {{x - 5.0f, origin.y + kRulerHeight - 7.0f},
                               {x + 5.0f, origin.y + kRulerHeight - 7.0f},
                               {x, origin.y + kRulerHeight}};
        dl->AddConvexPolyFilled(tri, 3, IM_COL32(120, 230, 180, 230));
        if (!m.Name.empty())
            dl->AddText(ImVec2(x + 6.0f, origin.y + kRulerHeight - 8.0f),
                        IM_COL32(150, 240, 200, 220), m.Name.c_str());
    }

    // --- Ключи --------------------------------------------------------------
    const float rowsTop = origin.y + kRulerHeight;
    int hotTrack = -1, hotKey = -1;
    for (int i = 0; i < (int)m_clip.Tracks.size(); ++i) {
        const anim::Track& t = m_clip.Tracks[i];
        const float y = rowsTop + kTrackHeight * (float)i + kTrackHeight * 0.5f;
        if (i == m_activeTrack) {
            dl->AddRectFilled(ImVec2(origin.x, y - kTrackHeight * 0.5f),
                              ImVec2(origin.x + avail.x, y + kTrackHeight * 0.5f),
                              IM_COL32(255, 255, 255, 12));
        }
        for (int k = 0; k < (int)t.Keys.size(); ++k) {
            const float x = toX(t.Keys[k].Time);
            if (x < origin.x - 8.0f || x > origin.x + avail.x + 8.0f) continue;
            bool selected = false;
            for (const KeyRef& r : m_selection)
                if (r.Track == i && r.Key == k) { selected = true; break; }
            const bool hot = hovered && std::fabs(mouse.x - x) <= kKeyRadius + 2.0f &&
                             std::fabs(mouse.y - y) <= kTrackHeight * 0.5f;
            if (hot) { hotTrack = i; hotKey = k; }

            // РОМБ, а не кружок: ключ — это момент, и остроугольная форма
            // показывает его положение точнее круглой, у которой «центр»
            // приходится угадывать. Форма разная по интерполяции: ступенька
            // квадратная — видно, что между ключами ничего не происходит.
            const ImU32 col = selected ? IM_COL32(255, 200, 110, 255)
                              : hot    ? IM_COL32(255, 230, 180, 255)
                                       : IM_COL32(150, 190, 240, 230);
            if (t.Keys[k].Out == anim::Interp::Constant) {
                dl->AddRectFilled(ImVec2(x - kKeyRadius + 1.0f, y - kKeyRadius + 1.0f),
                                  ImVec2(x + kKeyRadius - 1.0f, y + kKeyRadius - 1.0f), col, 1.0f);
            } else {
                const ImVec2 d[4] = {{x, y - kKeyRadius}, {x + kKeyRadius, y},
                                     {x, y + kKeyRadius}, {x - kKeyRadius, y}};
                dl->AddConvexPolyFilled(d, 4, col);
                if (t.Keys[k].Out == anim::Interp::Bezier)
                    dl->AddCircle(ImVec2(x, y), kKeyRadius + 2.0f, col, 10, 1.0f);
            }
        }
    }

    // --- Бегунок ------------------------------------------------------------
    const float px = toX(m_time);
    if (px >= origin.x && px <= origin.x + avail.x) {
        dl->AddLine(ImVec2(px, origin.y), ImVec2(px, origin.y + avail.y),
                    IM_COL32(255, 120, 60, 230), 1.5f);
        const ImVec2 head[3] = {{px - 6.0f, origin.y}, {px + 6.0f, origin.y},
                                {px, origin.y + 9.0f}};
        dl->AddConvexPolyFilled(head, 3, IM_COL32(255, 120, 60, 255));
    }

    // --- Мышь ---------------------------------------------------------------
    ImGui::InvisibleButton("##tl_area", avail, ImGuiButtonFlags_MouseButtonLeft |
                                                   ImGuiButtonFlags_MouseButtonRight);
    const bool overRuler = hovered && mouse.y < origin.y + kRulerHeight;

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (overRuler) {
            m_dragPlayhead = true;
        } else if (hotKey >= 0) {
            const KeyRef ref{hotTrack, hotKey};
            bool already = false;
            for (const KeyRef& r : m_selection) if (r == ref) { already = true; break; }
            if (ImGui::GetIO().KeyCtrl) {
                if (already) m_selection.erase(std::remove(m_selection.begin(), m_selection.end(), ref),
                                               m_selection.end());
                else m_selection.push_back(ref);
            } else if (!already) {
                m_selection.assign(1, ref);
            }
            m_activeTrack = hotTrack;
            // Тащить набор можно только в своём клипе: у импортированного
            // ключи показываются и выделяются, но не двигаются.
            if (Editable()) {
                m_dragKeys = true;
                m_dragStartTime = toTime(mouse.x);
                m_dragStartTimes.clear();
                for (const KeyRef& r : m_selection)
                    m_dragStartTimes.push_back(m_clip.Tracks[r.Track].Keys[r.Key].Time);
                PushUndo();
            }
        } else {
            m_selection.clear();
            m_dragPlayhead = true;   // щелчок по пустому месту — перевод времени
        }
    }

    if (m_dragPlayhead) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) m_dragPlayhead = false;
        else {
            m_time = std::max(0.0f, toTime(mouse.x));
            // Притяжка к сетке времени: попасть мышью в «ровно полсекунды»
            // невозможно, а нужна она постоянно. Alt отключает — иногда надо
            // именно между.
            if (!ImGui::GetIO().KeyAlt) m_time = std::round(m_time / 0.05f) * 0.05f;
            m_playing = false;
            SyncSceneToTime(host);
        }
    }

    if (m_dragKeys) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            m_dragKeys = false;
        } else {
            float d = toTime(mouse.x) - m_dragStartTime;
            if (!ImGui::GetIO().KeyAlt) d = std::round(d / 0.05f) * 0.05f;
            for (size_t i = 0; i < m_selection.size() && i < m_dragStartTimes.size(); ++i) {
                const KeyRef& r = m_selection[i];
                if (r.Track < 0 || r.Track >= (int)m_clip.Tracks.size()) continue;
                anim::Track& t = m_clip.Tracks[r.Track];
                if (r.Key < 0 || r.Key >= (int)t.Keys.size()) continue;
                t.Keys[r.Key].Time = std::max(0.0f, m_dragStartTimes[i] + d);
            }
            m_dirty = true;
            SyncSceneToTime(host);
        }
    }

    // Колесо — масштаб линейки ОТ КУРСОРА: точка под мышью остаётся на месте,
    // иначе после каждого шага приходится догонять прокруткой то, на что
    // смотрел.
    if (hovered && ImGui::GetIO().MouseWheel != 0.0f) {
        const float at = toTime(mouse.x);
        m_zoom = std::clamp(m_zoom * (ImGui::GetIO().MouseWheel > 0 ? 1.15f : 0.87f), 8.0f, 4000.0f);
        m_scroll = at - (mouse.x - origin.x) / m_zoom;
    }
    if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
        m_scroll -= ImGui::GetIO().MouseDelta.x / std::max(m_zoom, 1e-3f);
    }
    m_scroll = std::max(0.0f, m_scroll);

    ImGui::EndChild();
}


void AnimationPanel::DrawCurve(EditorHost& host, ImVec2 size) {
    (void)host;
    ImGui::BeginChild("##curve", size, ImGuiChildFlags_Borders);
    if (m_selection.empty() || m_selection[0].Track >= (int)m_clip.Tracks.size()) {
        ImGui::TextDisabled("%s", T("Select a key to see its curve."));
        ImGui::EndChild();
        return;
    }
    const KeyRef& ref = m_selection[0];
    anim::Track& track = m_clip.Tracks[ref.Track];
    if (ref.Key < 0 || ref.Key >= (int)track.Keys.size()) {
        ImGui::EndChild();
        return;
    }
    anim::Key& key = track.Keys[ref.Key];

    // --- Чем идёт значение от этого ключа -----------------------------------
    ImGui::BeginDisabled(!Editable());
    int interp = (int)key.Out;
    const char* names[] = {T("Constant"), T("Linear"), T("Bezier")};
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::Combo(T("Interpolation"), &interp, names, 3)) {
        PushUndo();
        // Интерполяция ставится ВСЕМУ выделенному набору: выделили пять
        // ключей и сказали «ступенькой» — значит всем пяти, а не первому.
        for (const KeyRef& r : m_selection) {
            if (r.Track < (int)m_clip.Tracks.size() &&
                r.Key < (int)m_clip.Tracks[r.Track].Keys.size())
                m_clip.Tracks[r.Track].Keys[r.Key].Out = (anim::Interp)interp;
        }
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    if (ImGui::DragFloat(T("Time"), &key.Time, 0.01f, 0.0f, 600.0f, "%.2f s")) {
        m_dirty = true;
        std::sort(track.Keys.begin(), track.Keys.end(),
                  [](const anim::Key& a, const anim::Key& b) { return a.Time < b.Time; });
    }
    ImGui::SameLine();
    const anim::PropertyType* p = anim::FindProperty(track.Property);
    const int comps = p ? p->Components : 4;
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::DragScalarN(T("Value"), ImGuiDataType_Float, &key.Value.x, comps, 0.05f))
        m_dirty = true;
    ImGui::EndDisabled();

    // --- Сама кривая --------------------------------------------------------
    //
    // ЗАЧЕМ ОНА ЗДЕСЬ. Разгон и торможение — это ХАРАКТЕР движения, и числами
    // касательных он не читается: «0.42, 0» не говорит ничего никому. Кривая
    // говорит всё с одного взгляда, и тянут её за те же две точки.
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 40.0f || avail.y < 30.0f) { ImGui::EndChild(); return; }
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const ImVec2 b(a.x + avail.x, a.y + avail.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, b, IM_COL32(18, 20, 24, 255));
    dl->AddRect(a, b, IM_COL32(255, 255, 255, 30));

    const float pad = 12.0f;
    auto toPix = [&](float u, float v) {
        return ImVec2(a.x + pad + u * (avail.x - pad * 2.0f),
                      b.y - pad - v * (avail.y - pad * 2.0f));
    };
    // Сетка «ноль — единица» по обеим осям: без неё не видно, где прямая.
    dl->AddLine(toPix(0, 0), toPix(1, 0), IM_COL32(255, 255, 255, 25));
    dl->AddLine(toPix(0, 1), toPix(1, 1), IM_COL32(255, 255, 255, 25));
    dl->AddLine(toPix(0, 0), toPix(0, 1), IM_COL32(255, 255, 255, 25));
    dl->AddLine(toPix(1, 0), toPix(1, 1), IM_COL32(255, 255, 255, 25));
    dl->AddLine(toPix(0, 0), toPix(1, 1), IM_COL32(255, 255, 255, 40));

    // Кривая рисуется ТЕМ ЖЕ СЧЁТОМ, которым движение и считается: отдельная
    // формула для рисунка однажды разойдётся с настоящей, и человек будет
    // настраивать одно, а получать другое.
    if (ref.Key + 1 < (int)track.Keys.size()) {
        const anim::Key& next = track.Keys[ref.Key + 1];
        anim::Track probe;
        probe.Keys = {key, next};
        const float t0 = key.Time, t1 = next.Time;
        const float dv = next.Value.x - key.Value.x;
        ImVec2 prev = toPix(0.0f, 0.0f);
        for (int i = 1; i <= 64; ++i) {
            const float u = (float)i / 64.0f;
            const float v = std::fabs(dv) > 1e-6f
                                ? (anim::Sample(probe, t0 + (t1 - t0) * u).x - key.Value.x) / dv
                                : u;
            const ImVec2 cur = toPix(u, v);
            dl->AddLine(prev, cur, IM_COL32(255, 190, 90, 230), 1.6f);
            prev = cur;
        }
        // Ручки касательных — те же управляющие точки, что в счёте.
        if (key.Out == anim::Interp::Bezier) {
            const ImVec2 h1 = toPix(key.OutTangent.x, key.OutTangent.y);
            const ImVec2 h2 = toPix(1.0f + next.InTangent.x, 1.0f + next.InTangent.y);
            dl->AddLine(toPix(0, 0), h1, IM_COL32(120, 200, 255, 160));
            dl->AddLine(toPix(1, 1), h2, IM_COL32(120, 200, 255, 160));
            dl->AddCircleFilled(h1, 4.0f, IM_COL32(120, 200, 255, 255));
            dl->AddCircleFilled(h2, 4.0f, IM_COL32(120, 200, 255, 255));

            // Тянутся мышью: числами касательную не подбирают.
            if (Editable()) {
                const ImVec2 m = ImGui::GetMousePos();
                auto grab = [&](const ImVec2& h, glm::vec2& tangent, bool fromEnd) {
                    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) return;
                    if (std::fabs(m.x - h.x) > 7.0f || std::fabs(m.y - h.y) > 7.0f) return;
                    const float u = (m.x - a.x - pad) / std::max(avail.x - pad * 2.0f, 1.0f);
                    const float v = (b.y - pad - m.y) / std::max(avail.y - pad * 2.0f, 1.0f);
                    tangent = fromEnd ? glm::vec2(u - 1.0f, v - 1.0f) : glm::vec2(u, v);
                    m_dirty = true;
                };
                grab(h1, key.OutTangent, false);
                grab(h2, track.Keys[ref.Key + 1].InTangent, true);
            }
        }
    } else {
        dl->AddText(ImVec2(a.x + pad, a.y + pad), IM_COL32(150, 150, 160, 200),
                    T("The last key: there is nothing after it to draw."));
    }
    ImGui::Dummy(avail);
    ImGui::EndChild();
}
