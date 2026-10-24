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

namespace fs = std::filesystem;
namespace anim = sage::anim;

#include "AnimationPanelDetail.h"

void AnimationPanel::PushUndo() {
    m_undo.push_back(m_clip);
    if (m_undo.size() > 64) m_undo.erase(m_undo.begin());
    m_redo.clear();
    m_dirty = true;
}

// --- ВЛАДЕЛЕЦ КЛИПА ---------------------------------------------------------
//
// «Кто это играет» — не украшение, а условие работы: дорожки адресуют цели
// путём ОТ ВЛААДЕЛЬЦА, ключ снимается с его настоящих свойств, а в сцене клип
// живёт компонентом на нём. Без владельца править нечего и некому.

entt::entity AnimationPanel::FindOwner(EditorHost& host) const {
    if (m_path.empty()) return entt::null;
    Scene& scene = const_cast<EditorHost&>(host).CurrentScene();
    entt::registry& reg = scene.Registry();
    std::error_code ec;
    const fs::path mine = fs::weakly_canonical(fs::path(m_path), ec);
    entt::entity found = entt::null;
    reg.view<PropertyAnimatorComponent>().each(
        [&](entt::entity e, const PropertyAnimatorComponent& pa) {
            if (found != entt::null || pa.ClipPath.empty()) return;
            // Сравниваем РАЗОБРАННЫЕ пути: в компоненте путь мог быть записан
            // относительным, а открыт клип абсолютным — это тот же файл, и
            // решать, что это разные клипы, значит терять привязку на ровном
            // месте.
            std::error_code e2;
            const fs::path theirs = fs::weakly_canonical(fs::path(pa.ClipPath), e2);
            if (theirs == mine) found = e;
        });
    return found;
}

GameObject AnimationPanel::Owner(EditorHost& host) {
    Scene& scene = host.CurrentScene();
    if (m_owner != entt::null && !scene.Registry().valid(m_owner)) m_owner = entt::null;
    if (m_owner == entt::null) m_owner = FindOwner(host);
    if (m_owner == entt::null) return GameObject{};
    return GameObject(&scene.Registry(), m_owner);
}

bool AnimationPanel::CreateClipFor(EditorHost& host, GameObject object) {
    if (!object.Valid()) return false;
    // ФАЙЛ СОЗДАЁТСЯ СРАЗУ. Клип, живущий только в памяти панели, — это и есть
    // «анимация из воздуха»: закрыли редактор, и работы нет.
    const std::string stem = object.Name().empty() ? std::string("clip") : object.Name();
    fs::path path = host.CurrentProject().AssetsDir() / (stem + ".sageclip");
    // Имя занято — берём следующее свободное, а не молча перезаписываем чужой
    // клип: второй объект с тем же именем обычная вещь.
    for (int n = 2; fs::exists(path) && n < 1000; ++n)
        path = host.CurrentProject().AssetsDir() / (stem + " " + std::to_string(n) + ".sageclip");

    anim::PropertyClip c;
    c.Name = stem;
    std::string err;
    if (!anim::SaveClipFile(c, path.string(), err)) {
        m_status = err;
        return false;
    }

    m_clip = std::move(c);
    m_path = path.string();
    m_hasClip = true;
    m_skeletal = false;
    m_dirty = false;
    m_time = 0.0f;
    m_selection.clear();
    m_undo.clear();
    m_redo.clear();
    m_activeTrack = -1;
    BindTo(host, object);
    m_status = std::string(T("Clip created: ")) + m_path;
    return true;
}

void AnimationPanel::BindTo(EditorHost& host, GameObject object) {
    if (!object.Valid() || m_path.empty()) return;
    entt::registry& reg = host.CurrentScene().Registry();
    host.PushUndoSnapshot();
    PropertyAnimatorComponent& pa = reg.get_or_emplace<PropertyAnimatorComponent>(object.Entity());
    pa.ClipPath = m_path;
    pa.Clip.reset();
    pa.Ready = false;
    // В редакторе клип НЕ ИГРАЕТ САМ: позу показывает бегунок панели. Иначе
    // объект уезжал бы из-под рук, едва его привязали.
    pa.Playing = false;
    m_owner = object.Entity();
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
    m_hasClip = true;
    // Кто его играет — вопрос к СЦЕНЕ, а не к тому, что выбрано мышью.
    m_owner = FindOwner(host);
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
    GameObject owner = Owner(host);
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
    GameObject owner = Owner(host);
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
    GameObject owner = Owner(host);
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

// --- ПУСТОЕ СОСТОЯНИЕ: КЛИПА НЕТ --------------------------------------------
//
// Панель открывается БЕЗ клипа и прямо говорит, что анимации пока нет. Раньше
// здесь всегда лежал безымянный клип «из воздуха»: выглядело это как готовая к
// работе панель, дорожки в неё добавлялись, ключи ставились — а в проекте не
// появлялось ни файла, ни компонента, и всё это пропадало вместе с окном.
void AnimationPanel::DrawNoClip(EditorHost& host) {
    GameObject sel = host.SelectedObject();

    EditorIcons::Inline("anim");
    ImGui::SameLine(0.0f, EditorIcons::TextGap());
    ImGui::TextUnformatted(T("No clip open"));
    ImGui::Separator();
    ImGui::TextWrapped("%s", T("An animation lives in a clip file and is played by a component\n"
                               "on the object. Create one and both appear at once."));
    ImGui::Dummy(ImVec2(0.0f, 6.0f));

    ImGui::BeginDisabled(!sel.Valid());
    if (EditorIcons::Button("plus", T("Create a clip for the object"),
                            T("Writes a .sageclip into assets and puts the component\n"
                              "that plays it on the selected object"))) {
        CreateClipFor(host, sel);
    }
    ImGui::EndDisabled();
    if (!sel.Valid()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", T("Select an object first."));
    } else {
        ImGui::SameLine();
        ImGui::TextDisabled("%s %s", T("Object:"), sel.Name().c_str());
    }

    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    ImGui::TextDisabled("%s", T("An existing clip opens from Assets: double-click the .sageclip."));
    if (!m_status.empty()) ImGui::TextDisabled("%s", m_status.c_str());
}

// --- ПОЛОСА ВЛАДЕЛЬЦА -------------------------------------------------------
//
// Отвечает на вопрос «кого я сейчас анимирую» — тот самый, на который панель
// раньше не отвечала вовсе. Владелец не «выбранный объект»: его видно, он не
// меняется от щелчка в иерархии, и если его нет — правки выключены, а не
// применяются неизвестно к кому.
void AnimationPanel::DrawOwnerBar(EditorHost& host) {
    GameObject owner = Owner(host);
    if (owner.Valid()) {
        EditorIcons::Inline("cube");
        ImGui::SameLine(0.0f, EditorIcons::TextGap());
        ImGui::TextDisabled("%s", T("Plays on:"));
        ImGui::SameLine();
        ImGui::TextUnformatted(owner.Name().c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", T("The component on this object plays the clip.\n"
                                      "Tracks address its children by name."));
        return;
    }

    // Клип открыт, а играть его некому: так бывает у только что открытого
    // файла. Скрывать это нельзя — иначе снова получится «анимация есть, а в
    // сцене ничего нет».
    ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(EditorTheme::Role::Warn));
    EditorIcons::Inline("warn");
    ImGui::SameLine(0.0f, EditorIcons::TextGap());
    ImGui::TextUnformatted(T("Not on any object — read only"));
    ImGui::PopStyleColor();
    ImGui::SameLine();
    GameObject sel = host.SelectedObject();
    ImGui::BeginDisabled(!sel.Valid() || m_clip.Imported);
    if (ImGui::Button(T("Put on the selected object"))) BindTo(host, sel);
    ImGui::EndDisabled();
    if (!sel.Valid()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", T("Select an object first."));
    }
}

void AnimationPanel::DrawToolbar(EditorHost& host) {
    const bool editable = Editable();

    DrawOwnerBar(host);

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
        } else if (ImGui::Button(T("Make it mine"))) {
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
    ImGui::BeginDisabled(!editable);
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
    ImGui::EndDisabled();
    ImGui::SameLine();
    // «ЕЩЁ ОДИН КЛИП» — ЭТО СНОВА ФАЙЛ И КОМПОНЕНТ, а не чистый лист в памяти.
    // Кнопка, заводившая безымянный клип ниоткуда, и была главным источником
    // «анимации из воздуха».
    ImGui::BeginDisabled(!host.SelectedObject().Valid());
    if (EditorIcons::IconOnlyButton("file", T("New clip for the selected object")))
        CreateClipFor(host, host.SelectedObject());
    ImGui::EndDisabled();

    if (m_dirty) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", T("(unsaved)"));
    }
}

void AnimationPanel::DrawAddTrackPopup(EditorHost& host) {
    if (Sage::UI::MenuScope addMenu; ImGui::BeginPopup("AddTrack###AddTrack")) {
        Scene& scene = host.CurrentScene();
        entt::registry& reg = scene.Registry();
        GameObject owner = Owner(host);
        if (!owner.Valid()) {
            ImGui::TextDisabled("%s", T("The clip is not on any object yet."));
            ImGui::EndPopup();
            return;
        }
        // ДОРОЖКУ СТАВИМ НА ВЫБРАННЫЙ ОБЪЕКТ, НО ТОЛЬКО ЕСЛИ ОН ПОД ВЛАДЕЛЬЦЕМ.
        // Так анимируются дети («Bar/Fill»), ради которых путь в дорожке и
        // заведён. Объект со стороны брать нельзя: клип, ссылающийся наружу,
        // перестанет работать на втором экземпляре — и сломается молча.
        GameObject target = host.SelectedObject();
        std::string path;
        if (target.Valid() && target.Entity() != owner.Entity()) {
            path = PathFromOwner(scene, owner.Entity(), target.Entity());
            if (path.empty()) {
                ImGui::TextDisabled("%s", T("Object:"));
                ImGui::SameLine();
                ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Warn), "%s",
                                   target.Name().c_str());
                ImGui::TextWrapped("%s", T("It is outside the clip owner. Pick the owner itself\n"
                                           "or one of its children."));
                ImGui::EndPopup();
                return;
            }
        } else {
            target = owner;
        }
        // Предлагается ТО, ЧТО У ЭТОГО ОБЪЕКТА ЕСТЬ. Список всех свойств движка
        // означал бы полсотни строк, из которых работают три.
        ImGui::TextDisabled("%s %s", T("Object:"), target.Name().c_str());
        ImGui::Separator();
        const std::vector<const anim::PropertyType*> props =
            anim::PropertiesFor(reg, target.Entity());
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
                if (t.Property == p->Id && t.Target == path) { already = true; break; }
            ImGui::BeginDisabled(already);
            if (ImGui::MenuItem(T(p->Title.c_str()))) {
                PushUndo();
                anim::Track t;
                t.Target = path;
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


void AnimationPanel::Draw(EditorHost& host, bool* open, const std::string& windowId) {
    if (!open || !*open) return;
    if (m_focusFrames > 0) {
        ImGui::SetNextWindowFocus();
        --m_focusFrames;
    }
    ImGui::SetNextWindowSize(ImVec2(960.0f, 320.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(sage::editor::panelid::Title("anim", T("Animation"), windowId).c_str(), open,
                     panelwindows::WindowFlags("Animation"))) {
        ImGui::End();
        return;
    }

    // DELETE ЗДЕСЬ — НАШ, И ОБЩИЙ ОБРАБОТЧИК ЕГО НЕ УВИДИТ.
    //
    // Заявка подаётся, пока панель в фокусе, — ВСЕГДА, а не только когда
    // выбраны ключи и открыт клип. «Клип не открыт» и «ключ не выбран» — не
    // повод удалить из сцены объект, над анимацией которого человек работает:
    // ровно так оно и было — Delete в линейке времени уносил сам объект.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
        sage::editor::hotkeys::Claim(sage::editor::hotkeys::Key::Delete);

    // КЛИПА НЕТ — И ПАНЕЛЬ ЭТО ГОВОРИТ. Ни линейки, ни дорожек: показывать
    // пустую линейку значит предлагать анимировать то, чего не существует.
    if (!m_hasClip) {
        DrawNoClip(host);
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
