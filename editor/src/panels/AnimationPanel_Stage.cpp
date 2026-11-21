// Таймлайн в пространстве Animation: клип показывается и снимается с КОПИИ
// цели в сцене предпросмотра (animws/AnimStage.h), скелетный клип правится
// ключами из позы кости. См. AnimationPanel.h, раздел «ПРОСТРАНСТВО ANIMATION».
#include "AnimationPanelDetail.h"

#include "../animws/AnimStage.h"
#include "sage/anim/AnimationComponents.h"
#include "sage/assets/AssetDatabase.h"

namespace {

// Клип таймлайна (дорожки «кость · канал») -> скелетный клип файла.
anim::ClipAsset ToSkeletal(const anim::PropertyClip& clip) {
    anim::ClipAsset out;
    out.Name = clip.Name;
    out.Duration = clip.Duration;
    out.Editable = !clip.Imported;
    for (const anim::Track& t : clip.Tracks) {
        if (!IsBoneChannel(t.Property) || t.Keys.empty()) continue;
        anim::ClipAsset::Channel ch;
        ch.Joint = t.Target;
        ch.Target = t.Property == "joint.translation" ? anim::AnimPath::Translation
                    : t.Property == "joint.rotation"  ? anim::AnimPath::Rotation
                                                      : anim::AnimPath::Scale;
        // Ступенька — если все ключи ступенькой; иначе линейно (поворот —
        // сферически): других видов у канала кости нет.
        bool step = true;
        std::vector<anim::Key> keys = t.Keys;
        std::sort(keys.begin(), keys.end(), [](const anim::Key& a, const anim::Key& b) { return a.Time < b.Time; });
        for (const anim::Key& k : keys) {
            step = step && k.Out == anim::Interp::Constant;
            ch.Times.push_back(k.Time);
            ch.Values.push_back(k.Value);
        }
        ch.Interp = step ? anim::AnimInterp::Step : anim::AnimInterp::Linear;
        out.Channels.push_back(std::move(ch));
    }
    return out;
}

} // namespace

bool AnimationPanel::StageMode(EditorHost& host) const {
    return host.Workspace() == EditorWorkspace::Animation && host.AnimationStage() != nullptr;
}

bool AnimationPanel::PoseContext(EditorHost& host, Scene*& scene, entt::entity& owner) {
    GameObject real = Owner(host);
    if (!real.Valid()) return false;
    if (!StageMode(host)) {
        scene = &host.CurrentScene();
        owner = real.Entity();
        return true;
    }
    // Клип свойств играет на копии своего владельца: цель пространства —
    // владелец клипа.
    AnimStage& stage = *host.AnimationStage();
    if (stage.TargetId() != real.Id()) stage.SetTarget(host, real.Id());
    GameObject root = stage.StageRoot();
    if (!root.Valid()) return false;
    scene = &stage.StageScene();
    owner = root.Entity();
    return true;
}

void AnimationPanel::PushToStage(EditorHost& host) {
    if (!m_skeletal || !StageMode(host)) return;
    AnimStage& stage = *host.AnimationStage();
    stage.ShowClip(m_path, ToSkeletal(m_clip), m_rev);
    stage.SetTime(m_time);
    stage.SetPlaying(m_playing);
}

void AnimationPanel::KeySelectedBone(EditorHost& host, bool onlyEdited) {
    // Не Editable(): тот смотрит на скелет, отмеченный при отрисовке, а ключ
    // ставят и без неё (горячая клавиша, авто-ключ, самопроверка).
    if (!m_skeletal || !m_hasClip || m_clip.Imported || !StageMode(host)) return;
    AnimStage& stage = *host.AnimationStage();
    const anim::Skeleton* sk = stage.Skeleton();
    const int bone = stage.SelectedBone();
    if (!sk || bone < 0 || bone >= sk->Count()) return;
    glm::vec3 t, s;
    glm::quat r;
    if (!stage.BoneLocal(bone, t, r, s)) return;
    // Какие каналы: поправленные гизмо (авто-ключ) или все три.
    bool want[3] = {true, true, true};
    if (onlyEdited) {
        const AnimationComponent* am = stage.StageAnim();
        const bool has = am && bone < (int)am->PoseOverrides.size();
        want[0] = has && am->PoseOverrides[(size_t)bone].HasTranslation;
        want[1] = has && am->PoseOverrides[(size_t)bone].HasRotation;
        want[2] = has && am->PoseOverrides[(size_t)bone].HasScale;
        if (!want[0] && !want[1] && !want[2]) return;
    }
    static const char* kProps[3] = {"joint.translation", "joint.rotation", "joint.scale"};
    const std::string& name = sk->Joints[(size_t)bone].Name;
    PushUndo();
    for (int c = 0; c < 3; ++c) {
        if (!want[c]) continue;
        int found = -1;
        for (int i = 0; i < (int)m_clip.Tracks.size(); ++i)
            if (m_clip.Tracks[(size_t)i].Target == name && m_clip.Tracks[(size_t)i].Property == kProps[c]) found = i;
        if (found < 0) {
            anim::Track tr;
            tr.Target = name;
            tr.Property = kProps[c];
            m_clip.Tracks.push_back(tr);
            found = (int)m_clip.Tracks.size() - 1;
        }
        anim::SetKey(m_clip.Tracks[(size_t)found], m_time, BoneValue(kProps[c], t, r, s));
        m_activeTrack = found;
    }
    if (m_time > m_clip.Duration) m_clip.Duration = m_time;
    // Поза теперь в клипе — ручная правка кости больше не нужна.
    stage.ClearBonePose(bone);
}

bool AnimationPanel::SaveCurrent(EditorHost& host) {
    if (m_skeletal) {
        if (m_clip.Imported || m_path.empty()) return false;
        try {
            anim::SaveClip(ToSkeletal(m_clip), m_path);
            m_dirty = false;
            m_status = std::string(T("Saved: ")) + m_path;
            // Объекты уровня с этим клипом перечитают его: путь тот же, а
            // данные новые.
            for (auto [e, am] : host.LevelScene().Registry().view<AnimationComponent>().each()) {
                (void)e;
                if (!am.ClipResolvedFrom.empty() &&
                    sage::AssetDatabase::Instance().LocatePath(am.ClipPath) == m_path)
                    am.ClipResolvedFrom.clear();
            }
            return true;
        } catch (const std::exception& e) {
            m_status = e.what();
            return false;
        }
    }
    if (m_path.empty()) {
        // Имя файла — из имени клипа: спрашивать его отдельным диалогом ровно в
        // тот момент, когда клип уже назван, значит спрашивать дважды об одном.
        const std::string stem = m_clip.Name.empty() ? std::string("clip") : m_clip.Name;
        m_path = (host.CurrentProject().AssetsDir() / (stem + ".sageclip")).string();
    }
    std::string err;
    if (anim::SaveClipFile(m_clip, m_path, err)) {
        m_dirty = false;
        m_status = std::string(T("Saved: ")) + m_path;
        return true;
    }
    m_status = err;
    return false;
}

void AnimationPanel::MakeOwnSkeletal(EditorHost& host) {
    // Свой файл рядом со своими клипами — не рядом с моделью: исходный клип
    // пересоздаётся переимпортом, и копия рядом с ним выглядела бы его частью.
    const fs::path dir = host.CurrentProject().AssetsDir() / "animations";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string base = m_clip.Name.empty() ? std::string("clip") : m_clip.Name;
    fs::path file = dir / anim::ClipFileName(base, "custom");
    for (int n = 2; fs::exists(file, ec) && n < 1000; ++n)
        file = dir / anim::ClipFileName(base, "custom " + std::to_string(n));
    m_clip.Imported = false;
    m_path = file.string();
    if (!SaveCurrent(host)) return;
    sage::AssetDatabase::Instance().Register(m_path, "animation");
    m_undo.clear();
    m_redo.clear();
    Bump();
    m_status = std::string(T("Editable copy: ")) + host.CurrentProject().AssetRef(file);
}

void AnimationPanel::UpdateRows(EditorHost& host) {
    m_rows.clear();
    std::string bone;
    if (m_skeletal && m_selectedBoneOnly && StageMode(host)) {
        AnimStage& stage = *host.AnimationStage();
        const anim::Skeleton* sk = stage.Skeleton();
        if (sk && stage.SelectedBone() >= 0 && stage.SelectedBone() < sk->Count())
            bone = sk->Joints[(size_t)stage.SelectedBone()].Name;
    }
    for (int i = 0; i < (int)m_clip.Tracks.size(); ++i)
        if (bone.empty() || m_clip.Tracks[(size_t)i].Target == bone) m_rows.push_back(i);
    if (!m_skeletal) return;
    // Дорожки скелета — по костям в порядке скелета (родитель раньше детей),
    // внутри кости — Position, Rotation, Scale: у сложного скелета иначе
    // каналы одной кости разбросаны по списку.
    const anim::Skeleton* sk = StageMode(host) ? host.AnimationStage()->Skeleton() : nullptr;
    auto boneRank = [sk](const std::string& name) {
        if (sk)
            for (int j = 0; j < sk->Count(); ++j)
                if (sk->Joints[(size_t)j].Name == name) return j;
        return 1 << 20;
    };
    auto channelRank = [](const std::string& p) { return p == "joint.translation" ? 0 : p == "joint.rotation" ? 1 : 2; };
    std::stable_sort(m_rows.begin(), m_rows.end(), [&](int a, int b) {
        const anim::Track& ta = m_clip.Tracks[(size_t)a];
        const anim::Track& tb = m_clip.Tracks[(size_t)b];
        if (ta.Target != tb.Target) {
            const int ra = boneRank(ta.Target), rb = boneRank(tb.Target);
            return ra != rb ? ra < rb : ta.Target < tb.Target;
        }
        return channelRank(ta.Property) < channelRank(tb.Property);
    });
}
