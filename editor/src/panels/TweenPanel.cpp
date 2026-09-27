#include "TweenPanel.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>

#include "imgui.h"

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

namespace {

// Подпись кривой в строке: «Quad Out», «Linear», «Custom».
// Имена форм (Quad, Back, Elastic…) не переводятся: это общие для всех
// движков термины, по ним ищут примеры. Направление — словами.
const char* const kShapeNames[] = {"Linear", "Quad", "Cubic", "Quart", "Quint", "Sine",
                                   "Expo",   "Circ", "Back",  "Elastic", "Bounce", "Custom curve"};
const char* ModeLabel(anim::EaseMode m) {
    switch (m) {
        case anim::EaseMode::In: return T("Ease in");
        case anim::EaseMode::Out: return T("Ease out");
        default: return T("Ease in-out");
    }
}
const char* ShapeLabel(anim::EaseShape s) {
    if (s == anim::EaseShape::Linear) return T("Linear");
    if (s == anim::EaseShape::Curve) return T("Custom curve");
    return kShapeNames[(int)s];
}

// В строке дорожки — коротко и в общепринятой записи («Back Out»): колонка
// узкая, а направление словами целиком есть в редакторе кривой рядом.
std::string EaseLabel(const anim::Ease& e) {
    if (e.Shape == anim::EaseShape::Linear || e.Shape == anim::EaseShape::Curve) return ShapeLabel(e.Shape);
    static const char* kShort[] = {"In", "Out", "InOut"};
    return std::string(kShapeNames[(int)e.Shape]) + " " + kShort[(int)e.Mode];
}

bool IsColor(const anim::PropertyType* p) {
    if (!p || p->Components < 3) return false;
    const std::string& id = p->Id;
    return id.find("color") != std::string::npos || id.find("colour") != std::string::npos ||
           id.find("tint") != std::string::npos;
}

// Строка — название свойства; чьё оно (Материал, Свет, Заливка) — в подсказке:
// в узкой колонке «Преобразование · Позиция» обрезалось бы на первом слове.
std::string PropertyLabel(const anim::PropertyType* p, const std::string& id) {
    if (!p) return id.empty() ? std::string(T("Delay")) : id;
    return T(p->Title.c_str());
}

// Маленький график кривой — в строке дорожки и в редакторе кривой.
void PlotEase(ImDrawList* dl, ImVec2 a, ImVec2 b, const anim::Ease& e, ImU32 color, float thickness) {
    ImVec2 pts[33];
    const float w = b.x - a.x, h = b.y - a.y;
    for (int i = 0; i <= 32; ++i) {
        const float t = i / 32.0f;
        const float v = anim::Evaluate(e, t);
        pts[i] = ImVec2(a.x + t * w, b.y - v * h);
    }
    dl->AddPolyline(pts, 33, color, ImDrawFlags_None, thickness);
}

} // namespace

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
    // Запомнить всё, что твин тронет, — чтобы Reset вернул объект как был.
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
    copy.Delay = 0.0f;   // задержку в просмотре ждать незачем — она видна на шкале
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
    if (m_focusFrames > 0) {
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

    // За выбором в сцене — пока окно не открыли на конкретный твин.
    GameObject sel = host.SelectedObject();
    if (m_follow && sel.Valid() && sel.Id() != m_ownerId) {
        StopPreview(host);
        m_ownerId = sel.Id();
        m_index = 0;
        m_selectedTrack = 0;
    }

    GameObject owner = scene.Get(m_ownerId);
    if (!owner.Valid()) {
        ImGui::TextDisabled("%s", T("Select an object to give it a tween."));
        ImGui::End();
        return;
    }
    anim::TweenComponent* tc = owner.Registry()->try_get<anim::TweenComponent>(owner.Entity());
    if (!tc || tc->Tweens.empty()) {
        EditorIcons::Inline("clock");
        ImGui::SameLine(0.0f, EditorIcons::TextGap());
        ImGui::Text("%s %s", T("No tweens on"), owner.Name().c_str());
        ImGui::TextDisabled("%s", T("A tween changes a property from one value to another over time — no clip needed."));
        ImGui::BeginDisabled(host.InPlayMode());
        if (EditorIcons::Button("plus", T("Create tween"))) {
            anim::TweenComponent& c = owner.Registry()->get_or_emplace<anim::TweenComponent>(owner.Entity());
            anim::TweenClip clip;
            clip.Name = "Tween";
            c.Tweens.push_back(clip);
            m_index = (int)c.Tweens.size() - 1;
            host.PushUndoSnapshot();
        }
        ImGui::EndDisabled();
        ImGui::End();
        return;
    }

    DrawHeader(host, *tc);
    anim::TweenClip* clip = Clip(host);
    if (!clip) {
        ImGui::End();
        return;
    }
    DrawSettings(host, *clip);
    ImGui::Separator();
    const entt::entity target = anim::TargetOf(scene, owner.Entity(), *clip);
    DrawTracks(host, *clip, target);
    ImGui::End();
}

// --- Шапка: объект, какой твин, кнопки просмотра -------------------------------------
void TweenPanel::DrawHeader(EditorHost& host, anim::TweenComponent& tc) {
    GameObject owner = host.CurrentScene().Get(m_ownerId);
    const bool play = host.InPlayMode();

    // Play / Pause / Reset — первыми: ради них окно и открывают.
    const bool running = m_previewing && !m_paused && host.CurrentScene().Tweens.IsActive(m_handle);
    ImGui::BeginDisabled(play);
    if (EditorIcons::IconOnlyButton("play", T("Play"), running)) Play(host);
    ImGui::SameLine();
    if (EditorIcons::IconOnlyButton("pause", T("Pause"), m_paused)) Pause(host);
    ImGui::SameLine();
    if (EditorIcons::IconOnlyButton("refresh", T("Reset: put the object back as it was"))) StopPreview(host);
    ImGui::EndDisabled();
    ImGui::SameLine();
    const anim::TweenClip* clip = Clip(host);
    ImGui::AlignTextToFramePadding();
    char timeText[48];
    std::snprintf(timeText, sizeof(timeText), "%.2f / %.2f", PreviewTime(host), clip ? clip->Length() : 0.0f);
    ImGui::TextDisabled("%s %s", timeText, T("s"));
    if (play) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", T("(in the game tweens run by themselves)"));
    }

    // Объект и твин.
    ImGui::SameLine(0.0f, 24.0f);
    EditorIcons::Inline("cube");
    ImGui::SameLine(0.0f, EditorIcons::TextGap());
    ImGui::TextUnformatted(owner.Name().c_str());
    ImGui::SameLine();
    if (EditorIcons::IconOnlyButton(m_follow ? "unlock" : "lock",
                                    m_follow ? T("Follows the selection — click to pin this object")
                                             : T("Pinned — click to follow the selection"),
                                    !m_follow))
        m_follow = !m_follow;

    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::BeginCombo("##tween", tc.Tweens[(size_t)m_index].Name.c_str())) {
        for (int i = 0; i < (int)tc.Tweens.size(); ++i) {
            ImGui::PushID(i);
            if (ImGui::Selectable(tc.Tweens[(size_t)i].Name.c_str(), i == m_index) && i != m_index) {
                StopPreview(host);
                m_index = i;
                m_selectedTrack = 0;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(play);
    if (EditorIcons::IconOnlyButton("plus", T("New tween on this object"))) {
        StopPreview(host);
        anim::TweenClip c;
        c.Name = "Tween " + std::to_string(tc.Tweens.size() + 1);
        tc.Tweens.push_back(c);
        m_index = (int)tc.Tweens.size() - 1;
        m_selectedTrack = 0;
        host.PushUndoSnapshot();
    }
    ImGui::SameLine();
    if (EditorIcons::IconOnlyButton("trash", T("Delete this tween"))) {
        StopPreview(host);
        tc.Tweens.erase(tc.Tweens.begin() + m_index);
        m_index = std::max(0, m_index - 1);
        if (tc.Tweens.empty()) owner.Registry()->remove<anim::TweenComponent>(owner.Entity());
        host.PushUndoSnapshot();
    }
    ImGui::EndDisabled();
}

// --- Настройки твина целиком: имя, цель, время, повтор -------------------------------
void TweenPanel::DrawSettings(EditorHost& host, anim::TweenClip& clip) {
    ImGui::BeginDisabled(host.InPlayMode());
    const float field = 90.0f;

    char name[64];
    std::snprintf(name, sizeof(name), "%s", clip.Name.c_str());
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(T("Name"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130.0f);
    if (ImGui::InputText("##name", name, sizeof(name), ImGuiInputTextFlags_EnterReturnsTrue) ||
        (ImGui::IsItemDeactivatedAfterEdit())) {
        if (clip.Name != name && name[0]) {
            clip.Name = name;
            Edited(host);
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("By this name a script plays it: Tween.play(obj, \"Name\")"));

    // Длительность — растягивает весь твин, пропорции дорожек сохраняются.
    ImGui::SameLine(0.0f, 16.0f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(T("Duration"));
    ImGui::SameLine();
    float length = clip.Length();
    ImGui::SetNextItemWidth(field);
    if (ImGui::DragFloat("##len", &length, 0.01f, 0.01f, 600.0f, "%.2f s")) {
        StopPreview(host);
        if (clip.Tracks.empty()) {
            anim::TweenTrack gap;
            gap.Duration = std::max(length, 0.01f);
            clip.Tracks.push_back(gap);
        } else {
            clip.Stretch(std::max(length, 0.01f));
        }
    }
    host.TrackLastImGuiItem();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Stretches or squeezes the whole tween in time"));

    ImGui::SameLine(0.0f, 16.0f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(T("Delay"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(field);
    if (ImGui::DragFloat("##delay", &clip.Delay, 0.01f, 0.0f, 600.0f, "%.2f s")) StopPreview(host);
    host.TrackLastImGuiItem();

    ImGui::SameLine(0.0f, 16.0f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(T("Loop"));
    ImGui::SameLine();
    static const char* kLoops[] = {"Once", "Loop", "Ping-pong"};
    ImGui::SetNextItemWidth(110.0f);
    if (ImGui::BeginCombo("##loop", T(kLoops[(int)clip.Loop]))) {
        for (int i = 0; i < 3; ++i)
            if (ImGui::Selectable(T(kLoops[i]), i == (int)clip.Loop) && i != (int)clip.Loop) {
                clip.Loop = (anim::TweenLoop)i;
                Edited(host);
            }
        ImGui::EndCombo();
    }

    // Воспроизведение: скорость, задом наперёд, сам при запуске.
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(T("Speed"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(field);
    if (ImGui::DragFloat("##speed", &clip.Speed, 0.01f, 0.01f, 20.0f, "x%.2f")) StopPreview(host);
    host.TrackLastImGuiItem();
    ImGui::SameLine(0.0f, 16.0f);
    if (ImGui::Checkbox(T("Reverse"), &clip.Reverse)) Edited(host);
    ImGui::SameLine(0.0f, 16.0f);
    if (ImGui::Checkbox(T("Play on start"), &clip.PlayOnStart)) Edited(host);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Starts by itself when the game starts"));
    // Цель — последней в строке: слот объекта занимает всю оставшуюся ширину.
    ImGui::SameLine(0.0f, 16.0f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(T("Target"));
    ImGui::SameLine();
    objectslot::Options opt;
    opt.EmptyLabel = "This object";
    opt.SelfId = m_ownerId;
    const objectslot::Result r = objectslot::Draw(host, "target", clip.Target, opt);
    if (r.Changed) {
        StopPreview(host);
        clip.Target = r.Id == m_ownerId ? 0 : r.Id;
        host.PushUndoSnapshot();
    }

    ImGui::EndDisabled();
}

// --- Значение свойства по числу компонент ------------------------------------------------
bool TweenPanel::DrawValue(const char* id, const anim::PropertyType* p, glm::vec4& v) {
    ImGui::PushID(id);
    bool changed = false;
    const int comps = p ? p->Components : 1;
    if (IsColor(p)) {
        changed = comps >= 4 ? Sage::UI::ColorField4("##c", &v.x, Sage::UI::ColorField_Compact)
                             : Sage::UI::ColorField3("##c", &v.x, Sage::UI::ColorField_Compact);
    } else {
        ImGui::SetNextItemWidth(-FLT_MIN);
        switch (comps) {
            case 1: changed = ImGui::DragFloat("##v", &v.x, 0.01f, 0.0f, 0.0f, "%.2f"); break;
            case 2: changed = ImGui::DragFloat2("##v", &v.x, 0.05f, 0.0f, 0.0f, "%.2f"); break;
            case 3: changed = ImGui::DragFloat3("##v", &v.x, 0.05f, 0.0f, 0.0f, "%.2f"); break;
            default: changed = ImGui::DragFloat4("##v", &v.x, 0.01f, 0.0f, 0.0f, "%.2f"); break;
        }
    }
    ImGui::PopID();
    return changed;
}

// --- Дорожки: таблица свойств + шкала времени --------------------------------------------
void TweenPanel::DrawTracks(EditorHost& host, anim::TweenClip& clip, entt::entity target) {
    Scene& scene = host.CurrentScene();
    entt::registry& reg = scene.Registry();
    const bool play = host.InPlayMode();
    if (target == entt::null) {
        ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Warn), "%s",
                           T("The target object is gone — pick another target."));
        return;
    }

    // Кнопки над таблицей: добавить свойство, паузу; расставить по очереди/вместе.
    ImGui::BeginDisabled(play);
    if (EditorIcons::Button("plus", T("Property"))) ImGui::OpenPopup("##addprop");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Add a property the tween changes"));
    {
        Sage::UI::MenuScope menu;
        if (ImGui::BeginPopup("##addprop")) {
            std::string group;
            for (const anim::PropertyType* p : anim::PropertiesFor(reg, target)) {
                if (p->Group != group) {
                    group = p->Group;
                    Sage::UI::MenuSection(T(group.c_str()), ImGui::GetCursorPosY() < 20.0f);
                }
                if (ImGui::MenuItem((std::string(T(p->Title.c_str())) + "##" + p->Id).c_str())) {
                    StopPreview(host);
                    anim::TweenTrack t;
                    t.Property = p->Id;
                    // Конец — нынешнее значение: ничего не прыгнет, пока его
                    // не поправят; начало — «как есть в момент старта».
                    anim::ReadProperty(*p, reg, target, t.To);
                    t.From = t.To;
                    t.Duration = clip.Tracks.empty() ? 1.0f : clip.Tracks.back().Duration;
                    clip.AppendWith(t);
                    m_selectedTrack = (int)clip.Tracks.size() - 1;
                    host.PushUndoSnapshot();
                }
            }
            ImGui::EndPopup();
        }
    }
    ImGui::SameLine();
    if (EditorIcons::Button("clock", T("Delay"))) {
        StopPreview(host);
        anim::TweenTrack gap;
        gap.Duration = 0.25f;
        clip.AppendAfter(gap);
        host.PushUndoSnapshot();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("A pause: what comes after it waits"));
    ImGui::SameLine(0.0f, 24.0f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", T("Arrange:"));
    ImGui::SameLine();
    if (ImGui::Button(T("One after another"))) {
        StopPreview(host);
        clip.ArrangeSequence();
        host.PushUndoSnapshot();
    }
    ImGui::SameLine();
    if (ImGui::Button(T("All together"))) {
        StopPreview(host);
        clip.ArrangeParallel();
        host.PushUndoSnapshot();
    }
    ImGui::EndDisabled();

    if (clip.Tracks.empty()) {
        ImGui::TextDisabled("%s", T("Add a property: position, scale, colour, opacity…"));
        return;
    }
    m_selectedTrack = std::clamp(m_selectedTrack, 0, (int)clip.Tracks.size() - 1);

    // Таблица слева, редактор кривой справа.
    const float curveW = 200.0f;
    const float avail = ImGui::GetContentRegionAvail().x;
    if (!ImGui::BeginChild("##rows", ImVec2(std::max(avail - curveW - 8.0f, 300.0f), 0.0f))) {
        ImGui::EndChild();
        return;
    }
    const ImGuiTableFlags tf = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                               ImGuiTableFlags_Resizable;
    if (ImGui::BeginTable("##tracks", 5, tf)) {
        ImGui::TableSetupColumn(T("Property"), ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableSetupColumn(T("From value"), ImGuiTableColumnFlags_WidthFixed, 140.0f);
        ImGui::TableSetupColumn(T("To value"), ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableSetupColumn(T("Ease"), ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn(T("Time"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        // Линейка времени — над колонкой «Время».
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(4);
        const float rulerW = ImGui::GetContentRegionAvail().x;
        DrawRuler(host, clip, rulerW);

        int moveFrom = -1, moveTo = -1, remove = -1;
        for (size_t i = 0; i < clip.Tracks.size(); ++i) {
            anim::TweenTrack& t = clip.Tracks[i];
            const anim::PropertyType* p = anim::FindProperty(t.Property);
            ImGui::PushID((int)i);
            ImGui::TableNextRow();

            // Свойство + порядок + удалить.
            ImGui::TableSetColumnIndex(0);
            ImGui::BeginDisabled(play);
            if (ImGui::ArrowButton("##up", ImGuiDir_Up) && i > 0) { moveFrom = (int)i; moveTo = (int)i - 1; }
            ImGui::SameLine(0.0f, 2.0f);
            if (ImGui::ArrowButton("##down", ImGuiDir_Down) && i + 1 < clip.Tracks.size()) {
                moveFrom = (int)i;
                moveTo = (int)i + 1;
            }
            ImGui::SameLine(0.0f, 4.0f);
            ImGui::EndDisabled();
            const bool selected = (int)i == m_selectedTrack;
            if (ImGui::Selectable(PropertyLabel(p, t.Property).c_str(), selected, 0,
                                  ImVec2(std::max(ImGui::GetContentRegionAvail().x - 26.0f, 20.0f), 0.0f)))
                m_selectedTrack = (int)i;
            if (ImGui::IsItemHovered()) {
                if (p) ImGui::SetTooltip("%s · %s", T(p->Group.c_str()), T(p->Title.c_str()));
                else if (!t.Property.empty())
                    ImGui::SetTooltip("%s", T("This object has no such property — the row is skipped"));
            }
            ImGui::SameLine(0.0f, 2.0f);
            ImGui::BeginDisabled(play);
            if (EditorIcons::IconOnlyButton("trash", T("Remove"))) remove = (int)i;
            ImGui::EndDisabled();

            ImGui::BeginDisabled(play);
            if (t.Property.empty()) {
                // Пауза: только её длина.
                ImGui::TableSetColumnIndex(1);
                ImGui::TextDisabled("%s", T("pause"));
                ImGui::TableSetColumnIndex(2);
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::DragFloat("##gap", &t.Duration, 0.01f, 0.0f, 600.0f, "%.2f s")) StopPreview(host);
                host.TrackLastImGuiItem();
            } else {
                // Начало: «как есть» или своё значение.
                ImGui::TableSetColumnIndex(1);
                bool fromNow = t.FromCurrent;
                if (ImGui::Checkbox("##now", &fromNow)) {
                    t.FromCurrent = fromNow;
                    if (!fromNow && p) anim::ReadProperty(*p, reg, target, t.From);
                    Edited(host);
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", T("On: start from whatever the value is when the step begins"));
                ImGui::SameLine();
                if (t.FromCurrent) {
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("%s", T("current"));
                } else {
                    if (DrawValue("from", p, t.From)) StopPreview(host);
                    host.TrackLastImGuiItem();
                }

                // Конец + «взять нынешнее значение».
                ImGui::TableSetColumnIndex(2);
                if (EditorIcons::IconOnlyButton("eyedropper", T("Take the value the object has now"))) {
                    StopPreview(host);
                    if (p) anim::ReadProperty(*p, reg, target, t.To);
                    host.PushUndoSnapshot();
                }
                ImGui::SameLine(0.0f, 2.0f);
                if (DrawValue("to", p, t.To)) StopPreview(host);
                host.TrackLastImGuiItem();

                // Кривая: подпись с маленьким графиком, щелчок — в редактор кривой.
                ImGui::TableSetColumnIndex(3);
                const ImVec2 at = ImGui::GetCursorScreenPos();
                const float h = ImGui::GetFrameHeight();
                if (ImGui::Selectable(("##ease" + std::to_string(i)).c_str(), selected, 0, ImVec2(-FLT_MIN, h)))
                    m_selectedTrack = (int)i;
                ImDrawList* dl = ImGui::GetWindowDrawList();
                PlotEase(dl, ImVec2(at.x + 2, at.y + 3), ImVec2(at.x + h + 6, at.y + h - 3), t.Curve,
                         ImGui::GetColorU32(EditorTheme::Color(EditorTheme::Role::Accent)), 1.5f);
                dl->AddText(ImVec2(at.x + h + 12, at.y + (h - ImGui::GetTextLineHeight()) * 0.5f),
                            ImGui::GetColorU32(ImGuiCol_Text), EaseLabel(t.Curve).c_str());
            }
            ImGui::EndDisabled();

            // Полоса на шкале.
            ImGui::TableSetColumnIndex(4);
            DrawTimelineRow(host, clip, i, rulerW);
            ImGui::PopID();
        }
        ImGui::EndTable();

        if (remove >= 0) {
            StopPreview(host);
            clip.Tracks.erase(clip.Tracks.begin() + remove);
            host.PushUndoSnapshot();
        } else if (moveFrom >= 0) {
            StopPreview(host);
            std::swap(clip.Tracks[(size_t)moveFrom], clip.Tracks[(size_t)moveTo]);
            if (m_selectedTrack == moveFrom) m_selectedTrack = moveTo;
            host.PushUndoSnapshot();
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();
    if (ImGui::BeginChild("##curve", ImVec2(0.0f, 0.0f))) {
        if (m_selectedTrack < (int)clip.Tracks.size()) {
            anim::TweenTrack& t = clip.Tracks[(size_t)m_selectedTrack];
            if (t.Property.empty()) ImGui::TextDisabled("%s", T("A pause has no curve."));
            else DrawCurveEditor(host, t);
        }
    }
    ImGui::EndChild();
}

// --- Линейка времени: деления и бегунок --------------------------------------------------
void TweenPanel::DrawRuler(EditorHost& host, anim::TweenClip& clip, float width) {
    const float length = std::max(clip.Length(), 0.01f);
    // Видимая часть шкалы — с запасом за концом, чтобы полосу было куда тянуть.
    m_visibleSeconds = std::max(length * 1.25f, 0.5f);
    m_pxPerSec = std::max(width, 40.0f) / m_visibleSeconds;
    const float h = ImGui::GetFrameHeight();
    const ImVec2 a = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 line = ImGui::GetColorU32(EditorTheme::Color(EditorTheme::Role::LineStrong));
    const ImU32 text = ImGui::GetColorU32(EditorTheme::Color(EditorTheme::Role::TextDim));

    // Шаг делений — «круглый», чтобы подписей было 4–10 на ширину.
    // Шаг делений — «круглый» и такой, чтобы подписи не налезали друг на
    // друга: не чаще, чем ширина подписи с зазором.
    const float labelW = ImGui::CalcTextSize("0.00").x + 8.0f;
    float step = 60.0f;
    for (float s : {0.05f, 0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f, 30.0f, 60.0f}) {
        if (s * m_pxPerSec >= labelW) { step = s; break; }
    }
    for (float t = 0.0f; t <= m_visibleSeconds + 1e-4f; t += step) {
        const float x = a.x + t * m_pxPerSec;
        dl->AddLine(ImVec2(x, a.y + h * 0.55f), ImVec2(x, a.y + h), line);
        char buf[16];
        std::snprintf(buf, sizeof(buf), step < 1.0f ? "%.2f" : "%.0f", t);
        dl->AddText(ImVec2(x + 2.0f, a.y), text, buf);
    }
    // Конец твина — ручка: тянешь — растягиваешь весь твин.
    const float endX = a.x + length * m_pxPerSec;
    ImGui::SetCursorScreenPos(ImVec2(endX - 5.0f, a.y));
    ImGui::BeginDisabled(host.InPlayMode());
    ImGui::InvisibleButton("##end", ImVec2(10.0f, h));
    ImGui::EndDisabled();
    const bool endHot = ImGui::IsItemHovered() || ImGui::IsItemActive();
    if (endHot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (ImGui::IsItemActivated()) StopPreview(host);
    if (ImGui::IsItemActive()) {
        const float mouseT = (ImGui::GetIO().MousePos.x - a.x) / m_pxPerSec;
        clip.Stretch(std::max(mouseT, 0.05f));
    }
    if (ImGui::IsItemDeactivated()) host.PushUndoSnapshot();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Drag to stretch the whole tween"));
    dl->AddTriangleFilled(ImVec2(endX - 5, a.y), ImVec2(endX + 5, a.y), ImVec2(endX, a.y + 8),
                          ImGui::GetColorU32(EditorTheme::Color(endHot ? EditorTheme::Role::AccentHover
                                                                       : EditorTheme::Role::Accent)));

    // Бегунок — щелчок по линейке ставит момент (просмотр на паузе).
    ImGui::SetCursorScreenPos(a);
    ImGui::InvisibleButton("##scrub", ImVec2(std::max(endX - a.x - 6.0f, 1.0f), h));
    if (ImGui::IsItemActive() && !host.InPlayMode()) {
        const float t = std::clamp((ImGui::GetIO().MousePos.x - a.x) / m_pxPerSec, 0.0f, length);
        if (!m_previewing) Play(host);
        if (m_previewing) {
            Pause(host);
            host.CurrentScene().Tweens.Seek(m_handle, t);
            m_lastTime = t;
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Click or drag to see the tween at that moment"));
    if (m_previewing) {
        const float x = a.x + std::min(PreviewTime(host), length) * m_pxPerSec;
        dl->AddLine(ImVec2(x, a.y), ImVec2(x, a.y + h), ImGui::GetColorU32(EditorTheme::Color(EditorTheme::Role::Warn)),
                    2.0f);
    }
    ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + h));
    ImGui::Dummy(ImVec2(width, 0.0f));
}

// --- Полоса дорожки: середина — когда, края — сколько ------------------------------------
void TweenPanel::DrawTimelineRow(EditorHost& host, anim::TweenClip& clip, size_t index, float width) {
    anim::TweenTrack& t = clip.Tracks[index];
    const float h = ImGui::GetFrameHeight();
    const ImVec2 a = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float x0 = a.x + t.Start * m_pxPerSec;
    const float x1 = a.x + t.End() * m_pxPerSec;
    const bool gap = t.Property.empty();
    const bool selected = (int)index == m_selectedTrack;
    const bool play = host.InPlayMode();
    const float edge = 6.0f;

    // Края — отдельными кнопками поверх середины: за край тянут длительность.
    ImGui::BeginDisabled(play);
    auto handle = [&](const char* id, float x, float w, int mode) {
        ImGui::SetCursorScreenPos(ImVec2(x, a.y + 2.0f));
        ImGui::InvisibleButton(id, ImVec2(std::max(w, 1.0f), h - 4.0f));
        if (ImGui::IsItemHovered() || ImGui::IsItemActive())
            ImGui::SetMouseCursor(mode == 0 ? ImGuiMouseCursor_ResizeAll : ImGuiMouseCursor_ResizeEW);
        // Сдвиг копится между кадрами: шаг 0.05 с съел бы медленное движение
        // мыши целиком, и полоса не сдвинулась бы вовсе.
        static float carry = 0.0f;
        if (ImGui::IsItemActivated()) {
            StopPreview(host);
            m_selectedTrack = (int)index;
            carry = 0.0f;
        }
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
            // Шаг 0.05 с: «ровно полсекунды» ставится мышью, а не угадывается.
            // Shift — без шага.
            carry += ImGui::GetIO().MouseDelta.x / m_pxPerSec;
            auto snap = [](float v) { return ImGui::GetIO().KeyShift ? v : std::round(v / 0.05f) * 0.05f; };
            if (mode == 0) {
                const float s = snap(std::max(0.0f, t.Start + carry));
                carry -= s - t.Start;
                t.Start = s;
            } else if (mode == 1) {
                const float end = t.End();
                const float s = snap(std::clamp(t.Start + carry, 0.0f, end - 0.01f));
                carry -= s - t.Start;
                t.Start = s;
                t.Duration = end - s;
            } else {
                const float d = snap(std::max(0.01f, t.Duration + carry));
                carry -= d - t.Duration;
                t.Duration = std::max(d, 0.01f);
            }
        }
        if (ImGui::IsItemDeactivated()) host.PushUndoSnapshot();
    };
    handle("##body", x0 + edge, x1 - x0 - edge * 2.0f, 0);
    handle("##left", x0, edge, 1);
    handle("##right", x1 - edge, edge, 2);
    ImGui::EndDisabled();

    const ImU32 fill = ImGui::GetColorU32(EditorTheme::Color(
        gap ? EditorTheme::Role::SurfaceAlt : selected ? EditorTheme::Role::Accent : EditorTheme::Role::AccentMuted));
    dl->AddRectFilled(ImVec2(x0, a.y + 2.0f), ImVec2(x1, a.y + h - 2.0f), fill, 3.0f);
    dl->AddRect(ImVec2(x0, a.y + 2.0f), ImVec2(x1, a.y + h - 2.0f),
                ImGui::GetColorU32(EditorTheme::Color(EditorTheme::Role::LineStrong)), 3.0f);
    if (!gap && x1 - x0 > 24.0f)
        PlotEase(dl, ImVec2(x0 + 3, a.y + 4), ImVec2(x1 - 3, a.y + h - 4), t.Curve,
                 ImGui::GetColorU32(EditorTheme::Color(selected ? EditorTheme::Role::TextOnAccent
                                                                : EditorTheme::Role::Text)),
                 1.0f);
    char dur[24];
    std::snprintf(dur, sizeof(dur), "%.2fs", t.Duration);
    const ImVec2 ts = ImGui::CalcTextSize(dur);
    if (x1 + 4.0f + ts.x < a.x + width)
        dl->AddText(ImVec2(x1 + 4.0f, a.y + (h - ts.y) * 0.5f),
                    ImGui::GetColorU32(EditorTheme::Color(EditorTheme::Role::TextDim)), dur);
    ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + h));
    ImGui::Dummy(ImVec2(width, 0.0f));
}

// --- Редактор кривой -----------------------------------------------------------------------
void TweenPanel::DrawCurveEditor(EditorHost& host, anim::TweenTrack& t) {
    const bool play = host.InPlayMode();
    ImGui::BeginDisabled(play);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##shape", ShapeLabel(t.Curve.Shape))) {
        for (int i = 0; i < (int)anim::EaseShape::Count; ++i) {
            ImGui::PushID(i);
            const bool picked = ImGui::Selectable(ShapeLabel((anim::EaseShape)i), i == (int)t.Curve.Shape);
            ImGui::PopID();
            if (picked && i != (int)t.Curve.Shape) {
                // Своя кривая начинается с формы, которая была: правят, а не
                // рисуют заново.
                if ((anim::EaseShape)i == anim::EaseShape::Curve) t.Curve.Bezier = anim::BezierFor(t.Curve);
                t.Curve.Shape = (anim::EaseShape)i;
                Edited(host);
            }
        }
        ImGui::EndCombo();
    }
    // Направление — у всех форм, кроме прямой и своей кривой.
    const bool modes = t.Curve.Shape != anim::EaseShape::Linear && t.Curve.Shape != anim::EaseShape::Curve;
    if (modes) {
        // Списком, а не тремя кнопками: «Разгон и торможение» в узкой
        // колонке рядом со шкалой не помещается.
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##mode", ModeLabel(t.Curve.Mode))) {
            for (int m = 0; m < 3; ++m)
                if (ImGui::Selectable(ModeLabel((anim::EaseMode)m), (int)t.Curve.Mode == m) &&
                    (int)t.Curve.Mode != m) {
                    t.Curve.Mode = (anim::EaseMode)m;
                    Edited(host);
                }
            ImGui::EndCombo();
        }
    }

    // График: время по x, значение по y (с запасом на перелёт Back/Elastic).
    const float w = ImGui::GetContentRegionAvail().x;
    const float side = std::clamp(std::min(w, ImGui::GetContentRegionAvail().y - 30.0f), 80.0f, 220.0f);
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const ImVec2 b(a.x + side, a.y + side);
    const float pad = side * 0.2f;   // y ∈ [-0.25, 1.25]
    const ImVec2 g0(a.x, a.y + pad), g1(b.x, b.y - pad);
    auto toScreen = [&](float x, float y) { return ImVec2(g0.x + x * (g1.x - g0.x), g1.y - y * (g1.y - g0.y)); };
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, b, ImGui::GetColorU32(EditorTheme::Color(EditorTheme::Role::Input)), 4.0f);
    const ImU32 grid = ImGui::GetColorU32(EditorTheme::Color(EditorTheme::Role::Line));
    dl->AddLine(toScreen(0, 0), toScreen(1, 0), grid);
    dl->AddLine(toScreen(0, 1), toScreen(1, 1), grid);
    dl->AddLine(toScreen(0, 0), toScreen(1, 1), grid);   // прямая — для сравнения
    PlotEase(dl, g0, g1, t.Curve, ImGui::GetColorU32(EditorTheme::Color(EditorTheme::Role::Accent)), 2.0f);

    if (t.Curve.Shape == anim::EaseShape::Curve) {
        // Две ручки: тянешь — меняешь форму. x держится в [0, 1] (время не
        // идёт назад), y свободен — так кривая перелетает цель.
        glm::vec4& c = t.Curve.Bezier;
        const ImU32 handleCol = ImGui::GetColorU32(EditorTheme::Color(EditorTheme::Role::Warn));
        for (int k = 0; k < 2; ++k) {
            float& hx = k == 0 ? c.x : c.z;
            float& hy = k == 0 ? c.y : c.w;
            const ImVec2 anchor = k == 0 ? toScreen(0, 0) : toScreen(1, 1);
            const ImVec2 p = toScreen(hx, hy);
            dl->AddLine(anchor, p, handleCol);
            ImGui::SetCursorScreenPos(ImVec2(p.x - 6, p.y - 6));
            ImGui::PushID(k);
            ImGui::InvisibleButton("##h", ImVec2(12, 12));
            ImGui::PopID();
            const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
            dl->AddCircleFilled(p, hot ? 6.0f : 4.5f, handleCol);
            if (ImGui::IsItemActivated()) StopPreview(host);
            if (ImGui::IsItemActive()) {
                const ImVec2 m = ImGui::GetIO().MousePos;
                hx = std::clamp((m.x - g0.x) / (g1.x - g0.x), 0.0f, 1.0f);
                hy = std::clamp((g1.y - m.y) / (g1.y - g0.y), -1.0f, 2.0f);
            }
            if (ImGui::IsItemDeactivated()) host.PushUndoSnapshot();
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(a.x, b.y + 4.0f));
    if (t.Curve.Shape != anim::EaseShape::Curve) {
        if (ImGui::Button(T("Edit as curve"))) {
            t.Curve.Bezier = anim::BezierFor(t.Curve);
            t.Curve.Shape = anim::EaseShape::Curve;
            Edited(host);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Turns the shape into a curve with two handles you can drag"));
    }
    ImGui::EndDisabled();
}
