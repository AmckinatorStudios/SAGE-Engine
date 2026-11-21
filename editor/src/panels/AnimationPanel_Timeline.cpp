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
    dl->AddRectFilled(origin, ImVec2(origin.x + avail.x, origin.y + RulerHeight()),
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
        dl->AddLine(ImVec2(x, origin.y + RulerHeight()), ImVec2(x, origin.y + avail.y),
                    IM_COL32(120, 230, 180, 120), 1.0f);
        const ImVec2 tri[3] = {{x - 5.0f, origin.y + RulerHeight() - 7.0f},
                               {x + 5.0f, origin.y + RulerHeight() - 7.0f},
                               {x, origin.y + RulerHeight()}};
        dl->AddConvexPolyFilled(tri, 3, IM_COL32(120, 230, 180, 230));
        if (!m.Name.empty())
            dl->AddText(ImVec2(x + 6.0f, origin.y + RulerHeight() - 8.0f),
                        IM_COL32(150, 240, 200, 220), m.Name.c_str());
    }

    // --- Ключи --------------------------------------------------------------
    const float rowsTop = origin.y + RulerHeight();
    int hotTrack = -1, hotKey = -1;
    for (int row = 0; row < (int)m_rows.size(); ++row) {
        const int i = m_rows[(size_t)row];
        const anim::Track& t = m_clip.Tracks[i];
        const float y = rowsTop + kTrackHeight * (float)row + kTrackHeight * 0.5f;
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
    const bool overRuler = hovered && mouse.y < origin.y + RulerHeight();

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
            // Пустое место среди дорожек — рамка выделения (с Ctrl — в
            // добавок). Время переводят по шкале сверху: раньше щелчок мимо
            // ключа сбрасывал выделение и прыгал бегунком, и выделить
            // несколько ключей можно было только по одному.
            if (!ImGui::GetIO().KeyCtrl) m_selection.clear();
            m_boxSelect = true;
            m_boxFrom = mouse;
        }
    }

    if (m_boxSelect) {
        const ImVec2 lo(std::min(m_boxFrom.x, mouse.x), std::min(m_boxFrom.y, mouse.y));
        const ImVec2 hi(std::max(m_boxFrom.x, mouse.x), std::max(m_boxFrom.y, mouse.y));
        dl->AddRectFilled(lo, hi, IM_COL32(120, 170, 255, 30));
        dl->AddRect(lo, hi, IM_COL32(120, 170, 255, 160));
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            m_boxSelect = false;
            for (int row = 0; row < (int)m_rows.size(); ++row) {
                const int ti = m_rows[(size_t)row];
                const float y = rowsTop + kTrackHeight * (float)row + kTrackHeight * 0.5f;
                if (y < lo.y || y > hi.y) continue;
                const anim::Track& t = m_clip.Tracks[(size_t)ti];
                for (int k = 0; k < (int)t.Keys.size(); ++k) {
                    const float x = toX(t.Keys[(size_t)k].Time);
                    if (x < lo.x || x > hi.x) continue;
                    const KeyRef ref{ti, k};
                    if (std::find(m_selection.begin(), m_selection.end(), ref) == m_selection.end())
                        m_selection.push_back(ref);
                    m_activeTrack = ti;
                }
            }
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
            // Ключи снова по времени: проигрывание считает их упорядоченными,
            // а ключ, перетащенный за соседа, иначе ломал бы отрезки. Выделение
            // переезжает вместе с ключами.
            std::vector<int> tracks;
            for (const KeyRef& r : m_selection)
                if (std::find(tracks.begin(), tracks.end(), r.Track) == tracks.end()) tracks.push_back(r.Track);
            for (int t : tracks) ResortTrack(t);
            SyncSceneToTime(host);
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
