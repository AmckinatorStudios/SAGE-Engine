// Редактор кривых и операции с ключами окна анимации.
//
// КРИВАЯ — ТО ЖЕ, ЧТО ИГРАЕТ. Рисуется anim::Sample — тем же счётом, которым
// движение и считается, — по каждому компоненту свойства (x, y, z, w своими
// цветами), во времени той же шкалы, что линейка над ней. Раньше показывался
// один отрезок в долях «0..1» и только Value.x: у позиции, где двигался Y, на
// графике была прямая.
//
// БЕЗЬЕ В PropertyClip — ФОРМА ОТРЕЗКА (как cubic-bezier в CSS): исходящая
// касательная левого ключа и входящая правого, в долях времени и значения
// отрезка, одна на все компоненты. Ручки рисуются на графике первого видимого
// компонента, у которого на этом отрезке есть перепад значения.
#include "AnimationPanelDetail.h"

#include "../animws/AnimStage.h"

namespace {

const ImU32 kCompColors[4] = {IM_COL32(235, 95, 90, 255), IM_COL32(120, 210, 90, 255),
                              IM_COL32(90, 150, 245, 255), IM_COL32(200, 200, 200, 255)};
const char* const kCompNames[4] = {"X", "Y", "Z", "W"};

int ComponentsOf(const anim::Track& t) {
    if (IsBoneChannel(t.Property)) return t.Property == "joint.rotation" ? 4 : 3;
    const anim::PropertyType* p = anim::FindProperty(t.Property);
    return p ? std::clamp(p->Components, 1, 4) : 4;
}

} // namespace

int AnimationPanel::CurveTrack() const {
    if (m_activeTrack >= 0 && m_activeTrack < (int)m_clip.Tracks.size()) return m_activeTrack;
    if (!m_selection.empty() && m_selection[0].Track < (int)m_clip.Tracks.size()) return m_selection[0].Track;
    return -1;
}

void AnimationPanel::ResortTrack(int track) {
    if (track < 0 || track >= (int)m_clip.Tracks.size()) return;
    const std::vector<int> moved = anim::SortKeys(m_clip.Tracks[(size_t)track]);
    for (KeyRef& r : m_selection)
        if (r.Track == track && r.Key >= 0 && r.Key < (int)moved.size()) r.Key = moved[(size_t)r.Key];
    m_dirty = true;
    Bump();
}

void AnimationPanel::CopyKeys() {
    if (m_selection.empty()) return;
    float first = 1e9f;
    for (const KeyRef& r : m_selection)
        if (r.Track < (int)m_clip.Tracks.size() && r.Key < (int)m_clip.Tracks[(size_t)r.Track].Keys.size())
            first = std::min(first, m_clip.Tracks[(size_t)r.Track].Keys[(size_t)r.Key].Time);
    m_copied.clear();
    for (const KeyRef& r : m_selection) {
        if (r.Track >= (int)m_clip.Tracks.size()) continue;
        const anim::Track& t = m_clip.Tracks[(size_t)r.Track];
        if (r.Key >= (int)t.Keys.size()) continue;
        CopiedKey c{t.Target, t.Property, t.Keys[(size_t)r.Key]};
        c.Key.Time -= first;   // время — от первого скопированного
        m_copied.push_back(c);
    }
    m_status = std::string(T("Keys copied: ")) + std::to_string(m_copied.size());
}

// Вставка — в момент бегунка, на те же дорожки (цель и свойство); дорожки
// нет — заводится. Вставленные ключи становятся выделением.
void AnimationPanel::PasteKeys() {
    if (m_copied.empty()) return;
    PushUndo();
    std::vector<std::pair<int, float>> pasted;
    for (const CopiedKey& c : m_copied) {
        int track = -1;
        for (int i = 0; i < (int)m_clip.Tracks.size(); ++i)
            if (m_clip.Tracks[(size_t)i].Target == c.Target && m_clip.Tracks[(size_t)i].Property == c.Property) track = i;
        if (track < 0) {
            anim::Track t;
            t.Target = c.Target;
            t.Property = c.Property;
            m_clip.Tracks.push_back(t);
            track = (int)m_clip.Tracks.size() - 1;
        }
        const float time = m_time + c.Key.Time;
        const int at = anim::SetKey(m_clip.Tracks[(size_t)track], time, c.Key.Value, c.Key.Out);
        anim::Key& k = m_clip.Tracks[(size_t)track].Keys[(size_t)at];
        k.InTangent = c.Key.InTangent;
        k.OutTangent = c.Key.OutTangent;
        pasted.push_back({track, time});
    }
    // Номера — после всех вставок: каждая сдвигает соседние ключи.
    m_selection.clear();
    for (const auto& [track, time] : pasted) {
        const auto& keys = m_clip.Tracks[(size_t)track].Keys;
        for (int k = 0; k < (int)keys.size(); ++k)
            if (std::fabs(keys[(size_t)k].Time - time) < 1e-5f) m_selection.push_back({track, k});
    }
    m_dirty = true;
    m_status = std::string(T("Keys pasted: ")) + std::to_string(pasted.size());
}

// Поля выделенных ключей — одной строкой над кривой.
void AnimationPanel::DrawKeyFields(EditorHost& host) {
    if (m_selection.empty()) {
        ImGui::TextDisabled("%s", T("Select keys to edit their interpolation, time and value"));
        return;
    }
    const bool editable = Editable();
    ImGui::BeginDisabled(!editable);

    // --- Интерполяция — у всего выделенного ---------------------------------
    // Показано, ЧТО стоит у выделенных: одинаковое — нажатой кнопкой.
    int common = -1;
    for (const KeyRef& r : m_selection) {
        if (r.Track >= (int)m_clip.Tracks.size() || r.Key >= (int)m_clip.Tracks[(size_t)r.Track].Keys.size()) continue;
        const int v = (int)m_clip.Tracks[(size_t)r.Track].Keys[(size_t)r.Key].Out;
        common = common == -1 ? v : (common == v ? v : -2);
    }
    ictl::GroupItem modes[3];
    modes[0].Text = T("Constant");
    modes[0].Tooltip = T("Holds the value until the next key (a step)");
    modes[1].Text = T("Linear");
    modes[1].Tooltip = T("Straight to the next key at a steady rate");
    modes[2].Text = T("Bezier");
    modes[2].Tooltip = m_skeletal ? T("Bone channels play Constant or Linear: a Bezier shape would not play")
                                  : T("Ease in and out: drag the handles on the curve");
    modes[2].Enabled = !m_skeletal;
    for (int i = 0; i < 3; ++i) modes[i].Active = common == i;
    if (const int hit = ictl::ButtonGroup("##key_interp", modes, 3, ictl::ButtonLook::Standard); hit >= 0) {
        PushUndo();
        for (const KeyRef& r : m_selection) {
            if (r.Track >= (int)m_clip.Tracks.size()) continue;
            anim::Track& t = m_clip.Tracks[(size_t)r.Track];
            // Канал кости играет одной интерполяцией на весь канал (ступенька или
            // прямая): правка одного ключа меняет канал целиком — иначе кривая
            // показывала бы не то, что сыграет.
            if (m_skeletal) {
                for (anim::Key& k : t.Keys) k.Out = (anim::Interp)hit;
            } else if (r.Key < (int)t.Keys.size()) {
                t.Keys[(size_t)r.Key].Out = (anim::Interp)hit;
            }
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("How the value goes from the selected keys to the next ones"));

    // --- Время и значение — у одного ключа ----------------------------------
    if (m_selection.size() == 1) {
        const KeyRef ref = m_selection[0];
        if (ref.Track < (int)m_clip.Tracks.size() && ref.Key < (int)m_clip.Tracks[(size_t)ref.Track].Keys.size()) {
            anim::Track& t = m_clip.Tracks[(size_t)ref.Track];
            anim::Key& key = t.Keys[(size_t)ref.Key];
            ImGui::SameLine(0.0f, 14.0f);
            ictl::NumOpts o;
            o.Step = 0.01f;
            o.Min = 0.0f;
            o.Max = 600.0f;
            o.Format = "%.3f s";
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.5f);
            {
                ictl::UndoScope undo([this] { PushUndo(); });
                if (ictl::Number(host, "##key_time", &key.Time, o, ictl::NumLook::Compact)) {
                    m_dirty = true;
                    Bump();
                }
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Time of the key"));
            // Порядок — по отпусканию поля: пересортировка посреди набора числа
            // перебрасывала бы правку на другой ключ.
            if (ImGui::IsItemDeactivatedAfterEdit()) ResortTrack(ref.Track);
            ImGui::SameLine();
            const int comps = ComponentsOf(t);
            ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 5.0f * comps));
            {
                // Снимок — в начале правки (как у полей инспектора), в историю
                // клипа, а не сцены.
                ictl::UndoScope undo([this] { PushUndo(); });
                if (ictl::DragVec(&host, "##key_value", &key.Value.x, comps, 0.01f)) {
                    m_dirty = true;
                    Bump();
                }
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Value of the key"));
        }
    } else {
        ImGui::SameLine(0.0f, 14.0f);
        ImGui::TextDisabled(T("%d keys selected"), (int)m_selection.size());
    }
    ImGui::EndDisabled();
    (void)host;
}

void AnimationPanel::DrawCurve(EditorHost& host, ImVec2 size) {
    (void)host;
    const float width = ImGui::GetContentRegionAvail().x;
    const float leftW = std::min(260.0f, width * 0.3f);
    const int ti = CurveTrack();

    // --- Слева: чья кривая и какие компоненты видны -------------------------
    ImGui::BeginChild("##curve_side", ImVec2(leftW, size.y), ImGuiChildFlags_Borders);
    if (ti < 0) {
        ImGui::TextDisabled("%s", T("Select a track or a key to see its curve."));
    } else {
        const anim::Track& t = m_clip.Tracks[(size_t)ti];
        const std::string title = IsBoneChannel(t.Property)
                                      ? t.Target + " · " + BoneChannelTitle(t.Property)
                                      : (t.Target.empty() ? std::string() : t.Target + " · ") + t.Property;
        ImGui::TextUnformatted(title.c_str());
        const int comps = ComponentsOf(t);
        for (int c = 0; c < comps; ++c) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kCompColors[c]));
            ictl::Check(kCompNames[c], &m_curveComp[c]);
            ImGui::PopStyleColor();
            if (c + 1 < comps) ImGui::SameLine();
        }
        if (t.Property == "joint.rotation")
            ImGui::TextDisabled("%s", T("Rotation: quaternion components"));
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // --- Справа: график во времени линейки ----------------------------------
    ImGui::BeginChild("##curve", ImVec2(0.0f, size.y), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (ti < 0 || avail.x < 40.0f || avail.y < 30.0f) {
        ImGui::EndChild();
        return;
    }
    anim::Track& track = m_clip.Tracks[(size_t)ti];
    const int comps = ComponentsOf(track);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 b(o.x + avail.x, o.y + avail.y);
    dl->AddRectFilled(o, b, IM_COL32(18, 20, 24, 255));

    // Время — как у линейки (тот же масштаб и сдвиг), значение — по ключам и
    // кривой с запасом в десятую часть.
    auto toX = [&](float t) { return o.x + (t - m_scroll) * m_zoom; };
    auto toTime = [&](float x) { return (x - o.x) / std::max(m_zoom, 1e-3f) + m_scroll; };
    float lo = 1e9f, hi = -1e9f;
    for (const anim::Key& k : track.Keys)
        for (int c = 0; c < comps; ++c)
            if (m_curveComp[c]) { lo = std::min(lo, k.Value[c]); hi = std::max(hi, k.Value[c]); }
    // Безье выходит за ключи (перелёт) — диапазон по самой кривой.
    if (!track.Keys.empty()) {
        const float t0 = track.Keys.front().Time, t1 = track.Keys.back().Time;
        for (int i = 0; i <= 64; ++i) {
            const glm::vec4 v = anim::Sample(track, t0 + (t1 - t0) * (float)i / 64.0f);
            for (int c = 0; c < comps; ++c)
                if (m_curveComp[c]) { lo = std::min(lo, v[c]); hi = std::max(hi, v[c]); }
        }
    }
    if (lo > hi) { lo = 0.0f; hi = 1.0f; }
    if (hi - lo < 1e-4f) { lo -= 0.5f; hi += 0.5f; }
    const float pad = (hi - lo) * 0.1f;
    lo -= pad;
    hi += pad;
    auto toY = [&](float v) { return b.y - 6.0f - (v - lo) / (hi - lo) * (avail.y - 12.0f); };
    auto toValue = [&](float y) { return lo + (b.y - 6.0f - y) / std::max(avail.y - 12.0f, 1.0f) * (hi - lo); };

    // Ноль и границы значений — подписями слева.
    for (float v : {lo + pad, hi - pad}) {
        const float y = toY(v);
        dl->AddLine(ImVec2(o.x, y), ImVec2(b.x, y), IM_COL32(255, 255, 255, 18));
        char lab[32];
        std::snprintf(lab, sizeof(lab), "%.3g", v);
        dl->AddText(ImVec2(o.x + 3.0f, y - ImGui::GetTextLineHeight()), IM_COL32(170, 175, 185, 170), lab);
    }
    if (lo < 0.0f && hi > 0.0f) dl->AddLine(ImVec2(o.x, toY(0.0f)), ImVec2(b.x, toY(0.0f)), IM_COL32(255, 255, 255, 40));
    // Конец клипа и бегунок — как на линейке.
    const float endX = toX(m_clip.Duration);
    if (endX < b.x) dl->AddRectFilled(ImVec2(std::max(endX, o.x), o.y), b, IM_COL32(0, 0, 0, 60));
    const float px = toX(m_time);
    if (px >= o.x && px <= b.x) dl->AddLine(ImVec2(px, o.y), ImVec2(px, b.y), IM_COL32(255, 120, 60, 200), 1.5f);

    // Кривые — anim::Sample, шагом в три точки экрана.
    for (int c = 0; c < comps; ++c) {
        if (!m_curveComp[c] || track.Keys.empty()) continue;
        ImVec2 prev(0, 0);
        bool havePrev = false;
        for (float x = o.x; x <= b.x; x += 3.0f) {
            const ImVec2 cur(x, toY(anim::Sample(track, toTime(x))[c]));
            if (havePrev) dl->AddLine(prev, cur, kCompColors[c], 1.6f);
            prev = cur;
            havePrev = true;
        }
    }

    // Ключи — точками на каждом видимом компоненте; щелчок выбирает ключ.
    const ImVec2 mouse = ImGui::GetMousePos();
    const bool hovered = ImGui::IsWindowHovered();
    int hotKey = -1;
    for (int k = 0; k < (int)track.Keys.size(); ++k) {
        const float x = toX(track.Keys[(size_t)k].Time);
        if (x < o.x - 6.0f || x > b.x + 6.0f) continue;
        const bool sel = std::find(m_selection.begin(), m_selection.end(), KeyRef{ti, k}) != m_selection.end();
        for (int c = 0; c < comps; ++c) {
            if (!m_curveComp[c]) continue;
            const ImVec2 p(x, toY(track.Keys[(size_t)k].Value[c]));
            dl->AddCircleFilled(p, sel ? 5.0f : 3.5f, sel ? IM_COL32(255, 200, 110, 255) : kCompColors[c]);
            if (hovered && std::fabs(mouse.x - p.x) < 6.0f && std::fabs(mouse.y - p.y) < 6.0f) hotKey = k;
        }
    }

    // --- Ручки безье выбранного ключа ---------------------------------------
    int focus = -1;
    for (const KeyRef& r : m_selection)
        if (r.Track == ti && r.Key < (int)track.Keys.size()) { focus = r.Key; break; }
    struct Handle {
        int Kind = 0;   // 1 — исходящая ключа focus, 2 — входящая ключа focus
        int Comp = -1;
        ImVec2 At{0, 0};
    };
    std::vector<Handle> handles;
    // Компонент, на котором рисуются ручки отрезка a->b: первый видимый с перепадом.
    auto compFor = [&](const anim::Key& a, const anim::Key& bk) {
        for (int c = 0; c < comps; ++c)
            if (m_curveComp[c] && std::fabs(bk.Value[c] - a.Value[c]) > 1e-6f) return c;
        return -1;
    };
    if (focus >= 0) {
        const anim::Key& k = track.Keys[(size_t)focus];
        if (k.Out == anim::Interp::Bezier && focus + 1 < (int)track.Keys.size()) {
            const anim::Key& n = track.Keys[(size_t)focus + 1];
            const int c = compFor(k, n);
            if (c >= 0) {
                const float span = n.Time - k.Time, d = n.Value[c] - k.Value[c];
                handles.push_back({1, c, ImVec2(toX(k.Time + k.OutTangent.x * span), toY(k.Value[c] + k.OutTangent.y * d))});
            }
        }
        if (focus > 0 && track.Keys[(size_t)focus - 1].Out == anim::Interp::Bezier) {
            const anim::Key& p = track.Keys[(size_t)focus - 1];
            const int c = compFor(p, k);
            if (c >= 0) {
                const float span = k.Time - p.Time, d = k.Value[c] - p.Value[c];
                handles.push_back({2, c, ImVec2(toX(k.Time + k.InTangent.x * span), toY(k.Value[c] + k.InTangent.y * d))});
            }
        }
        const ImVec2 kp(toX(k.Time), 0.0f);
        for (const Handle& h : handles) {
            const ImVec2 anchor(kp.x, toY(k.Value[h.Comp]));
            dl->AddLine(anchor, h.At, IM_COL32(255, 255, 255, 150));
            dl->AddCircleFilled(h.At, 5.0f, IM_COL32(255, 255, 255, 255));
            dl->AddCircle(h.At, 5.0f, kCompColors[h.Comp], 12, 1.5f);
        }
    }

    // Мышь: ручка ловится нажатием и держится до отпускания, где бы ни была
    // мышь; иначе щелчок по точке ключа выбирает ключ.
    ImGui::InvisibleButton("##curve_area", avail);
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        bool grabbed = false;
        if (Editable()) {
            for (const Handle& h : handles) {
                if (std::fabs(mouse.x - h.At.x) <= 7.0f && std::fabs(mouse.y - h.At.y) <= 7.0f) {
                    PushUndo();
                    m_curveDrag = h.Kind;
                    m_curveDragTrack = ti;
                    m_curveDragKey = focus;
                    m_curveDragComp = h.Comp;
                    grabbed = true;
                    break;
                }
            }
        }
        if (!grabbed && hotKey >= 0) {
            const KeyRef ref{ti, hotKey};
            if (ImGui::GetIO().KeyCtrl) {
                auto it = std::find(m_selection.begin(), m_selection.end(), ref);
                if (it != m_selection.end()) m_selection.erase(it);
                else m_selection.push_back(ref);
            } else {
                m_selection.assign(1, ref);
            }
        }
    }
    if (m_curveDrag != 0) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || m_curveDragTrack >= (int)m_clip.Tracks.size()) {
            m_curveDrag = 0;
        } else {
            anim::Track& t = m_clip.Tracks[(size_t)m_curveDragTrack];
            const int k = m_curveDragKey;
            const int c = m_curveDragComp;
            if (m_curveDrag == 1 && k + 1 < (int)t.Keys.size()) {
                anim::Key& a = t.Keys[(size_t)k];
                const anim::Key& n = t.Keys[(size_t)k + 1];
                const float span = std::max(n.Time - a.Time, 1e-6f), d = n.Value[c] - a.Value[c];
                a.OutTangent = glm::vec2((toTime(mouse.x) - a.Time) / span, (toValue(mouse.y) - a.Value[c]) / d);
                anim::ClampTangents(a);
            } else if (m_curveDrag == 2 && k > 0 && k < (int)t.Keys.size()) {
                anim::Key& kk = t.Keys[(size_t)k];
                const anim::Key& p = t.Keys[(size_t)k - 1];
                const float span = std::max(kk.Time - p.Time, 1e-6f), d = kk.Value[c] - p.Value[c];
                kk.InTangent = glm::vec2((toTime(mouse.x) - kk.Time) / span, (toValue(mouse.y) - kk.Value[c]) / d);
                anim::ClampTangents(kk);
            }
            m_dirty = true;
            Bump();
            SyncSceneToTime(host);
        }
    }
    // Колесо и средняя кнопка — как на линейке: шкала времени одна.
    if (hovered && ImGui::GetIO().MouseWheel != 0.0f) {
        const float at = toTime(mouse.x);
        m_zoom = std::clamp(m_zoom * (ImGui::GetIO().MouseWheel > 0 ? 1.15f : 0.87f), 8.0f, 4000.0f);
        m_scroll = std::max(0.0f, at - (mouse.x - o.x) / m_zoom);
    }
    if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
        m_scroll = std::max(0.0f, m_scroll - ImGui::GetIO().MouseDelta.x / std::max(m_zoom, 1e-3f));
    ImGui::EndChild();
}
