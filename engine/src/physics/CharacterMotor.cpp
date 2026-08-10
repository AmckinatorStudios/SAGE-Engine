#include "sage/physics/CharacterMotor.h"

#include <algorithm>
#include <cmath>

namespace sage::physics {
namespace {

// Зазор между телом и поверхностью. Без него персонаж, вставший вплотную к
// полу, считается пересекающим его: следующий же запрос отвечает «занято», и
// он не может сдвинуться ни на шаг — стоит и дрожит.
constexpr float kSkin = 0.002f;

// Сколько половинных делений тратим на поиск упора. Шесть дают точность
// 1/64 шага — при шаге в сантиметры это доли миллиметра, то есть меньше
// зазора выше. Больше итераций уточняли бы то, чего уже не видно.
constexpr int kBisect = 6;

// На сколько щупаем опору под подошвой. Меньше зазора нельзя (не заметим пол),
// заметно больше — нельзя тоже: персонаж считал бы себя стоящим, паря над
// полом на видимой глазу высоте.
constexpr float kGroundProbe = 0.02f;

// Минимальный подъём, который вообще считается ступенькой. Ниже — это не
// ступенька, а неровность зазора, и «шагать» на неё значит дёргать камеру.
constexpr float kMinRise = 0.01f;

} // namespace

void CharacterMotor::Configure(const CharacterDesc& desc) {
    m_desc = desc;
    m_desc.Radius = std::max(0.01f, m_desc.Radius);
    m_desc.Height = std::max(0.05f, m_desc.Height);
    // Ступенька выше собственного роста — это не ходьба, а телепорт: тело
    // проскакивало бы над препятствиями целиком. Ограничение здесь, а не в
    // StepMove, чтобы неверная настройка чинилась один раз при создании.
    m_desc.StepHeight = std::clamp(m_desc.StepHeight, 0.0f, m_desc.Height * 0.9f);
    m_state.Position = m_desc.Position;
}

void CharacterMotor::BoundsAt(const glm::vec3& feet, glm::vec3& min, glm::vec3& max) const {
    const float r = m_desc.Radius - kSkin;
    min = glm::vec3(feet.x - r, feet.y + kSkin, feet.z - r);
    max = glm::vec3(feet.x + r, feet.y + m_desc.Height - kSkin, feet.z + r);
}

bool CharacterMotor::Fits(const SolidQuery& solid, const glm::vec3& feet) const {
    if (!solid) return true;
    glm::vec3 lo, hi;
    BoundsAt(feet, lo, hi);
    return !solid(lo, hi);
}

float CharacterMotor::SlideAxis(const SolidQuery& solid, int axis, float d) {
    if (d == 0.0f) return 0.0f;

    glm::vec3 target = m_state.Position;
    target[axis] += d;
    if (Fits(solid, target)) {
        m_state.Position = target;
        return d;
    }

    // Упёрлись. Половинным делением ищем, насколько всё-таки можно продвинуться,
    // чтобы встать ВПЛОТНУЮ к препятствию, а не остановиться за полшага до него.
    // Без этого персонаж застревает в сантиметрах от стены, и щель между ним и
    // стеной видно из-за угла.
    float lo = 0.0f, hi = d;
    for (int i = 0; i < kBisect; ++i) {
        const float mid = (lo + hi) * 0.5f;
        glm::vec3 probe = m_state.Position;
        probe[axis] += mid;
        if (Fits(solid, probe)) lo = mid; else hi = mid;
    }
    m_state.Position[axis] += lo;
    return lo;
}

void CharacterMotor::Unstick(const SolidQuery& solid) {
    if (Fits(solid, m_state.Position)) return;
    // Наверх, а не в ближайшую свободную сторону: вбок персонаж уехал бы сквозь
    // стену, а вверх — выберется на крышу того, во что его замуровали. Шаг —
    // четверть роста, потолок попыток — рост целиком: если и там твердь, мир
    // сплошной, и дёргать тело бесконечно бессмысленно.
    const float step = m_desc.Height * 0.25f;
    for (float lifted = 0.0f; lifted < m_desc.Height; lifted += step) {
        glm::vec3 probe = m_state.Position;
        probe.y += step;
        m_state.Position = probe;
        if (Fits(solid, m_state.Position)) return;
    }
}

void CharacterMotor::StepMove(const SolidQuery& solid, float dx, float dz, bool grounded) {
    const glm::vec3 start = m_state.Position;

    bool hit = false;
    if (dx != 0.0f) hit = std::abs(SlideAxis(solid, 0, dx)) < std::abs(dx) - 1e-6f || hit;
    if (dz != 0.0f) hit = std::abs(SlideAxis(solid, 2, dz)) < std::abs(dz) - 1e-6f || hit;
    if (!hit) return;

    m_blocked = true;
    if (!grounded || m_desc.StepHeight <= 0.0f) return;

    // Куда дошли обычной ходьбой — с этим и будем сравнивать: ступенька имеет
    // смысл, только если она даёт пройти ДАЛЬШЕ.
    const glm::vec3 walked = m_state.Position;
    m_state.Position = start;

    // Условие первое: подняться не выше StepHeight и не упереться головой.
    // SlideAxis сам остановит подъём под потолком, и тогда rise окажется
    // меньше запрошенного — на низкой галерее персонаж не «шагнёт» в перекрытие.
    const float rise = SlideAxis(solid, 1, m_desc.StepHeight);
    if (rise < kMinRise) {
        m_state.Position = walked;
        return;
    }

    if (dx != 0.0f) SlideAxis(solid, 0, dx);
    if (dz != 0.0f) SlideAxis(solid, 2, dz);

    const float gainedStep = std::abs(m_state.Position.x - start.x) + std::abs(m_state.Position.z - start.z);
    const float gainedWalk = std::abs(walked.x - start.x) + std::abs(walked.z - start.z);
    if (gainedStep <= gainedWalk + 1e-5f) {
        m_state.Position = walked;   // подъём ничего не дал — стена, а не ступенька
        return;
    }

    // Условие третье, и самое важное: ОПУСТИТЬСЯ ОБРАТНО НА ОПОРУ.
    //
    // Именно его не хватает наивной реализации, и именно поэтому она позволяет
    // взбираться по отвесу: подняв тело на высоту ступеньки и оставив его там,
    // алгоритм каждый следующий кадр начинает подъём с новой высоты. Двадцать
    // кадров у стены — и персонаж стоит на её верхушке, ни разу не прыгнув.
    //
    // Спуск отвечает на вопрос «а было ли на что вставать». Прошли весь путь
    // вниз, ни во что не упёршись, — опоры нет, ступеньки не было, шаг
    // отменяется целиком.
    const float dropped = -SlideAxis(solid, 1, -rise);
    if (dropped >= rise - 1e-5f) {
        m_state.Position = walked;
        return;
    }

    m_stepUp = rise - dropped;
    m_stepped = true;
}

void CharacterMotor::Move(const SolidQuery& solid, const glm::vec3& velocity, float dt) {
    m_landed = false;
    m_leftGround = false;
    m_blocked = false;
    m_stepped = false;
    m_stepUp = 0.0f;

    m_state.Velocity = velocity;
    if (dt <= 0.0f) return;

    const bool wasGrounded = m_state.Grounded;

    if (!solid) {
        // Мира нет — лететь свободно честнее, чем стоять: игра увидит, что
        // персонаж проваливается, и найдёт причину. Молчаливая неподвижность
        // выглядела бы «залипшим управлением».
        m_state.Position += velocity * dt;
        m_state.Grounded = false;
        m_leftGround = wasGrounded;
        return;
    }

    Unstick(solid);

    const glm::vec3 delta = velocity * dt;

    // Дробим шаг так, чтобы за раз не проскочить мимо препятствия: подшаг
    // длиннее половины тела означает туннелирование сквозь стену — на большой
    // скорости или на длинном кадре персонаж оказывается по ту сторону, и
    // никакая проверка объёма этого уже не заметит.
    const float maxStep = std::max(0.05f, std::min(m_desc.Radius, m_desc.Height * 0.5f) * 0.8f);
    const float len = glm::length(delta);
    const int steps = std::max(1, (int)std::ceil(len / maxStep));
    const glm::vec3 d = delta / (float)steps;

    bool grounded = false;
    for (int i = 0; i < steps; ++i) {
        // Вертикаль первой: пока не разобрались, стоим мы или падаем, вопрос
        // «можно ли взойти на ступеньку» не имеет ответа.
        if (d.y != 0.0f) {
            const float moved = SlideAxis(solid, 1, d.y);
            if (std::abs(moved) < std::abs(d.y) - 1e-6f) {
                if (d.y < 0.0f) grounded = true;   // упёрлись подошвой — это пол
                m_state.Velocity.y = 0.0f;
            }
        }
        StepMove(solid, d.x, d.z, wasGrounded || grounded || m_stepped);
    }
    if (m_stepped) grounded = true;

    // Опора под подошвой. Без этой проверки персонаж, которому игра каждый кадр
    // обнуляет вертикальную скорость (стоит на месте), считает себя в воздухе:
    // прыжок не работает, шаги не звучат, а «в воздухе» он ещё и не всходит на
    // ступеньки.
    if (!grounded && m_state.Velocity.y <= 0.0f) {
        glm::vec3 probe = m_state.Position;
        probe.y -= kGroundProbe;
        if (!Fits(solid, probe)) grounded = true;
    }

    // Прилипание к спуску: сходя по лестнице вниз, персонаж иначе отрывается от
    // каждой ступеньки и летит до следующей — камера скачет, шаги молчат, и
    // спуск выглядит серией маленьких падений. Ищем опору не глубже той же
    // высоты ступеньки; не нашли — падаем честно, ничего не подкручивая.
    if (!grounded && wasGrounded && m_state.Velocity.y <= 0.0f && m_desc.StepHeight > 0.0f) {
        const float before = m_state.Position.y;
        const float dropped = -SlideAxis(solid, 1, -m_desc.StepHeight);
        if (dropped < m_desc.StepHeight - 1e-5f) {
            grounded = true;
        } else {
            m_state.Position.y = before;
        }
    }

    m_landed = grounded && !wasGrounded;
    m_leftGround = wasGrounded && !grounded;
    m_state.Grounded = grounded;
    // Нормаль опоры у коробочного мира всегда вертикальна: грани вокселей и
    // AABB-тел не бывают наклонными, и выдумывать наклон значило бы врать игре,
    // которая по этой нормали ориентирует персонажа.
    m_state.GroundNormal = glm::vec3(0.0f, 1.0f, 0.0f);
    m_state.OnSteepSlope = false;
    if (grounded && m_state.Velocity.y < 0.0f) m_state.Velocity.y = 0.0f;
}

} // namespace sage::physics
