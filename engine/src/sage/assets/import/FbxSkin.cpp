#include "sage/assets/import/FbxSkin.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <functional>
#include <unordered_set>
#include <vector>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <stb_image.h>

#include "sage/assets/import/FbxTree.h"
#include "sage/core/Log.h"
#include "sage/render/ModelData.h"

namespace fs = std::filesystem;

namespace sage::assets {
namespace {

using fbx::Doubles;
using fbx::Ints;
using fbx::Node;
using fbx::Property70;
using fbx::Property70Vec;

// Время в FBX — целое в «ktime»: 46186158000 единиц в секунде. Число не
// круглое намеренно: оно делится на все стандартные частоты кадров (24, 25, 30,
// 60, 120…), поэтому кадр никогда не попадает между двумя значениями.
constexpr double kFbxTimeUnit = 46186158000.0;

// Имя объекта FBX: в свойстве лежит «Имя\0\x01Тип», нас интересует часть до
// нуля. Без обрезки имена костей не совпадут с именами в клипах, и клип от
// другого экспорта не ляжет на скелет.
std::string ObjectName(const Node& n) {
    std::string name = n.Props.size() > 1 ? n.Props[1].Text : std::string();
    const size_t sep = name.find('\0');
    return sep == std::string::npos ? name : name.substr(0, sep);
}

int64_t Uid(const Node& n) { return n.Props.empty() ? 0 : (int64_t)n.Props[0].Number; }

// Матрица 4x4 из записи вида Transform/TransformLink (16 чисел по столбцам).
glm::mat4 MatrixFrom(const Node* n) {
    const std::vector<double>* v = Doubles(n);
    if (!v || v->size() < 16) return glm::mat4(1.0f);
    glm::mat4 m(1.0f);
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) m[c][r] = (float)(*v)[(size_t)(c * 4 + r)];
    return m;
}

// --- Локальный трансформ узла FBX -------------------------------------------
//
// Это НЕ просто «перенос-поворот-масштаб»: между ними стоят предповорот,
// постповорот и опорные точки, и пропустить их нельзя. Mixamo и Maya кладут
// разворот кости именно в PreRotation, и модель, собранная без него, выходит
// вывернутой — руки назад, ноги в стороны, — хотя все числа «прочитаны верно».
//
// Порядок из спецификации FBX:
//   Local = T * Roff * Rp * Rpre * R * Rpost⁻¹ * Rp⁻¹ * Soff * Sp * S * Sp⁻¹
struct NodeTransform {
    glm::vec3 Translation{0.0f};
    glm::vec3 Rotation{0.0f};      // градусы, порядок XYZ
    glm::vec3 Scale{1.0f};
    glm::vec3 PreRotation{0.0f};
    glm::vec3 PostRotation{0.0f};
    glm::vec3 RotationOffset{0.0f};
    glm::vec3 RotationPivot{0.0f};
    glm::vec3 ScalingOffset{0.0f};
    glm::vec3 ScalingPivot{0.0f};

    glm::mat4 Matrix() const {
        auto euler = [](const glm::vec3& deg) {
            // Порядок поворота FBX по умолчанию — XYZ, то есть Rz * Ry * Rx.
            return glm::eulerAngleZYX(glm::radians(deg.z), glm::radians(deg.y),
                                      glm::radians(deg.x));
        };
        const glm::mat4 T = glm::translate(glm::mat4(1.0f), Translation);
        const glm::mat4 Roff = glm::translate(glm::mat4(1.0f), RotationOffset);
        const glm::mat4 Rp = glm::translate(glm::mat4(1.0f), RotationPivot);
        const glm::mat4 RpInv = glm::translate(glm::mat4(1.0f), -RotationPivot);
        const glm::mat4 Soff = glm::translate(glm::mat4(1.0f), ScalingOffset);
        const glm::mat4 Sp = glm::translate(glm::mat4(1.0f), ScalingPivot);
        const glm::mat4 SpInv = glm::translate(glm::mat4(1.0f), -ScalingPivot);
        const glm::mat4 S = glm::scale(glm::mat4(1.0f), Scale);
        return T * Roff * Rp * euler(PreRotation) * euler(Rotation) *
               glm::inverse(euler(PostRotation)) * RpInv * Soff * Sp * S * SpInv;
    }
};

NodeTransform ReadTransform(const Node& n) {
    NodeTransform t;
    t.Translation = Property70Vec(&n, "Lcl Translation", glm::vec3(0.0f));
    t.Rotation = Property70Vec(&n, "Lcl Rotation", glm::vec3(0.0f));
    t.Scale = Property70Vec(&n, "Lcl Scaling", glm::vec3(1.0f));
    t.PreRotation = Property70Vec(&n, "PreRotation", glm::vec3(0.0f));
    t.PostRotation = Property70Vec(&n, "PostRotation", glm::vec3(0.0f));
    t.RotationOffset = Property70Vec(&n, "RotationOffset", glm::vec3(0.0f));
    t.RotationPivot = Property70Vec(&n, "RotationPivot", glm::vec3(0.0f));
    t.ScalingOffset = Property70Vec(&n, "ScalingOffset", glm::vec3(0.0f));
    t.ScalingPivot = Property70Vec(&n, "ScalingPivot", glm::vec3(0.0f));
    return t;
}

// Разложение матрицы на TRS. Скелет хранит именно TRS (см. anim/Skeleton.h):
// так каналы анимации правят перенос и поворот независимо, а поворот
// интерполируется сферически, а не покомпонентно.
void Decompose(const glm::mat4& m, glm::vec3& t, glm::quat& r, glm::vec3& s) {
    t = glm::vec3(m[3]);
    glm::vec3 axis[3] = {glm::vec3(m[0]), glm::vec3(m[1]), glm::vec3(m[2])};
    s = glm::vec3(glm::length(axis[0]), glm::length(axis[1]), glm::length(axis[2]));
    for (int i = 0; i < 3; ++i) {
        if (s[i] > 1e-8f) axis[i] /= s[i];
        else axis[i] = glm::vec3(i == 0, i == 1, i == 2);
    }
    // Зеркальный трансформ (отрицательный масштаб) ловим по определителю:
    // без этого поворот получился бы «вывернутым наизнанку».
    glm::mat3 rot(axis[0], axis[1], axis[2]);
    if (glm::determinant(rot) < 0.0f) {
        s.x = -s.x;
        rot[0] = -rot[0];
    }
    r = glm::normalize(glm::quat_cast(rot));
}

// --- Связи -------------------------------------------------------------------
struct Connections {
    // РОДИТЕЛЕЙ У ОБЪЕКТА НЕСКОЛЬКО, и это не редкость, а норма формата: кость
    // связана и со своей родительской костью, и с кластером скина, и иногда с
    // позой привязки. Пока здесь лежал ОДИН родитель, побеждала последняя
    // связь в файле — и у кости «родителем» оказывался её собственный кластер.
    // Скелет от этого разваливался: иерархии нет, поворот бедра не двигает
    // ногу, а имена костей берутся у посторонних объектов.
    std::unordered_multimap<int64_t, int64_t> ParentsOf;           // OO: ребёнок -> родители
    std::unordered_multimap<int64_t, int64_t> ChildrenOf;          // OO: родитель -> дети
    // OP: ребёнок -> (родитель, имя свойства). Так кривая привязана к КАНАЛУ,
    // а канал — к свойству узла; без имени свойства поворот неотличим от
    // переноса.
    struct PropLink { int64_t Parent; std::string Property; };
    std::unordered_multimap<int64_t, PropLink> PropertyOf;
};

Connections ReadConnections(const Node& root) {
    Connections c;
    const Node* conns = root.Find("Connections");
    if (!conns) return c;
    for (const Node& n : conns->Children) {
        if (n.Name != "C" || n.Props.size() < 3) continue;
        const int64_t child = (int64_t)n.Props[1].Number;
        const int64_t parent = (int64_t)n.Props[2].Number;
        if (n.Props[0].Text == "OO") {
            c.ParentsOf.emplace(child, parent);
            c.ChildrenOf.emplace(parent, child);
        } else if (n.Props[0].Text == "OP" && n.Props.size() >= 4) {
            c.PropertyOf.emplace(child, Connections::PropLink{parent, n.Props[3].Text});
        }
    }
    return c;
}

// --- Кривая анимации ---------------------------------------------------------
struct Curve {
    std::vector<double> Times;   // секунды
    std::vector<float> Values;

    bool Empty() const { return Times.empty(); }

    float At(double t, float fallback) const {
        if (Times.empty()) return fallback;
        if (t <= Times.front()) return Values.front();
        if (t >= Times.back()) return Values.back();
        for (size_t i = 1; i < Times.size(); ++i) {
            if (t > Times[i]) continue;
            const double span = Times[i] - Times[i - 1];
            const double k = span > 1e-9 ? (t - Times[i - 1]) / span : 0.0;
            return (float)(Values[i - 1] + (Values[i] - Values[i - 1]) * k);
        }
        return Values.back();
    }
};

// Канал узла: три кривые на свойство (d|X, d|Y, d|Z).
struct ChannelCurves {
    Curve X, Y, Z;
    bool Empty() const { return X.Empty() && Y.Empty() && Z.Empty(); }
    void CollectTimes(std::vector<double>& out) const {
        for (const Curve* c : {&X, &Y, &Z})
            out.insert(out.end(), c->Times.begin(), c->Times.end());
    }
    glm::vec3 At(double t, const glm::vec3& fallback) const {
        return glm::vec3(X.At(t, fallback.x), Y.At(t, fallback.y), Z.At(t, fallback.z));
    }
};

struct AnimatedNode {
    ChannelCurves Translation;
    ChannelCurves Rotation;
    ChannelCurves Scale;
    bool Any() const { return !Translation.Empty() || !Rotation.Empty() || !Scale.Empty(); }
};

} // namespace

bool ImportFbxSkinned(const std::string& path, sage::render::ModelData& out, std::string& err) {
    Node root;
    if (!fbx::ReadTree(path, root, err)) return false;
    const fbx::Units units = fbx::ReadUnits(root);
    const glm::mat4 C = units.Matrix();
    const glm::mat4 Cinv = glm::inverse(C);

    const Node* objects = root.Find("Objects");
    if (!objects) {
        err = "в файле FBX нет блока Objects";
        return false;
    }
    const Connections conns = ReadConnections(root);

    // --- Объекты по uid ------------------------------------------------------
    std::unordered_map<int64_t, const Node*> byUid;
    for (const Node& n : objects->Children) {
        if (!n.Props.empty()) byUid[Uid(n)] = &n;
    }

    // Родитель НУЖНОЙ ПОРОДЫ: у объекта их несколько (см. Connections), и
    // спрашивать надо именно тот, ради которого пришли, — кость у кости,
    // модель у геометрии, слой у канала.
    auto parentOfKind = [&](int64_t uid, const char* record) -> int64_t {
        auto range = conns.ParentsOf.equal_range(uid);
        for (auto it = range.first; it != range.second; ++it) {
            auto obj = byUid.find(it->second);
            if (obj != byUid.end() && obj->second->Name == record) return it->second;
        }
        return 0;
    };
    auto anyParent = [&](int64_t uid) -> int64_t {
        auto it = conns.ParentsOf.find(uid);
        return it == conns.ParentsOf.end() ? 0 : it->second;
    };
    (void)anyParent;

    // --- Скины и их кластеры -------------------------------------------------
    //
    // Deformer в FBX двух пород: Skin (привязан к геометрии) и Cluster (одна
    // кость с весами). Порода лежит в ТРЕТЬЕМ свойстве записи — по имени её не
    // угадать, экспортёры называют кластеры как попало.
    struct Cluster {
        const Node* Record = nullptr;
        int64_t Bone = 0;
        glm::mat4 Transform{1.0f};      // меш -> мир на момент привязки
        glm::mat4 TransformLink{1.0f};  // кость -> мир на момент привязки
    };
    std::unordered_map<int64_t, std::vector<Cluster>> clustersOfSkin;
    std::unordered_map<int64_t, int64_t> skinOfGeometry;

    for (const Node& n : objects->Children) {
        if (n.Name != "Deformer" || n.Props.size() < 3) continue;
        const std::string kind = n.Props[2].Text;
        if (kind == "Skin") {
            // Скин связан с геометрией: Skin --OO--> Geometry.
            const int64_t geometry = parentOfKind(Uid(n), "Geometry");
            if (geometry != 0) skinOfGeometry[geometry] = Uid(n);
        } else if (kind == "Cluster") {
            Cluster c;
            c.Record = &n;
            c.Transform = MatrixFrom(n.Find("Transform"));
            c.TransformLink = MatrixFrom(n.Find("TransformLink"));
            // Кость кластера: Model --OO--> Cluster (кость числится РЕБЁНКОМ).
            auto range = conns.ChildrenOf.equal_range(Uid(n));
            for (auto it = range.first; it != range.second; ++it) {
                auto obj = byUid.find(it->second);
                if (obj != byUid.end() && obj->second->Name == "Model") c.Bone = it->second;
            }
            const int64_t skin = parentOfKind(Uid(n), "Deformer");
            if (skin != 0 && c.Bone != 0) clustersOfSkin[skin].push_back(c);
        }
    }

    if (skinOfGeometry.empty() || clustersOfSkin.empty()) {
        err = "в файле FBX нет скина (Deformer::Skin) — это обычная модель без костей";
        return false;
    }

    // --- Скелет --------------------------------------------------------------
    //
    // Кости — это Model, на которые ссылаются кластеры, ПЛЮС все их предки:
    // без предков цепочка рвётся, и поворот бедра не двигает ногу.
    std::unordered_set<int64_t> boneSet;
    for (const auto& [skin, clusters] : clustersOfSkin) {
        for (const Cluster& c : clusters) {
            int64_t cur = c.Bone;
            for (int guard = 0; cur != 0 && guard < 128; ++guard) {
                auto obj = byUid.find(cur);
                if (obj == byUid.end() || obj->second->Name != "Model") break;
                boneSet.insert(cur);
                cur = parentOfKind(cur, "Model");
            }
        }
    }

    // Порядок: РОДИТЕЛЬ РАНЬШЕ РЕБЁНКА. Аниматор считает глобальные матрицы
    // одним проходом по списку, и кость, встреченная раньше своего родителя,
    // получила бы вчерашнюю позу родителя.
    std::vector<int64_t> bones;
    std::unordered_map<int64_t, int> jointOf;
    {
        std::unordered_set<int64_t> placed;
        std::function<void(int64_t)> place = [&](int64_t uid) {
            if (uid == 0 || !boneSet.count(uid) || placed.count(uid)) return;
            const int64_t parent = parentOfKind(uid, "Model");
            if (parent != 0 && boneSet.count(parent)) place(parent);
            placed.insert(uid);
            jointOf[uid] = (int)bones.size();
            bones.push_back(uid);
        };
        for (int64_t uid : boneSet) place(uid);
    }

    if (bones.empty()) {
        err = "в скине FBX не нашлось костей";
        return false;
    }
    if ((int)bones.size() > sage::anim::kMaxBones) {
        LOG_WARN("Anim") << "FBX: костей " << bones.size() << ", в палитру влезает "
                         << sage::anim::kMaxBones << " — лишние не анимируются";
    }

    // Локальный трансформ кости — из свойств узла (с пред- и постповоротом), а
    // матрица привязки — из кластера. Это разные вещи и берутся из разных мест
    // намеренно: поза покоя описана узлами, а привязка меша к костям — только
    // кластерами.
    out.Skeleton.Joints.clear();
    out.Skeleton.Joints.resize(bones.size());
    for (size_t i = 0; i < bones.size(); ++i) {
        const Node& node = *byUid[bones[i]];
        sage::anim::Joint& joint = out.Skeleton.Joints[i];
        joint.Name = ObjectName(node);
        const int64_t parent = parentOfKind(bones[i], "Model");
        joint.Parent = (parent != 0 && jointOf.count(parent)) ? jointOf[parent] : -1;
        // Сопряжение C * M * C⁻¹: так преобразование переезжает в систему
        // движка целиком — вместе с тем, как оно двигает точки.
        Decompose(C * ReadTransform(node).Matrix() * Cinv, joint.Translation, joint.Rotation,
                  joint.Scale);
    }

    // --- Геометрия со скином -------------------------------------------------
    std::vector<std::string> warnings;
    int submeshes = 0;
    for (const Node& n : objects->Children) {
        if (n.Name != "Geometry") continue;
        auto skin = skinOfGeometry.find(Uid(n));
        if (skin == skinOfGeometry.end()) continue;   // меш без скина — не наш случай

        // Трансформ узла, которому принадлежит геометрия: вершины хранятся уже
        // в мировом пространстве файла (так же, как у статического пути).
        glm::mat4 nodeWorld(1.0f);
        int64_t modelUid = 0;
        modelUid = parentOfKind(Uid(n), "Model");
        if (modelUid != 0) {
            glm::mat4 m(1.0f);
            for (int64_t cur = modelUid, guard = 0; cur != 0 && guard < 64; ++guard) {
                auto obj = byUid.find(cur);
                if (obj == byUid.end() || obj->second->Name != "Model") break;
                m = ReadTransform(*obj->second).Matrix() * m;
                cur = parentOfKind(cur, "Model");
            }
            nodeWorld = m;
        }

        const std::vector<fbx::MeshCorner> corners =
            fbx::BuildCorners(n, units, nodeWorld, warnings);
        if (corners.empty()) continue;

        // Веса по КОНТРОЛЬНЫМ ТОЧКАМ: в FBX они заданы именно так, а вершин
        // после разбиения многоугольников больше — одна контрольная точка даёт
        // по вершине на каждый свой угол.
        struct Influence { int Joint; float Weight; };
        std::unordered_map<int64_t, std::vector<Influence>> weights;
        for (const Cluster& c : clustersOfSkin[skin->second]) {
            auto jointIt = jointOf.find(c.Bone);
            if (jointIt == jointOf.end()) continue;
            const std::vector<int64_t>* idx = Ints(c.Record->Find("Indexes"));
            const std::vector<double>* w = Doubles(c.Record->Find("Weights"));
            if (!idx || !w) continue;
            const size_t count = std::min(idx->size(), w->size());
            for (size_t k = 0; k < count; ++k) {
                if ((*w)[k] <= 0.0) continue;
                weights[(*idx)[k]].push_back({jointIt->second, (float)(*w)[k]});
            }
            // Обратная матрица привязки. Вершины лежат в мировом пространстве
            // файла, поэтому к обычной inverse(TransformLink) добавляется
            // приведение из этого пространства: Transform * inverse(nodeWorld)
            // (у согласованного экспорта это единица, у рассогласованного —
            // именно то, что спасает модель от разъезда).
            const glm::mat4 ib =
                glm::inverse(c.TransformLink) * c.Transform * glm::inverse(nodeWorld);
            out.Skeleton.Joints[(size_t)jointIt->second].InverseBind = C * ib * Cinv;
        }

        sage::render::ModelSubMeshData sub;
        sub.Vertices.reserve(corners.size());
        sub.Indices.reserve(corners.size());
        for (const fbx::MeshCorner& corner : corners) {
            sage::render::SkinnedVertex v;
            v.Position = corner.Position;
            v.Normal = corner.Normal;
            v.TexCoords = corner.TexCoords;

            auto it = weights.find(corner.ControlPoint);
            if (it != weights.end()) {
                // ЧЕТЫРЕ САМЫХ ВЕСОМЫХ. В шейдере на вершину ровно четыре
                // места; отбросить «первые попавшиеся» значит потерять главную
                // кость и получить вершину, висящую на мизинце.
                std::vector<Influence> list = it->second;
                std::sort(list.begin(), list.end(),
                          [](const Influence& a, const Influence& b) { return a.Weight > b.Weight; });
                float sum = 0.0f;
                for (size_t k = 0; k < list.size() && k < 4; ++k) {
                    v.Joints[(int)k] = (float)list[k].Joint;
                    v.Weights[(int)k] = list[k].Weight;
                    sum += list[k].Weight;
                }
                // Нормировка обязательна: сумма весов после отсечения лишних
                // меньше единицы, и вершина уезжает к началу координат.
                if (sum > 1e-6f) v.Weights /= sum;
            } else {
                // Вершина без весов остаётся на месте — привязываем к корню с
                // единичным весом, иначе она схлопнется в ноль.
                v.Joints = glm::vec4(0.0f);
                v.Weights = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
            }
            sub.Indices.push_back((unsigned int)sub.Vertices.size());
            sub.Vertices.push_back(v);
        }
        out.SubMeshes.push_back(std::move(sub));
        ++submeshes;
        (void)modelUid;
    }

    if (out.SubMeshes.empty()) {
        err = "в файле FBX есть скин, но нет геометрии с весами";
        return false;
    }

    // --- Клипы ---------------------------------------------------------------
    //
    // AnimationStack — это КЛИП («Idle», «Run»), Layer — его слой, CurveNode —
    // канал свойства узла, Curve — сама кривая по одной оси. Имя клипа берётся
    // у стека: номер зависит от порядка в файле и молча меняется при
    // переэкспорте (см. anim/AnimationComponents.h о том, почему это важно).
    std::unordered_map<int64_t, Curve> curves;
    for (const Node& n : objects->Children) {
        if (n.Name != "AnimationCurve") continue;
        Curve c;
        const std::vector<int64_t>* times = Ints(n.Find("KeyTime"));
        const std::vector<double>* values = Doubles(n.Find("KeyValueFloat"));
        if (!times || !values) continue;
        const size_t count = std::min(times->size(), values->size());
        c.Times.reserve(count);
        c.Values.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            c.Times.push_back((double)(*times)[i] / kFbxTimeUnit);
            c.Values.push_back((float)(*values)[i]);
        }
        if (!c.Empty()) curves[Uid(n)] = std::move(c);
    }

    // Слой -> узел -> его каналы.
    std::unordered_map<int64_t, std::unordered_map<int64_t, AnimatedNode>> layers;
    for (const Node& n : objects->Children) {
        if (n.Name != "AnimationCurveNode") continue;
        const int64_t nodeUid = Uid(n);

        // Куда этот канал смотрит: CurveNode --OP("Lcl Rotation")--> Model.
        int64_t target = 0;
        std::string property;
        auto propRange = conns.PropertyOf.equal_range(nodeUid);
        for (auto it = propRange.first; it != propRange.second; ++it) {
            if (jointOf.count(it->second.Parent)) {
                target = it->second.Parent;
                property = it->second.Property;
                break;
            }
        }
        if (target == 0) continue;   // канал не на кости скина

        // В каком слое живёт: CurveNode --OO--> AnimationLayer.
        const int64_t layer = parentOfKind(nodeUid, "AnimationLayer");

        ChannelCurves channel;
        // Кривые осей: Curve --OP("d|X")--> CurveNode.
        for (const auto& [curveUid, curve] : curves) {
            auto range = conns.PropertyOf.equal_range(curveUid);
            for (auto it = range.first; it != range.second; ++it) {
                if (it->second.Parent != nodeUid) continue;
                const std::string& axis = it->second.Property;
                if (axis == "d|X") channel.X = curve;
                else if (axis == "d|Y") channel.Y = curve;
                else if (axis == "d|Z") channel.Z = curve;
            }
        }
        if (channel.Empty()) continue;

        AnimatedNode& animated = layers[layer][target];
        if (property == "Lcl Translation") animated.Translation = channel;
        else if (property == "Lcl Rotation") animated.Rotation = channel;
        else if (property == "Lcl Scaling") animated.Scale = channel;
    }

    // Имя стека и его слои.
    std::unordered_map<int64_t, std::string> stackNames;
    for (const Node& n : objects->Children) {
        if (n.Name == "AnimationStack") stackNames[Uid(n)] = ObjectName(n);
    }

    for (const auto& [layerUid, nodes] : layers) {
        if (nodes.empty()) continue;
        sage::anim::AnimationClip clip;
        // Слой принадлежит стеку: Layer --OO--> Stack.
        if (const int64_t stack = parentOfKind(layerUid, "AnimationStack"); stack != 0) {
            auto name = stackNames.find(stack);
            if (name != stackNames.end()) clip.Name = name->second;
        }
        if (clip.Name.empty()) clip.Name = "clip" + std::to_string(out.Clips.size());

        for (const auto& [boneUid, animated] : nodes) {
            auto jointIt = jointOf.find(boneUid);
            if (jointIt == jointOf.end() || !animated.Any()) continue;
            const Node& boneNode = *byUid[boneUid];
            const NodeTransform rest = ReadTransform(boneNode);

            // ВРЕМЕНА ВСЕХ КРИВЫХ СРАЗУ. Перенос, поворот и масштаб в FBX живут
            // отдельными кривыми со своими ключами; свести их в общий набор
            // времён нужно потому, что локальный трансформ собирается из всех
            // трёх разом (предповорот, опорные точки — см. NodeTransform), и
            // посчитать его «по каналу» нельзя.
            std::vector<double> times;
            animated.Translation.CollectTimes(times);
            animated.Rotation.CollectTimes(times);
            animated.Scale.CollectTimes(times);
            std::sort(times.begin(), times.end());
            times.erase(std::unique(times.begin(), times.end(),
                                    [](double a, double b) { return std::abs(a - b) < 1e-6; }),
                        times.end());
            if (times.empty()) continue;

            sage::anim::AnimChannel chT, chR, chS;
            chT.Joint = chR.Joint = chS.Joint = jointIt->second;
            chT.Target = sage::anim::AnimPath::Translation;
            chR.Target = sage::anim::AnimPath::Rotation;
            chS.Target = sage::anim::AnimPath::Scale;

            for (double t : times) {
                NodeTransform frame = rest;
                frame.Translation = animated.Translation.At(t, rest.Translation);
                frame.Rotation = animated.Rotation.At(t, rest.Rotation);
                frame.Scale = animated.Scale.At(t, rest.Scale);

                glm::vec3 translation, scale;
                glm::quat rotation;
                Decompose(C * frame.Matrix() * Cinv, translation, rotation, scale);

                const float time = (float)t;
                chT.Times.push_back(time);
                chT.Values.push_back(glm::vec4(translation, 0.0f));
                chR.Times.push_back(time);
                chR.Values.push_back(glm::vec4(rotation.x, rotation.y, rotation.z, rotation.w));
                chS.Times.push_back(time);
                chS.Values.push_back(glm::vec4(scale, 0.0f));
                clip.Duration = std::max(clip.Duration, time);
            }
            clip.Channels.push_back(std::move(chT));
            clip.Channels.push_back(std::move(chR));
            clip.Channels.push_back(std::move(chS));
        }
        if (!clip.Channels.empty()) out.Clips.push_back(std::move(clip));
    }

    // Клипы по именам: порядок обхода слоёв зависит от хеш-таблицы, а человек
    // ждёт в списке один и тот же порядок от запуска к запуску.
    std::sort(out.Clips.begin(), out.Clips.end(),
              [](const sage::anim::AnimationClip& a, const sage::anim::AnimationClip& b) {
                  return a.Name < b.Name;
              });

    for (const std::string& w : warnings) LOG_WARN("Anim") << "FBX: " << w;
    LOG_INFO("Anim") << "FBX со скином разобран: " << path << " (костей "
                     << out.Skeleton.Count() << ", submesh " << submeshes << ", клипов "
                     << out.Clips.size() << ")";
    return true;
}

} // namespace sage::assets
