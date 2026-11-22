#pragma once
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

// ---------------------------------------------------------------------------
// Скелет и клипы так, как их ДОСТАЁТ ИМПОРТ: кости, иерархия, bind-матрицы,
// ключи каналов. Только данные — без проигрывания и без смешивания.
//
// ПОЧЕМУ ОТДЕЛЬНО ОТ sage/anim. Разбор FBX и кэш моделей — работа слоя
// ресурсов, а sage::anim::Skeleton и AnimationClip — типы слоя мира, которые
// умеют сэмплировать себя и живут в аниматоре. Пока импорт писал прямо в них,
// ресурсы зависели от анимации. Теперь импорт заполняет эти структуры, а
// sage/anim сам строит из них свои (anim::SkeletonFromData, ClipFromData).
// Поля и их порядок — те же: кэш моделей пишет их без перевода.
// ---------------------------------------------------------------------------
namespace sage::assets {

// Сколько костей несёт палитра скиннинга. Здесь, а не в анимации, потому что
// это и предел того, что импорт готовит: номер кости за ним отбрасывается
// ещё при разборе весов (см. SkinInfluences.h). sage::anim::kMaxBones — это же
// число.
constexpr int kMaxSkinJoints = 256;

// Локальная матрица из TRS — одна формула для импорта и для анимации.
inline glm::mat4 TrsMatrix(const glm::vec3& t, const glm::quat& r, const glm::vec3& s) {
    return glm::translate(glm::mat4(1.0f), t) * glm::mat4_cast(r) * glm::scale(glm::mat4(1.0f), s);
}

// Кость: имя, родитель, поза покоя в TRS, обратная bind-матрица.
struct JointData {
    std::string Name;
    int Parent = -1;                       // индекс родителя в SkeletonData::Joints (-1 — корень)
    glm::vec3 Translation{0.0f};
    glm::quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 Scale{1.0f};
    glm::mat4 InverseBind{1.0f};           // mesh-space -> joint-space

    glm::mat4 LocalMatrix() const { return TrsMatrix(Translation, Rotation, Scale); }
};

struct SkeletonData {
    std::vector<JointData> Joints;
    // Трансформ НАД корневой костью (всё, что стоит в файле выше скелета) —
    // смысл тот же, что у sage::anim::Skeleton::Root.
    glm::mat4 Root{1.0f};

    int Count() const { return (int)Joints.size(); }
};

// Что анимирует канал у одной кости и как интерполировать между ключами.
// Перечисления общие с sage/anim (там — псевдонимы): числа значений пишет кэш.
enum class AnimPath { Translation, Rotation, Scale };
enum class AnimInterp { Linear, Step, CubicSpline };

// Ключи одного свойства одной кости. Translation/Scale — xyz Values;
// Rotation — кватернион (x,y,z,w). Касательные — только у CubicSpline.
struct ChannelData {
    int Joint = -1;
    AnimPath Target = AnimPath::Translation;
    AnimInterp Interp = AnimInterp::Linear;
    std::vector<float> Times;
    std::vector<glm::vec4> Values;
    std::vector<glm::vec4> InTangents;
    std::vector<glm::vec4> OutTangents;
};

struct ClipData {
    std::string Name;
    float Duration = 0.0f;
    std::vector<ChannelData> Channels;
};

} // namespace sage::assets
