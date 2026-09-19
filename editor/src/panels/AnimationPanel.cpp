#include "AnimationPanel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

#include "../EditorHost.h"
#include "../EditorIcons.h"
#include "../EditorTheme.h"
#include "../Localization.h"
#include "../PanelWindows.h"
#include "../Project.h"
#include "../ui/UI.h"
#include "sage/anim/AnimProperty.h"
#include "sage/anim/ClipFile.h"
#include "sage/anim/PropertyAnimator.h"
#include "sage/core/Log.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"

namespace fs = std::filesystem;
namespace anim = sage::anim;

namespace {

constexpr float kTrackHeight = 22.0f;
constexpr float kKeyRadius = 5.0f;
constexpr float kRulerHeight = 22.0f;

// Путь к объекту ОТ ВЛАДЕЛЬЦА КЛИПА. Владелец — выбранный объект: клип
// адресует не «сущность номер сорок», а «мой ребёнок по имени Icon», иначе он
// не лёг бы ни на одну вторую кнопку.
std::string PathFromOwner(Scene& scene, entt::entity owner, entt::entity target) {
    if (owner == target) return {};
    entt::registry& reg = scene.Registry();
    std::string path;
    entt::entity cur = target;
    while (cur != entt::null && cur != owner) {
        const NameComponent* n = reg.try_get<NameComponent>(cur);
        if (!n) return {};
        path = path.empty() ? n->Name : (n->Name + "/" + path);
        cur = scene.ParentOf(cur);
    }
    // Цель не под владельцем — дорожку класть некуда: клип, ссылающийся вверх,
    // перестанет работать на втором экземпляре.
    return cur == owner ? path : std::string();
}

const char* InterpTitle(anim::Interp i) {
    switch (i) {
        case anim::Interp::Constant: return T("Constant");
        case anim::Interp::Bezier: return T("Bezier");
        default: return T("Linear");
    }
}

} // namespace

void AnimationPanel::PushUndo() {
    m_undo.push_back(m_clip);
    if (m_undo.size() > 64) m_undo.erase(m_undo.begin());
    m_redo.clear();
    m_dirty = true;
}

void AnimationPanel::OpenClip(EditorHost& host, const std::string& path) {
    anim::PropertyClip c;
    std::string err;
    const bool skeletal = fs::path(path).extension() == ".sageanim";
    if (skeletal) {
        // КЛИП МОДЕЛИ — ТОЛЬКО ПОСМОТРЕТЬ. Он описывает движение костей, а не
        // свойств сцены, и пересоздаётся при каждом переимпорте модели.
        // Показываем его настоящую раскладку ключей — по ней видно, где в
        // движении что происходит, и под неё ставят свои ключи, — но править
        // нечего: правка жила бы до первого переимпорта.
        try {
            const sage::anim::ClipAsset asset = sage::anim::LoadClip(path);
            c.Name = asset.Name.empty() ? fs::path(path).stem().string() : asset.Name;
            c.Duration = asset.Duration > 0.0f ? asset.Duration : 1.0f;
            c.Imported = true;
            for (const sage::anim::ClipAsset::Channel& ch : asset.Channels) {
                anim::Track t;
                t.Target = ch.Joint;
                t.Property = ch.Target == sage::anim::AnimPath::Translation ? "joint.translation"
                             : ch.Target == sage::anim::AnimPath::Rotation  ? "joint.rotation"
                                                                : "joint.scale";
                for (size_t i = 0; i < ch.Times.size(); ++i) {
                    anim::Key k;
                    k.Time = ch.Times[i];
                    if (i < ch.Values.size()) k.Value = ch.Values[i];
                    t.Keys.push_back(k);
                }
                c.Tracks.push_back(std::move(t));
            }
        } catch (const std::exception& e) {
            m_status = std::string(T("Could not open the clip: ")) + e.what();
            return;
        }
    } else if (!anim::LoadClipFile(path, c, err)) {
        m_status = std::string(T("Could not open the clip: ")) + err;
        return;
    }
    m_skeletal = skeletal;
    m_clip = std::move(c);
    m_path = path;
    m_dirty = false;
    m_time = 0.0f;
    m_selection.clear();
    m_undo.clear();
    m_redo.clear();
    m_status.clear();
    (void)host;
}

// Ставит ключ на ВСЕ дорожки (или на одну), взяв текущее значение из сцены.
// Именно так работает «записать позу»: человек расставил объекты как надо и
// говорит «вот так в этот момент», а не вбивает числа по одному.
void AnimationPanel::CaptureKey(EditorHost& host, int trackIndex) {
    Scene& scene = host.CurrentScene();
    entt::registry& reg = scene.Registry();
    GameObject owner = host.SelectedObject();
    if (!owner.Valid()) return;
    PushUndo();
    for (int i = 0; i < (int)m_clip.Tracks.size(); ++i) {
        if (trackIndex >= 0 && i != trackIndex) continue;
        anim::Track& t = m_clip.Tracks[i];
        const anim::PropertyType* p = anim::FindProperty(t.Property);
        if (!p) continue;
        const entt::entity target = anim::ResolveTarget(scene, owner.Entity(), t.Target);
        if (target == entt::null) continue;
        glm::vec4 v(0.0f);
        if (!anim::ReadProperty(*p, reg, target, v)) continue;
        anim::SetKey(t, m_time, v);
    }
}

// Показывает позу клипа в сцене на текущем времени. Инструмент правит клип, а
// смотреть на результат надо на настоящих объектах: иначе «подобрал кривую» и
// «увидел, что получилось» — два разных дела с переключением окон между ними.
void AnimationPanel::SyncSceneToTime(EditorHost& host) {
    GameObject owner = host.SelectedObject();
    if (!owner.Valid() || m_clip.Tracks.empty()) return;
    anim::ApplyClipAt(host.CurrentScene(), owner.Entity(), m_clip, m_time);
}

// --- АВТО-КЛЮЧ ---------------------------------------------------------------
//
// Пока он включён, правка объекта в сцене сама становится ключом в текущий
// момент. Это единственный способ анимировать руками: иначе каждый сдвиг надо
// подтверждать кнопкой, и половина забывается — а забытый ключ выглядит как
// «редактор потерял мою работу».
//
// СДЕЛАНО СРАВНЕНИЕМ, А НЕ ПЕРЕХВАТОМ ПРАВОК. Перехват означал бы крючок в
// каждом месте редактора, которое двигает объект: инспектор, гизмо, холст
// вёрстки, выравнивание, скрипт. Один забытый — и ключ не ставится именно там,
// где его ждут. Сравнение читает ровно то, что получилось, и потому работает
// для любого способа правки, включая те, которых ещё нет.
void AnimationPanel::AutoKeyTick(EditorHost& host) {
    if (!m_autoKey || !Editable() || m_playing) return;
    Scene& scene = host.CurrentScene();
    entt::registry& reg = scene.Registry();
    GameObject owner = host.SelectedObject();
    if (!owner.Valid()) return;
    bool pushed = false;
    for (anim::Track& t : m_clip.Tracks) {
        const anim::PropertyType* p = anim::FindProperty(t.Property);
        if (!p) continue;
        const entt::entity target = anim::ResolveTarget(scene, owner.Entity(), t.Target);
        if (target == entt::null) continue;
        glm::vec4 now(0.0f);
        if (!anim::ReadProperty(*p, reg, target, now)) continue;
        const glm::vec4 want = anim::Sample(t, m_time);
        // Порог — не «красиво», а обязательное условие: значение приходит с
        // плавающей точкой через сэмплер и обратно, и точное сравнение ставило
        // бы ключ каждый кадр на ровном месте.
        bool same = true;
        for (int c = 0; c < p->Components; ++c)
            if (std::fabs(now[c] - want[c]) > 1e-4f) { same = false; break; }
        if (same) continue;
        if (!pushed) { PushUndo(); pushed = true; }
        anim::SetKey(t, m_time, now);
    }
}

void AnimationPanel::DrawToolbar(EditorHost& host) {
    const bool editable = Editable();

    // --- Откуда клип --------------------------------------------------------
    //
    // Сказано ПЕРВЫМ делом и цветом: импортированный клип выглядит как обычный,
    // и человек, начавший его править, узнал бы правду только после
    // переимпорта модели — то есть потеряв работу.
    if (m_clip.Imported) {
        ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(EditorTheme::Role::Warn));
        EditorIcons::Inline("import");
        ImGui::SameLine(0.0f, EditorIcons::TextGap());
        ImGui::TextUnformatted(T("Imported — read only"));
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", T("This clip comes from the model file and is rebuilt on every\n"
                                      "re-import. Make a copy to edit it."));
        }
        ImGui::SameLine();
        // Скелетный клип «своим» не делается: у костей нет свойства сцены, и
        // копия была бы клипом, который этот инструмент не умеет применить.
        // Его играет компонент Animation, и сказано это прямо.
        if (m_skeletal) {
            ImGui::TextDisabled("%s", T("A skeletal clip: the Animation component plays it."));
        } else if (ImGui::SmallButton(T("Make it mine"))) {
            m_clip.Imported = false;
            m_clip.Name += " (custom)";
            m_path.clear();   // сохранится как новый файл, чужой не тронем
            m_dirty = true;
            m_status = T("The copy is editable. Save it next to the model.");
        }
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(EditorTheme::Role::Ok));
        EditorIcons::Inline("pencil");
        ImGui::SameLine(0.0f, EditorIcons::TextGap());
        ImGui::TextUnformatted(T("Custom — editable"));
        ImGui::PopStyleColor();
    }

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    char name[128];
    std::snprintf(name, sizeof(name), "%s", m_clip.Name.c_str());
    ImGui::SetNextItemWidth(160.0f);
    ImGui::BeginDisabled(!editable);
    if (ImGui::InputText(T("Clip"), name, sizeof(name))) {
        m_clip.Name = name;
        m_dirty = true;
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    ImGui::BeginDisabled(!editable);
    if (ImGui::DragFloat(T("Length"), &m_clip.Duration, 0.05f, 0.05f, 600.0f, "%.2f s")) {
        m_dirty = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!editable);
    if (ImGui::Checkbox(T("Loop"), &m_clip.Loop)) m_dirty = true;
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    // --- Показ --------------------------------------------------------------
    if (EditorIcons::IconOnlyButton(m_playing ? "pause" : "play",
                                    m_playing ? T("Stop") : T("Play"), m_playing))
        m_playing = !m_playing;
    ImGui::SameLine();
    if (EditorIcons::IconOnlyButton("stop", T("To the start"))) {
        m_playing = false;
        m_time = 0.0f;
        SyncSceneToTime(host);
    }
    ImGui::SameLine();
    // АВТО-КЛЮЧ. Пока он включён, любая правка объекта в сцене ложится ключом
    // в текущий момент. Это единственный способ анимировать «руками»: иначе
    // каждый сдвиг надо подтверждать кнопкой, и половина забывается.
    if (EditorIcons::IconOnlyButton("clock", T("Auto-key: an edit becomes a key"), m_autoKey)) {
        if (editable) m_autoKey = !m_autoKey;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!editable || m_clip.Tracks.empty());
    if (EditorIcons::IconOnlyButton("plus", T("Key the current pose (K)"))) CaptureKey(host, -1);
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (EditorIcons::IconOnlyButton("save", T("Save the clip")) && editable) {
        if (m_path.empty()) {
            // Имя файла — из имени клипа: спрашивать его отдельным диалогом
            // ровно в тот момент, когда клип уже назван, значит спрашивать
            // дважды об одном.
            const std::string stem = m_clip.Name.empty() ? std::string("clip") : m_clip.Name;
            m_path = (host.CurrentProject().AssetsDir() / (stem + ".sageclip")).string();
        }
        std::string err;
        if (anim::SaveClipFile(m_clip, m_path, err)) {
            m_dirty = false;
            m_status = std::string(T("Saved: ")) + m_path;
        } else {
            m_status = err;
        }
    }
    ImGui::SameLine();
    if (EditorIcons::IconOnlyButton("file", T("New clip"))) {
        m_clip = anim::PropertyClip{};
        m_clip.Name = T("New clip");
        m_path.clear();
        m_selection.clear();
        m_undo.clear();
        m_dirty = false;
        m_time = 0.0f;
    }

    if (m_dirty) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", T("(unsaved)"));
    }
}

void AnimationPanel::DrawAddTrackPopup(EditorHost& host) {
    if (Sage::UI::MenuScope addMenu; ImGui::BeginPopup("AddTrack###AddTrack")) {
        Scene& scene = host.CurrentScene();
        entt::registry& reg = scene.Registry();
        GameObject owner = host.SelectedObject();
        if (!owner.Valid()) {
            ImGui::TextDisabled("%s", T("Select an object first."));
            ImGui::EndPopup();
            return;
        }
        // Предлагается ТО, ЧТО У ЭТОГО ОБЪЕКТА ЕСТЬ. Список всех свойств движка
        // означал бы полсотни строк, из которых работают три.
        ImGui::TextDisabled("%s %s", T("Object:"), owner.Name().c_str());
        ImGui::Separator();
        const std::vector<const anim::PropertyType*> props =
            anim::PropertiesFor(reg, owner.Entity());
        std::string group;
        for (const anim::PropertyType* p : props) {
            if (p->Group != group) {
                group = p->Group;
                ImGui::SeparatorText(T(group.c_str()));
            }
            // Уже добавленное показываем погашенным, а не прячем: «почему здесь
            // нет размера» — вопрос, на который молчание не отвечает.
            bool already = false;
            for (const anim::Track& t : m_clip.Tracks)
                if (t.Property == p->Id && t.Target.empty()) { already = true; break; }
            ImGui::BeginDisabled(already);
            if (ImGui::MenuItem(T(p->Title.c_str()))) {
                PushUndo();
                anim::Track t;
                t.Target = PathFromOwner(scene, owner.Entity(), owner.Entity());
                t.Property = p->Id;
                m_clip.Tracks.push_back(t);
                m_activeTrack = (int)m_clip.Tracks.size() - 1;
            }
            ImGui::EndDisabled();
        }
        ImGui::EndPopup();
    }
}

void AnimationPanel::DrawTracks(EditorHost& host, float width) {
    ImGui::BeginChild("##tracks", ImVec2(width, 0), ImGuiChildFlags_Borders);
    ImGui::BeginDisabled(!Editable());
    if (EditorIcons::Button("plus", T("Track"), T("Animate one more property")))
        ImGui::OpenPopup("AddTrack###AddTrack");
    ImGui::EndDisabled();
    DrawAddTrackPopup(host);

    ImGui::Dummy(ImVec2(0.0f, kRulerHeight - ImGui::GetTextLineHeight()));

    for (int i = 0; i < (int)m_clip.Tracks.size(); ++i) {
        anim::Track& t = m_clip.Tracks[i];
        const anim::PropertyType* p = anim::FindProperty(t.Property);
        ImGui::PushID(i);
        const bool active = m_activeTrack == i;
        // Свойство, которого движок не знает, — не ошибка: клип мог прийти из
        // сборки с другими частями. Но сказать это надо, иначе «дорожка есть, а
        // не работает» выглядит поломкой.
        const std::string title =
            p ? (t.Target.empty() ? T(p->Title.c_str())
                                  : (t.Target + " · " + T(p->Title.c_str())))
            // У клипа модели свойств сцены нет по определению: там кости, и
            // писать напротив каждой «(неизвестно)» значит пугать тем, что в
            // порядке. Показываем как есть: кость и канал.
            : m_clip.Imported ? (t.Target + " · " + t.Property)
                              : (t.Property + " " + T("(unknown)"));
        if (ImGui::Selectable(title.c_str(), active, 0, ImVec2(0.0f, kTrackHeight)))
            m_activeTrack = i;
        if (!p && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", T("This build of the game has no such property.\n"
                                      "The track is kept as it is."));
        if (Sage::UI::MenuScope trackMenu; ImGui::BeginPopupContextItem("##track_ctx")) {
            ImGui::BeginDisabled(!Editable());
            if (EditorIcons::MenuItem("plus", T("Key the current pose (K)"))) CaptureKey(host, i);
            if (EditorIcons::MenuItem("trash", T("Delete the track"))) {
                PushUndo();
                m_clip.Tracks.erase(m_clip.Tracks.begin() + i);
                m_selection.clear();
                m_activeTrack = -1;
                ImGui::EndDisabled();
                ImGui::EndPopup();
                ImGui::PopID();
                break;
            }
            ImGui::EndDisabled();
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (m_clip.Tracks.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", T("No tracks yet. Select an object and add the property\n"
                                   "you want to animate."));
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();
}

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

void AnimationPanel::Draw(EditorHost& host, bool* open) {
    if (!open || !*open) return;
    if (m_focusFrames > 0) {
        ImGui::SetNextWindowFocus();
        --m_focusFrames;
    }
    ImGui::SetNextWindowSize(ImVec2(960.0f, 320.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(T("Animation" "###Animation"), open, panelwindows::WindowFlags("Animation"))) {
        ImGui::End();
        return;
    }

    DrawToolbar(host);
    ImGui::Separator();
    AutoKeyTick(host);

    // Проигрывание идёт по настоящему времени кадра: клип, который «играет» с
    // шагом в один кадр, показывает не ту скорость, под которую его настроили.
    if (m_playing) {
        m_time = anim::WrapTime(m_time + ImGui::GetIO().DeltaTime, m_clip.Duration, m_clip.Loop);
        if (!m_clip.Loop && m_time >= m_clip.Duration - 1e-4f) m_playing = false;
        SyncSceneToTime(host);
    }

    // --- Горячие клавиши ----------------------------------------------------
    //
    // Те же, что везде: пробел — играть, K — ключ, Del — убрать. Привыкать к
    // особым сочетаниям ради одной панели человек не обязан.
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (focused && !ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Space)) m_playing = !m_playing;
        if (Editable() && ImGui::IsKeyPressed(ImGuiKey_K)) CaptureKey(host, -1);
        if (Editable() && ImGui::IsKeyPressed(ImGuiKey_Delete) && !m_selection.empty()) {
            PushUndo();
            // С КОНЦА: удаление ключа сдвигает номера тех, что правее, и
            // удаление по возрастанию номеров снесло бы не те ключи.
            std::vector<KeyRef> sorted = m_selection;
            std::sort(sorted.begin(), sorted.end(), [](const KeyRef& a, const KeyRef& b) {
                return a.Track == b.Track ? a.Key > b.Key : a.Track < b.Track;
            });
            for (const KeyRef& r : sorted) {
                if (r.Track >= 0 && r.Track < (int)m_clip.Tracks.size())
                    anim::RemoveKey(m_clip.Tracks[r.Track], r.Key);
            }
            m_selection.clear();
        }
        if (Editable() && ImGui::IsKeyPressed(ImGuiKey_M)) {
            PushUndo();
            m_clip.Markers.push_back({m_time, T("Marker")});
        }
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z) && !m_undo.empty()) {
            m_redo.push_back(m_clip);
            m_clip = m_undo.back();
            m_undo.pop_back();
            m_selection.clear();
        }
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y) && !m_redo.empty()) {
            m_undo.push_back(m_clip);
            m_clip = m_redo.back();
            m_redo.pop_back();
            m_selection.clear();
        }
    }

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float trackWidth = std::min(240.0f, avail.x * 0.3f);
    // КРИВАЯ ПОЯВЛЯЕТСЯ, КОГДА ЕСТЬ МЕСТО. На панели в две строки высоты она
    // съела бы саму линейку — то есть то, ради чего панель открыли. Порог не
    // «красивое число»: ниже него кривая всё равно нечитаема, и честнее её не
    // рисовать вовсе.
    const bool roomForCurve = avail.y > 260.0f;
    const float curveHeight = roomForCurve ? std::min(180.0f, avail.y * 0.4f) : 0.0f;
    const float rowsHeight = roomForCurve ? (avail.y - curveHeight - 8.0f) : 0.0f;

    DrawTracks(host, trackWidth);
    ImGui::SameLine();
    DrawTimeline(host, ImVec2(0.0f, rowsHeight));
    if (roomForCurve) DrawCurve(host, ImVec2(0.0f, 0.0f));

    if (!m_status.empty()) ImGui::TextDisabled("%s", m_status.c_str());
    ImGui::End();
}
