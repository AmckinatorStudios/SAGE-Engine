#pragma once
#include <glm/glm.hpp>
#include <random>
#include <string>

// Рыбалка: с удочкой в руке ПКМ забрасывает поплавок на воду перед игроком.
// Через случайные 4-9 секунд — поклёвка ("!"), у игрока есть ~1.6 секунды
// подсечь (ЛКМ). Успел — сырая рыба в инвентарь; нет — сорвалась.
class FishingSystem {
public:
    enum class State { Idle, Waiting, Bite };

    FishingSystem() : m_rng(std::random_device{}()) {}

    State CurrentState() const { return m_state; }
    glm::vec3 BobberPosition() const { return m_bobberPos; }
    float BobPhase() const { return m_bobPhase; }

    bool IsActive() const { return m_state != State::Idle; }

    // Заброс: точка на воде перед игроком
    void Cast(glm::vec3 eyePos, glm::vec3 viewDir, float seaLevel) {
        // Проецируем взгляд на плоскость воды; если игрок смотрит вверх —
        // просто кидаем на фиксированную дистанцию перед собой
        glm::vec3 target;
        if (viewDir.y < -0.05f) {
            float t = (seaLevel - eyePos.y) / viewDir.y;
            t = glm::clamp(t, 2.0f, 14.0f);
            target = eyePos + viewDir * t;
        } else {
            glm::vec3 flat = glm::normalize(glm::vec3(viewDir.x, 0.0f, viewDir.z));
            target = eyePos + flat * 7.0f;
        }
        m_bobberPos = {target.x, seaLevel + 0.05f, target.z};
        m_state = State::Waiting;
        m_timer = std::uniform_real_distribution<float>(4.0f, 9.0f)(m_rng);
        m_bobPhase = 0.0f;
    }

    void Reel() { m_state = State::Idle; } // смотать без улова

    // Только для тестов/CI-скриншотов: телепортирует эмиттер сразу в Bite,
    // минуя реальное ожидание — иначе автотест поклёвки занимал бы 4-9
    // реальных секунд на кадр. В обычном геймплее не используется.
    void DebugForceBite() {
        if (m_state == State::Idle) return;
        m_state = State::Bite;
        m_timer = 1.6f;
    }

    // ЛКМ во время поклёвки. true = рыба поймана.
    bool TryHook() {
        if (m_state != State::Bite) return false;
        m_state = State::Idle;
        return true;
    }

    void Update(float dt) {
        if (m_state == State::Idle) return;
        m_bobPhase += dt;

        m_timer -= dt;
        if (m_state == State::Waiting && m_timer <= 0.0f) {
            m_state = State::Bite;
            m_timer = 1.6f; // окно подсечки
        } else if (m_state == State::Bite && m_timer <= 0.0f) {
            m_state = State::Idle; // сорвалась
        }
    }

    // Позиция поплавка с покачиванием (при поклёвке дёргается сильнее)
    glm::vec3 VisualBobberPosition() const {
        float amp = (m_state == State::Bite) ? 0.22f : 0.06f;
        float freq = (m_state == State::Bite) ? 9.0f : 1.6f;
        glm::vec3 p = m_bobberPos;
        p.y += amp * std::sin(m_bobPhase * freq);
        return p;
    }

private:
    State m_state = State::Idle;
    glm::vec3 m_bobberPos{0.0f};
    float m_timer = 0.0f;
    float m_bobPhase = 0.0f;
    std::mt19937 m_rng;
};
