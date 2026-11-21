#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "sage/anim/Animator.h"
#include "sage/anim/ClipFile.h"

// ---------------------------------------------------------------------------
// SAGE Animation Graph — поза персонажа, собранная из узлов.
//
// Граф ничего не проигрывает сам: он СТРОИТ ЛОКАЛЬНУЮ ПОЗУ и отдаёт её
// существующему Animator (Animator::SetExternalPose). Корневое движение, ручные
// переопределения костей, IK и скиннинг остаются там, где были, и работают
// поверх графа без изменений.
//
// Узлы — инструменты, а не готовые решения: клип, смешивание двух поз по
// числу, число из параметра, выход. Что именно смешивать и чем управлять
// весом, решает игра.
//
// Ресурс (AnimGraph) неизменяем во время игры и делится между всеми, кто его
// играет; состояние (время клипов, значения параметров) — у каждого объекта
// своё (AnimGraphInstance). Клипы адресуют кости ИМЕНАМИ и привязываются к
// скелету при Bind, поэтому один граф играет на любой модели с совместимыми
// именами костей.
// ---------------------------------------------------------------------------
namespace sage::anim {

// Порядок значений сохраняется в файл строкой, а не числом — новые виды можно
// дописывать куда угодно.
enum class AnimNodeKind { Output, Clip, Blend, Float };

// Что течёт по проводу.
enum class AnimPinType { None, Pose, Float };

struct AnimGraphNode {
    int Id = 0;
    AnimNodeKind Kind = AnimNodeKind::Clip;
    glm::vec2 Pos{0.0f};   // место на холсте редактора (единицы холста)

    // Clip: файл клипа (.sageanim) или, если файла нет, клип самой модели по
    // имени — так граф работает и с моделью, клипы которой ещё не вынуты.
    std::string ClipPath;
    std::string ClipName;
    bool Loop = true;
    float Speed = 1.0f;

    // Blend: вес, когда вход Weight не подключён.
    float Weight = 0.5f;

    // Float: имя параметра графа.
    std::string Param;
};

// Провод: выход узла From -> вход Pin узла To. У входа не больше одного
// провода, у выхода — сколько угодно.
struct AnimGraphLink {
    int From = 0;
    int To = 0;
    int Pin = 0;
};

// Параметр — именованное число, которым игра управляет графом. Значение по
// умолчанию — то, с чего начинает каждый экземпляр.
struct AnimGraphParam {
    std::string Name;
    float Default = 0.0f;
    float Min = 0.0f;
    float Max = 1.0f;
};

// Входы смешивания.
constexpr int kBlendPinA = 0;
constexpr int kBlendPinB = 1;
constexpr int kBlendPinWeight = 2;

struct AnimGraph {
    std::vector<AnimGraphNode> Nodes;
    std::vector<AnimGraphLink> Links;
    std::vector<AnimGraphParam> Params;
    int NextId = 1;
    // Модель для предпросмотра в редакторе. Подсказка, не привязка: играет
    // граф на той модели, что стоит в Mesh объекта.
    std::string PreviewModel;

    // --- Описание узлов ---------------------------------------------------
    static int InputCount(AnimNodeKind kind);
    static AnimPinType InputType(AnimNodeKind kind, int pin);
    static AnimPinType OutputType(AnimNodeKind kind);
    static const char* KindName(AnimNodeKind kind);   // имя в файле: "clip", "blend"…

    // --- Правка -----------------------------------------------------------
    AnimGraphNode* Find(int id);
    const AnimGraphNode* Find(int id) const;
    // Новый узел; возвращает его Id. Второй Output не создаётся (вернёт 0):
    // выход у графа один, иначе неясно, какую позу он отдаёт.
    int Add(AnimNodeKind kind, glm::vec2 pos = glm::vec2(0.0f));
    // Удаляет узел и все его провода. Выход не удаляется.
    bool Remove(int id);
    // Можно ли провести провод; why — причина отказа для человека.
    bool CanLink(int from, int to, int pin, std::string* why = nullptr) const;
    // Проводит провод, заменяя прежний на этом входе.
    bool Connect(int from, int to, int pin);
    void Disconnect(int to, int pin);
    const AnimGraphLink* LinkInto(int to, int pin) const;
    int OutputNode() const;   // 0 — выхода нет

    AnimGraphParam* FindParam(const std::string& name);
    const AnimGraphParam* FindParam(const std::string& name) const;
};

// Новый граф: только Output. Это контракт ресурса — граф без выхода не
// отдаёт позу, и редактор не предлагает собирать его заново руками.
AnimGraph NewAnimGraph();

// --- Файл (.sageanimgraph, JSON, ключ версии "sage_animgraph_version") -------
std::string AnimGraphToJson(const AnimGraph& graph);
// Бросает std::runtime_error на непонятном тексте или версии новее движка.
AnimGraph AnimGraphFromJson(const std::string& text);
// Бросают std::runtime_error, если файл не пишется/не читается.
void SaveAnimGraph(const AnimGraph& graph, const std::string& path);
AnimGraph LoadAnimGraph(const std::string& path);

// Счётчик сохранений графов в этом процессе. Объект, играющий граф, сверяет
// его и перечитывает файл: граф, сохранённый в редакторе, сразу доезжает до
// сцены, без перезапуска.
uint64_t AnimGraphRevision();

// ---------------------------------------------------------------------------
// Экземпляр графа: время клипов и значения параметров ОДНОГО объекта.
// Копируется как значение: копия объекта получает своё независимое состояние.
// ---------------------------------------------------------------------------
class AnimGraphInstance {
public:
    // Читает клип из файла. Свой у движка (через AssetDatabase), свой у
    // тестов (без диска).
    using ClipLoader = std::function<bool(const std::string& path, ClipAsset& out, std::string& err)>;

    // Привязать граф к скелету. modelClips — клипы модели (для узлов, где
    // клип выбран по имени); указатели должны жить, пока жив экземпляр.
    // Время клипов и значения параметров с прежнего графа сохраняются по Id
    // узлов и именам — правка графа в редакторе не перезапускает анимацию.
    void Bind(std::shared_ptr<const AnimGraph> graph, const Skeleton* skeleton,
              const std::vector<AnimationClip>* modelClips, const ClipLoader& loader);
    void Unbind();
    bool Bound() const { return m_graph && m_skeleton; }
    const AnimGraph* Graph() const { return m_graph.get(); }
    const Skeleton* BoundSkeleton() const { return m_skeleton; }

    // Параметры. Неизвестное графу имя тоже хранится: игра может выставить
    // значение раньше, чем граф загрузится.
    void SetFloat(const std::string& name, float value) { m_floats[name] = value; }
    float GetFloat(const std::string& name) const;

    // Продвигает время всех клипов на dt и пересчитывает позу.
    void Update(float dt);
    // Время всех клипов — в начало.
    void Restart();

    const LocalPose& Pose() const { return m_pose; }
    float ClipTime(int nodeId) const;
    // Вес смешивания узла Blend в последнем Update (для подсказки в редакторе).
    float BlendWeight(int nodeId) const;

    // Что не получилось при привязке: клип не прочитан, не найден в модели.
    const std::vector<std::string>& Problems() const { return m_problems; }
    // Сколько каналов клипов не нашли своих костей в скелете.
    int MissingBones() const { return m_missingBones; }

private:
    struct ClipState {
        int Node = 0;
        int Own = -1;     // индекс в m_ownClips (клип из файла)
        int Model = -1;   // индекс в *m_modelClips (клип модели по имени)
        float Time = 0.0f;
    };

    const AnimationClip* ClipOf(const ClipState& c) const;
    float FloatInput(const AnimGraphNode& node, int pin) const;
    void Eval(int nodeId, LocalPose& out, int depth);

    std::shared_ptr<const AnimGraph> m_graph;
    const Skeleton* m_skeleton = nullptr;
    const std::vector<AnimationClip>* m_modelClips = nullptr;
    std::vector<AnimationClip> m_ownClips;
    std::vector<ClipState> m_clips;
    std::unordered_map<int, int> m_clipOfNode;      // Id узла -> индекс в m_clips
    std::unordered_map<std::string, float> m_floats;
    std::unordered_map<int, float> m_blendWeights;
    std::vector<LocalPose> m_scratch;               // по глубине рекурсии
    LocalPose m_pose;
    std::vector<std::string> m_problems;
    int m_missingBones = 0;
};

} // namespace sage::anim
