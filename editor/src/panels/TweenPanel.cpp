#include "TweenPanel.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "imgui.h"
#include "imgui_internal.h"   // PushMultiItemsWidths: поля осей ровно по ширине колонки

#include "../EditorHost.h"
#include "../EditorIcons.h"
#include "../EditorTheme.h"
#include "../Localization.h"
#include "../ObjectSlot.h"
#include "../PanelWindowId.h"
#include "../PanelWindows.h"
#include "../ui/ColorPicker.h"
#include "../ui/UI.h"
#include "sage/anim/AnimProperty.h"
#include "sage/scene/Scene.h"

namespace anim = sage::anim;

// =============================================================================
//  Слова для человека: свойства и кривые
// =============================================================================

#include "TweenPanelDetail.h"



// =============================================================================
//  Доступ
// =============================================================================

anim::TweenComponent* TweenPanel::Component(EditorHost& host) {
    if (m_ownerId == 0) return nullptr;
    GameObject o = host.CurrentScene().Get(m_ownerId);
    if (!o.Valid()) return nullptr;
    return o.Registry()->try_get<anim::TweenComponent>(o.Entity());
}

anim::TweenClip* TweenPanel::Clip(EditorHost& host) {
    anim::TweenComponent* tc = Component(host);
    if (!tc || tc->Tweens.empty()) return nullptr;
    m_index = std::clamp(m_index, 0, (int)tc->Tweens.size() - 1);
    return &tc->Tweens[(size_t)m_index];
}

void TweenPanel::Edited(EditorHost& host) {
    // Сначала вернуть объект, потом записывать отмену: иначе в снимок отмены
    // попали бы значения, выставленные просмотром, и уехали бы в сцену.
    StopPreview(host);
    host.PushUndoSnapshot();
}

void TweenPanel::Select(int tweenIndex) {
    m_index = tweenIndex;
    m_splitAxes.clear();
}

void TweenPanel::Open(int objectId, int tweenIndex, Action action) {
    m_ownerId = objectId;
    m_seenSelection = objectId;
    Select(tweenIndex);
    m_focusFrames = 3;
    m_wantProps = action == Action::AddProperty;
    m_wantPlay = action == Action::Play;
}

glm::vec4 TweenPanel::Current(entt::registry& reg, entt::entity target, const anim::PropertyType& p) const {
    if (m_previewing)
        for (const Saved& s : m_saved)
            if (s.Entity == target && s.Property == p.Id) return s.Value;
    glm::vec4 v(0.0f);
    anim::ReadProperty(p, reg, target, v);
    return v;
}

bool TweenPanel::AddProperty(EditorHost& host, const std::string& id) {
    if (host.InPlayMode()) return false;
    const anim::PropertyType* p = anim::FindProperty(id);
    Scene& scene = host.CurrentScene();
    GameObject owner = scene.Get(m_ownerId);
    if (!p || !owner.Valid()) return false;
    StopPreview(host);
    entt::registry& reg = scene.Registry();
    anim::TweenComponent& tc = reg.get_or_emplace<anim::TweenComponent>(owner.Entity());
    if (tc.Tweens.empty()) {
        anim::TweenClip clip;
        clip.Name = "Tween";
        tc.Tweens.push_back(clip);
        Select(0);
    }
    anim::TweenClip& clip = tc.Tweens[(size_t)std::clamp(m_index, 0, (int)tc.Tweens.size() - 1)];
    const entt::entity target = anim::TargetOf(scene, owner.Entity(), clip);
    if (target == entt::null || !anim::HasProperty(*p, reg, target)) return false;
    // Сразу рабочий твин: начало — «как есть в момент старта», конец —
    // заметно другой (SuggestedEnd), чтобы Play сразу показал, что твин делает.
    anim::TweenTrack t;
    t.Property = id;
    glm::vec4 now(0.0f);
    anim::ReadProperty(*p, reg, target, now);
    t.From = now;
    t.To = tweenui::SuggestedEnd(id, now);
    t.FromCurrent = true;
    t.Duration = 0.5f;
    t.Curve = anim::Ease::Make(anim::EaseShape::Quad, anim::EaseMode::Out);
    clip.AppendWith(t);   // вместе с остальными: сдвинуть — одним движением полосы
    host.PushUndoSnapshot();
    return true;
}

// =============================================================================
//  Просмотр — общим проигрывателем сцены
// =============================================================================

void TweenPanel::Play(EditorHost& host) {
    if (host.InPlayMode()) return;   // в игре твины идут сами
    Scene& scene = host.CurrentScene();
    if (m_previewing && m_previewScene == &scene && scene.Tweens.IsActive(m_handle)) {
        scene.Tweens.Resume(m_handle);
        m_paused = false;
        return;
    }
    StopPreview(host);   // доигравший — с начала
    anim::TweenClip* clip = Clip(host);
    GameObject owner = scene.Get(m_ownerId);
    if (!clip || !owner.Valid()) return;
    const entt::entity target = anim::TargetOf(scene, owner.Entity(), *clip);
    if (target == entt::null) return;
    // Запомнить всё, что твин тронет, — чтобы «Вернуть» поставил объект как был.
    m_saved.clear();
    entt::registry& reg = scene.Registry();
    for (const anim::TweenTrack& t : clip->Tracks) {
        const anim::PropertyType* p = anim::FindProperty(t.Property);
        if (!p) continue;
        Saved s;
        s.Entity = target;
        s.Property = t.Property;
        if (anim::ReadProperty(*p, reg, target, s.Value)) m_saved.push_back(s);
    }
    anim::TweenClip copy = *clip;
    copy.Delay = 0.0f;   // задержку в просмотре ждать незачем — она видна в настройках
    copy.Then.clear();   // следующий твин — уже другой просмотр
    m_handle = scene.Tweens.Play(reg, target, copy);
    m_previewScene = &scene;
    m_previewing = m_handle != anim::kNoTween;
    m_paused = false;
    m_lastTime = 0.0f;
}

void TweenPanel::Pause(EditorHost& host) {
    if (!m_previewing) return;
    host.CurrentScene().Tweens.Pause(m_handle);
    m_paused = true;
}

void TweenPanel::StopPreview(EditorHost& host) {
    if (!m_previewing) return;
    Scene& scene = host.CurrentScene();
    // Сцену за это время могли сменить — тогда возвращать некуда, и трогать
    // её проигрыватель нельзя: номер твина относится к прежней.
    if (m_previewScene == &scene) {
        scene.Tweens.Stop(m_handle);
        entt::registry& reg = scene.Registry();
        for (const Saved& s : m_saved)
            if (const anim::PropertyType* p = anim::FindProperty(s.Property))
                if (reg.valid(s.Entity)) anim::WriteProperty(*p, reg, s.Entity, s.Value);
    }
    m_saved.clear();
    m_previewing = false;
    m_paused = false;
    m_handle = anim::kNoTween;
    m_previewScene = nullptr;
    m_lastTime = 0.0f;
}

float TweenPanel::PreviewTime(EditorHost& host) const {
    if (!m_previewing || m_previewScene != &host.CurrentScene()) return 0.0f;
    const Scene& scene = host.CurrentScene();
    return scene.Tweens.IsActive(m_handle) ? scene.Tweens.TimeOf(m_handle) : m_lastTime;
}

// =============================================================================
//  Окно
// =============================================================================

void TweenPanel::Draw(EditorHost& host, bool* open, const std::string& windowId) {
    if (!open || !*open) {
        StopPreview(host);
        return;
    }
    // Просмотр идёт по времени кадра редактора: сцена правки системой кадра
    // не шагает, шагаем её проигрыватель сами — тот же, что в игре. ДО Begin:
    // окно, спрятанное за соседней вкладкой, не должно останавливать твин.
    {
        Scene& scene = host.CurrentScene();
        if (m_previewing) {
            if (m_previewScene != &scene || host.InPlayMode()) {
                m_previewing = false;
                m_saved.clear();
            } else if (!m_paused) {
                const bool was = scene.Tweens.IsActive(m_handle);
                if (was) m_lastTime = scene.Tweens.TimeOf(m_handle);
                scene.Tweens.Update(ImGui::GetIO().DeltaTime);
                if (was && !scene.Tweens.IsActive(m_handle)) {
                    const anim::TweenClip* c = Clip(host);
                    m_lastTime = c ? c->Length() : m_lastTime;
                }
            }
        }
    }
    // Фокус окну — несколько кадров (док решает, чья вкладка сверху, не
    // сразу), и каждый такой кадр закрывает всплывающие окна над ним. Меню,
    // о котором просили при открытии, ждёт, пока фокус устоится.
    const bool focusing = m_focusFrames > 0;
    if (focusing) {
        ImGui::SetNextWindowFocus();
        --m_focusFrames;
    }
    ImGui::SetNextWindowSize(ImVec2(960.0f, 340.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(sage::editor::panelid::Title("clock", T("Tween"), windowId).c_str(), open,
                      panelwindows::WindowFlags("Tween"))) {
        ImGui::End();
        return;
    }

    Scene& scene = host.CurrentScene();

    // За выбором в сцене: выбрал другой объект — окно про него.
    GameObject sel = host.SelectedObject();
    const int selId = sel.Valid() ? sel.Id() : 0;
    if (selId != m_seenSelection) {
        m_seenSelection = selId;
        if (selId != 0 && selId != m_ownerId) {
            StopPreview(host);
            m_ownerId = selId;
            Select(0);
        }
    }

    GameObject owner = scene.Get(m_ownerId);
    if (!owner.Valid()) {
        Sage::UI::EmptyState(T("No object selected"), T("Select an object in the scene to animate it with a tween."));
        ImGui::End();
        return;
    }
    anim::TweenComponent* tc = owner.Registry()->try_get<anim::TweenComponent>(owner.Entity());
    anim::TweenClip* clip = Clip(host);
    entt::entity target = clip ? anim::TargetOf(scene, owner.Entity(), *clip) : owner.Entity();

    if (m_wantPlay) {
        m_wantPlay = false;
        Play(host);
    }

    if (!tc || !clip) {
        DrawEmpty(host, false);
    } else {
        DrawHeader(host, *tc);
        clip = Clip(host);   // шапка могла удалить или добавить твин
        tc = owner.Registry()->try_get<anim::TweenComponent>(owner.Entity());
        if (clip && tc) {
            target = anim::TargetOf(scene, owner.Entity(), *clip);
            if (target == entt::null) {
                ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Warn), "%s",
                                   T("The object this tween animates is gone — pick another one under Advanced."));
            } else if (clip->Tracks.empty()) {
                DrawEmpty(host, true);
            } else {
                DrawRuler(host, *clip);
                for (size_t i = 0; i < clip->Tracks.size(); ++i) DrawCard(host, *clip, i, target);
                // Бегунок просмотра — через шкалу и все полосы.
                if (m_previewing) {
                    const float x = m_laneX + std::min(PreviewTime(host), clip->Length()) * m_pxPerSec;
                    ImGui::GetWindowDrawList()->AddLine(ImVec2(x, m_rulerTop), ImVec2(x, m_cardsBottom),
                                                        Col(EditorTheme::Role::Warn), 2.0f);
                }
                ImGui::BeginDisabled(host.InPlayMode());
                if (EditorIcons::Button("plus", T("Add property"))) m_wantProps = true;
                ImGui::EndDisabled();
            }
            DrawAdvanced(host, *tc, *clip);
        }
    }
    if (!focusing) DrawPopups(host, Clip(host), target);
    ImGui::End();
}

// --- Пусто: ни твина, ни свойств — одна кнопка ------------------------------------------
void TweenPanel::DrawEmpty(EditorHost& host, bool hasTween) {
    const float h = ImGui::GetFrameHeight();
    const char* label = hasTween ? T("Add property") : T("Tween");
    const char* hint = T("Smoothly change position, scale, colour, opacity… over time.");
    const float bw = EditorIcons::LabeledWidth(std::floor(h * 0.68f), label) + ImGui::GetStyle().FramePadding.x * 2.0f;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float blockH = h + ImGui::GetTextLineHeightWithSpacing() * 1.5f;
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::max(0.0f, (avail.y - blockH) * 0.4f));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (avail.x - bw) * 0.5f));
    ImGui::BeginDisabled(host.InPlayMode());
    if (EditorIcons::Button("plus", label)) m_wantProps = true;
    ImGui::EndDisabled();
    const float tw = ImGui::CalcTextSize(hint).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (avail.x - tw) * 0.5f));
    ImGui::TextDisabled("%s", hint);
}

// --- Шапка: какой твин, просмотр, время --------------------------------------------------
void TweenPanel::DrawHeader(EditorHost& host, anim::TweenComponent& tc) {
    GameObject owner = host.CurrentScene().Get(m_ownerId);
    const bool play = host.InPlayMode();
    const anim::TweenClip& clip = tc.Tweens[(size_t)m_index];
    const float lineRight = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;

    // Какой твин: список + «новый» и «удалить» в нём же — отдельные кнопки
    // рядом были бы шумом в шапке ради действия раз в день.
    ImGui::SetNextItemWidth(170.0f);
    if (ImGui::BeginCombo("##tween", clip.Name.c_str())) {
        for (int i = 0; i < (int)tc.Tweens.size(); ++i) {
            ImGui::PushID(i);
            if (ImGui::Selectable(tc.Tweens[(size_t)i].Name.c_str(), i == m_index) && i != m_index) {
                StopPreview(host);
                Select(i);
            }
            ImGui::PopID();
        }
        ImGui::Separator();
        ImGui::BeginDisabled(play);
        if (EditorIcons::MenuItem("plus", T("New tween"))) {
            StopPreview(host);
            anim::TweenClip c;
            c.Name = "Tween " + std::to_string(tc.Tweens.size() + 1);
            tc.Tweens.push_back(c);
            Select((int)tc.Tweens.size() - 1);
            host.PushUndoSnapshot();
        }
        if (EditorIcons::MenuItem("trash", T("Delete this tween"))) {
            StopPreview(host);
            tc.Tweens.erase(tc.Tweens.begin() + m_index);
            Select(std::max(0, m_index - 1));
            if (tc.Tweens.empty()) owner.Registry()->remove<anim::TweenComponent>(owner.Entity());
            host.PushUndoSnapshot();
        }
        ImGui::EndDisabled();
        ImGui::EndCombo();
        if (!owner.Registry()->all_of<anim::TweenComponent>(owner.Entity())) return;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Which tween of this object you are editing (an object can have several)"));
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    EditorIcons::Inline("cube");
    ImGui::SameLine(0.0f, EditorIcons::TextGap());
    ImGui::TextDisabled("%s", owner.Name().c_str());
    // Как это читать — одной подсказкой, а не угадыванием по полям.
    ImGui::SameLine();
    EditorIcons::IconOnlyButton("question", T("How a tween works:\n"
                                              "• each card is one property that changes from Start to End;\n"
                                              "• the bar on the right is when it happens: drag the middle to move it,\n"
                                              "  the dots to change when it starts and how long it lasts;\n"
                                              "• the curve says how the speed changes along the way;\n"
                                              "• Play shows it on the object, the round arrow next to it puts the object back."));

    // Справа: просмотр и время.
    const anim::TweenClip* c = Clip(host);
    char timeText[48];
    std::snprintf(timeText, sizeof(timeText), "%.2f / %.2f %s", PreviewTime(host), c ? c->Length() : 0.0f, T("s"));
    const float btn = ImGui::GetFrameHeight();
    const float sp = ImGui::GetStyle().ItemSpacing.x;
    const float right = btn * 2.0f + sp * 2.0f + ImGui::CalcTextSize(timeText).x;
    const float after = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + ImGui::GetScrollX() + sp;
    ImGui::SameLine(std::max(after, lineRight - right));
    const bool running = m_previewing && !m_paused && host.CurrentScene().Tweens.IsActive(m_handle);
    ImGui::BeginDisabled(play);
    if (EditorIcons::IconOnlyButton(running ? "pause" : "play", running ? T("Pause") : T("Play"), running))
        running ? Pause(host) : Play(host);
    ImGui::SameLine();
    ImGui::BeginDisabled(!m_previewing);
    if (EditorIcons::IconOnlyButton("refresh", T("Put the object back as it was"))) StopPreview(host);
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", timeText);
    if (play) ImGui::TextDisabled("%s", T("(in the game tweens run by themselves)"));
}

// --- Шкала времени над полосами ---------------------------------------------------------
void TweenPanel::DrawRuler(EditorHost& host, anim::TweenClip& clip) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float availW = ImGui::GetContentRegionAvail().x;
    // Левая колонка — название и значения; шкала — всё, что справа.
    m_infoW = std::clamp(availW * 0.5f, 330.0f, 520.0f);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    m_laneX = origin.x + m_infoW + st.ItemSpacing.x * 2.0f;
    m_laneW = std::max(origin.x + availW - m_laneX - 36.0f, 60.0f);   // справа — место подписи длительности
    const float length = std::max(clip.Length(), 0.01f);
    const float visible = std::max(length * 1.2f, 0.5f);   // с запасом: полосу есть куда тянуть
    m_pxPerSec = m_laneW / visible;

    const float h = ImGui::GetTextLineHeight() + 4.0f;
    m_rulerTop = origin.y;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 line = Col(EditorTheme::Role::Line);
    const ImU32 text = Col(EditorTheme::Role::TextFaint);
    // Шаг делений — «круглый» и не чаще ширины подписи.
    const float labelW = ImGui::CalcTextSize("0.00").x + 10.0f;
    float step = 60.0f;
    for (float s : {0.05f, 0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f, 30.0f, 60.0f})
        if (s * m_pxPerSec >= labelW) { step = s; break; }
    for (float t = 0.0f; t <= visible + 1e-4f; t += step) {
        const float x = m_laneX + t * m_pxPerSec;
        dl->AddLine(ImVec2(x, origin.y + h - 4.0f), ImVec2(x, origin.y + h), line);
        char buf[16];
        std::snprintf(buf, sizeof(buf), step < 1.0f ? "%.2f" : "%.0f", t);
        dl->AddText(ImVec2(x + 2.0f, origin.y), text, buf);
    }

    // Подписи колонок: слева — что меняется, справа — когда.
    dl->AddText(ImVec2(origin.x + 6.0f, origin.y), text, T("Property: Start → End"));
    {
        const char* timeLabel = T("time, s");
        const float w = ImGui::CalcTextSize(timeLabel).x;
        dl->AddText(ImVec2(m_laneX - w - st.ItemSpacing.x, origin.y), text, timeLabel);
    }

    // Бегунок: щелчок или протяжка по шкале — посмотреть момент.
    ImGui::SetCursorScreenPos(ImVec2(m_laneX, origin.y));
    ImGui::InvisibleButton("##scrub", ImVec2(std::max(length * m_pxPerSec - 6.0f, 1.0f), h));
    if (ImGui::IsItemActive() && !host.InPlayMode()) {
        const float t = std::clamp((ImGui::GetIO().MousePos.x - m_laneX) / m_pxPerSec, 0.0f, length);
        if (!m_previewing) Play(host);
        if (m_previewing) {
            Pause(host);
            host.CurrentScene().Tweens.Seek(m_handle, t);
            m_lastTime = t;
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Click or drag to see the tween at that moment"));

    // Конец твина — ручка: тянешь — растягиваешь весь твин.
    const float endX = m_laneX + length * m_pxPerSec;
    ImGui::SetCursorScreenPos(ImVec2(endX - 6.0f, origin.y));
    ImGui::BeginDisabled(host.InPlayMode());
    ImGui::InvisibleButton("##end", ImVec2(12.0f, h));
    ImGui::EndDisabled();
    const bool endHot = ImGui::IsItemHovered() || ImGui::IsItemActive();
    if (endHot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (ImGui::IsItemActivated()) StopPreview(host);
    if (ImGui::IsItemActive()) clip.Stretch(std::max((ImGui::GetIO().MousePos.x - m_laneX) / m_pxPerSec, 0.05f));
    if (ImGui::IsItemDeactivated()) host.PushUndoSnapshot();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Drag to stretch the whole tween"));
    dl->AddTriangleFilled(ImVec2(endX - 5, origin.y + h - 8), ImVec2(endX + 5, origin.y + h - 8), ImVec2(endX, origin.y + h),
                          Col(endHot ? EditorTheme::Role::AccentHover : EditorTheme::Role::Accent));

    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + h + st.ItemSpacing.y));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
}

// --- Значение свойства по числу компонент ------------------------------------------------
//
// У каждой оси — своя буква в поле («X 1.00»): три одинаковых числа подряд не
// говорят, какое из них высота.
bool TweenPanel::DrawValue(const char* id, const anim::PropertyType* p, glm::vec4& v, float width, bool uniform) {
    ImGui::PushID(id);
    bool changed = false;
    const int comps = p ? p->Components : 1;
    if (IsColor(p)) {
        ImGui::SetNextItemWidth(width);
        changed = comps >= 4 ? Sage::UI::ColorField4("##c", &v.x) : Sage::UI::ColorField3("##c", &v.x);
    } else if (uniform || comps == 1) {
        // Масштаб «одним числом»: в 9 из 10 твинов оси меняются вместе.
        ImGui::SetNextItemWidth(width);
        float s = v.x;
        const float speed = comps == 1 && s > 10.0f ? 0.1f : 0.01f;
        if ((changed = ImGui::DragFloat("##v", &s, speed, 0.0f, 0.0f, uniform ? "XYZ %.2f" : "%.2f")))
            v = uniform ? glm::vec4(s, s, s, v.w) : glm::vec4(s, v.y, v.z, v.w);
    } else {
        static const char* const kAxis[] = {"X %.2f", "Y %.2f", "Z %.2f", "W %.2f"};
        const int n = std::min(comps, 4);
        ImGui::PushMultiItemsWidths(n, width);
        for (int k = 0; k < n; ++k) {
            ImGui::PushID(k);
            if (k) ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
            changed |= ImGui::DragFloat("##a", &v[k], 0.05f, 0.0f, 0.0f, kAxis[k]);
            ImGui::PopItemWidth();
            ImGui::PopID();
        }
    }
    ImGui::PopID();
    return changed;
}


