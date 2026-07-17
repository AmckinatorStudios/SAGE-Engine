#pragma once
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

// ---------------------------------------------------------------------------
// Скелет и анимационные клипы — данные скелетной анимации, независимые от GPU
// и графического API (как sage/physics независим от бэкенда физики). Загружаются
// из glTF (см. SkinnedModel) или строятся процедурно; проигрываются классом
// Animator, который выдаёт палитру матриц костей для скиннинг-шейдера.
// ---------------------------------------------------------------------------
namespace sage::anim {

// Максимум костей в палитре — совпадает с размером uBones[] в скиннинг-шейдере.
constexpr int kMaxBones = 128;

// Кость (сустав) скелета. Локальный дефолт хранится как TRS, чтобы каналы
// анимации могли независимо переопределять перенос/поворот/масштаб, а поворот
// корректно интерполировался сферически (slerp).
struct Joint {
    std::string Name;
    int Parent = -1;                       // индекс родителя в Skeleton::Joints (-1 — корень)
    glm::vec3 Translation{0.0f};
    glm::quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 Scale{1.0f};
    glm::mat4 InverseBind{1.0f};           // mesh-space -> joint-space (из glTF inverseBindMatrices)

    glm::mat4 LocalMatrix() const;         // TRS дефолтной позы
};

struct Skeleton {
    std::vector<Joint> Joints;
    int Count() const { return (int)Joints.size(); }
};

// Что анимирует канал у одной кости.
enum class AnimPath { Translation, Rotation, Scale };
// Как интерполировать между ключами. CUBICSPLINE трактуем как Linear.
enum class AnimInterp { Linear, Step };

// Один канал: последовательность ключей (время → значение) для одного свойства
// одной кости. Для Translation/Scale значимы xyz Values; для Rotation — кватернион
// (x,y,z,w) в Values.
struct AnimChannel {
    int Joint = -1;
    AnimPath Target = AnimPath::Translation;
    AnimInterp Interp = AnimInterp::Linear;
    std::vector<float> Times;      // возрастающие метки времени (сек)
    std::vector<glm::vec4> Values; // по одному значению на метку

    // Сэмплирует канал в момент t: перенос/масштаб — в outVec3, поворот — в outQuat.
    // Используется только соответствующее поле (по Target).
    void Sample(float t, glm::vec3& outVec3, glm::quat& outQuat) const;
};

// Анимационный клип: набор каналов + длительность (макс. время ключей).
struct AnimationClip {
    std::string Name;
    float Duration = 0.0f;
    std::vector<AnimChannel> Channels;
};

} // namespace sage::anim
