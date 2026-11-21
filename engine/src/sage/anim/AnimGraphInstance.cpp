#include "sage/anim/AnimGraph.h"

#include <algorithm>
#include <cmath>

namespace sage::anim {

namespace {
// Предел вложенности: петлю граф не пропускает (CanLink), но рекурсия по
// чужому файлу не должна зависеть только от этого.
constexpr int kMaxDepth = 64;

float AdvanceTime(const AnimationClip* clip, float time, float dt, bool loop) {
    time += dt;
    if (!clip || clip->Duration <= 0.0f) return 0.0f;
    if (loop) {
        time = std::fmod(time, clip->Duration);
        if (time < 0.0f) time += clip->Duration;
        return time;
    }
    return std::clamp(time, 0.0f, clip->Duration);
}
} // namespace

void AnimGraphInstance::Bind(std::shared_ptr<const AnimGraph> graph, const Skeleton* skeleton,
                             const std::vector<AnimationClip>* modelClips, const ClipLoader& loader) {
    // Время клипов переносится по Id узлов: правка графа в редакторе (новый
    // провод, другой вес) не должна отбрасывать анимацию в начало.
    std::unordered_map<int, float> oldTimes;
    for (const ClipState& c : m_clips) oldTimes[c.Node] = c.Time;

    m_graph = std::move(graph);
    m_skeleton = skeleton;
    m_modelClips = modelClips;
    m_ownClips.clear();
    m_clips.clear();
    m_clipOfNode.clear();
    m_blendWeights.clear();
    m_problems.clear();
    m_missingBones = 0;
    if (!m_graph || !m_skeleton) return;
    // Черновики — сразу на всю глубину: рост вектора во время рекурсии сделал
    // бы недействительной ссылку на черновик уровнем выше.
    m_scratch.resize(kMaxDepth);

    // Файл клипа, на который ссылаются два узла, читается и привязывается один раз.
    std::unordered_map<std::string, int> ownByPath;
    for (const AnimGraphNode& n : m_graph->Nodes) {
        if (n.Kind != AnimNodeKind::Clip) continue;
        ClipState st;
        st.Node = n.Id;
        if (auto it = oldTimes.find(n.Id); it != oldTimes.end()) st.Time = it->second;
        if (!n.ClipPath.empty()) {
            auto found = ownByPath.find(n.ClipPath);
            if (found != ownByPath.end()) {
                st.Own = found->second;
            } else {
                ClipAsset asset;
                std::string err;
                if (loader && loader(n.ClipPath, asset, err)) {
                    int missing = 0;
                    m_ownClips.push_back(sage::anim::Bind(asset, *m_skeleton, &missing));
                    m_missingBones += missing;
                    st.Own = (int)m_ownClips.size() - 1;
                    ownByPath[n.ClipPath] = st.Own;
                } else {
                    m_problems.push_back("Клип '" + n.ClipPath + "' не прочитан" +
                                         (err.empty() ? std::string() : ": " + err));
                }
            }
        } else if (!n.ClipName.empty()) {
            if (m_modelClips) {
                for (int i = 0; i < (int)m_modelClips->size(); ++i)
                    if ((*m_modelClips)[i].Name == n.ClipName) { st.Model = i; break; }
            }
            if (st.Model < 0) m_problems.push_back("В модели нет клипа '" + n.ClipName + "'");
        }
        m_clipOfNode[n.Id] = (int)m_clips.size();
        m_clips.push_back(st);
    }
    // Поза сразу, без ожидания Update: только что привязанный граф не должен
    // показывать кадр позы покоя.
    Update(0.0f);
}

void AnimGraphInstance::Unbind() {
    m_graph.reset();
    m_skeleton = nullptr;
    m_modelClips = nullptr;
    m_ownClips.clear();
    m_clips.clear();
    m_clipOfNode.clear();
    m_blendWeights.clear();
    m_problems.clear();
    m_missingBones = 0;
    m_pose = LocalPose();
}

float AnimGraphInstance::GetFloat(const std::string& name) const {
    if (auto it = m_floats.find(name); it != m_floats.end()) return it->second;
    if (m_graph)
        if (const AnimGraphParam* p = m_graph->FindParam(name)) return p->Default;
    return 0.0f;
}

const AnimationClip* AnimGraphInstance::ClipOf(const ClipState& c) const {
    if (c.Own >= 0 && c.Own < (int)m_ownClips.size()) return &m_ownClips[c.Own];
    if (c.Model >= 0 && m_modelClips && c.Model < (int)m_modelClips->size()) return &(*m_modelClips)[c.Model];
    return nullptr;
}

float AnimGraphInstance::ClipTime(int nodeId) const {
    auto it = m_clipOfNode.find(nodeId);
    return it == m_clipOfNode.end() ? 0.0f : m_clips[it->second].Time;
}

float AnimGraphInstance::BlendWeight(int nodeId) const {
    auto it = m_blendWeights.find(nodeId);
    return it == m_blendWeights.end() ? 0.0f : it->second;
}

void AnimGraphInstance::Restart() {
    for (ClipState& c : m_clips) c.Time = 0.0f;
    Update(0.0f);
}

void AnimGraphInstance::Update(float dt) {
    if (!Bound()) return;
    // Время идёт у ВСЕХ клипов, а не только у тех, чей вес сейчас не ноль:
    // иначе клип, вернувшийся в смесь, начинал бы с того кадра, где его
    // выключили, и шаги рассинхронизировались бы с весом.
    for (ClipState& c : m_clips) {
        const AnimGraphNode* n = m_graph->Find(c.Node);
        if (!n) continue;
        c.Time = AdvanceTime(ClipOf(c), c.Time, dt * n->Speed, n->Loop);
    }
    const int out = m_graph->OutputNode();
    const AnimGraphLink* l = out ? m_graph->LinkInto(out, 0) : nullptr;
    if (l) Eval(l->From, m_pose, 0);
    else SampleClipPose(*m_skeleton, nullptr, 0.0f, m_pose);   // выход не подключён — поза покоя
}

float AnimGraphInstance::FloatInput(const AnimGraphNode& node, int pin) const {
    const AnimGraphLink* l = m_graph->LinkInto(node.Id, pin);
    if (!l) return node.Weight;
    const AnimGraphNode* src = m_graph->Find(l->From);
    if (!src || src->Kind != AnimNodeKind::Float) return node.Weight;
    return GetFloat(src->Param);
}

void AnimGraphInstance::Eval(int nodeId, LocalPose& out, int depth) {
    const AnimGraphNode* n = m_graph->Find(nodeId);
    if (!n || depth >= kMaxDepth) {
        SampleClipPose(*m_skeleton, nullptr, 0.0f, out);
        return;
    }
    switch (n->Kind) {
        case AnimNodeKind::Clip: {
            auto it = m_clipOfNode.find(nodeId);
            const ClipState* st = it == m_clipOfNode.end() ? nullptr : &m_clips[it->second];
            SampleClipPose(*m_skeleton, st ? ClipOf(*st) : nullptr, st ? st->Time : 0.0f, out);
            return;
        }
        case AnimNodeKind::Blend: {
            const float w = std::clamp(FloatInput(*n, kBlendPinWeight), 0.0f, 1.0f);
            m_blendWeights[nodeId] = w;
            const AnimGraphLink* a = m_graph->LinkInto(nodeId, kBlendPinA);
            const AnimGraphLink* b = m_graph->LinkInto(nodeId, kBlendPinB);
            // Неподключённый вход — поза покоя: смешивание с ней честно видно
            // («персонаж наполовину встаёт в T-позу»), а не прячется.
            auto evalOrRest = [&](const AnimGraphLink* link, LocalPose& into) {
                if (link) Eval(link->From, into, depth + 1);
                else SampleClipPose(*m_skeleton, nullptr, 0.0f, into);
            };
            // Крайние веса считают только одну ветвь: вторая ничего бы не дала.
            if (w <= 0.0f) { evalOrRest(a, out); return; }
            if (w >= 1.0f) { evalOrRest(b, out); return; }
            evalOrRest(a, out);
            // Ветвь B пишет в черновик СВОЕЙ глубины: вложенные вычисления
            // берут черновики глубже и его не затирают.
            LocalPose& tmp = m_scratch[depth];
            evalOrRest(b, tmp);
            BlendPoses(out, tmp, w, out);
            return;
        }
        default:
            // Выход и число позы не дают.
            SampleClipPose(*m_skeleton, nullptr, 0.0f, out);
            return;
    }
}

} // namespace sage::anim
