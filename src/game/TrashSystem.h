#pragma once
#include "Items.h"
#include <glm/glm.hpp>
#include <vector>
#include <random>

// Плавающий в океане мусор — главный источник ресурсов. Спавнится впереди
// по курсу (корабль "плывёт" в -Z), дрейфует мимо корабля по +Z (иллюзия
// хода судна) и исчезает далеко за кормой. Подбирается клавишей F, когда
// игрок смотрит на предмет с близкого расстояния.
struct TrashItem {
    ItemType Type = ItemType::None;
    glm::vec3 Position{0.0f};
    float BobPhase = 0.0f; // фаза покачивания на волнах (у каждого своя)
};

class TrashSystem {
public:
    TrashSystem(float seaLevel, glm::vec3 shipCenter)
        : m_seaLevel(seaLevel), m_shipCenter(shipCenter), m_rng(std::random_device{}()) {}

    void Update(float dt) {
        m_spawnTimer -= dt;
        if (m_spawnTimer <= 0.0f && m_items.size() < kMaxItems) {
            SpawnOne();
            // Мусор приходит волнами с паузами — океан не должен выглядеть свалкой
            m_spawnTimer = std::uniform_real_distribution<float>(3.0f, 8.0f)(m_rng);
        }

        for (auto& item : m_items) {
            item.Position.z += kDriftSpeed * dt; // течение мимо корабля
            item.BobPhase += dt;
            // Покачивание на волнах
            item.Position.y = m_seaLevel + 0.10f + 0.08f * std::sin(item.BobPhase * 1.8f);
        }

        // Удаляем уплывшее далеко за корму
        m_items.erase(
            std::remove_if(m_items.begin(), m_items.end(), [this](const TrashItem& t) {
                return t.Position.z > m_shipCenter.z + 60.0f;
            }),
            m_items.end());
    }

    // Ближайший предмет в направлении взгляда: dot(взгляд, направление на
    // предмет) близок к 1 и дистанция мала. Возвращает индекс или -1.
    int FindTargeted(glm::vec3 eyePos, glm::vec3 viewDir, float maxDistance = 6.0f) const {
        int best = -1;
        float bestScore = 0.94f; // ~20 градусов конус
        for (size_t i = 0; i < m_items.size(); ++i) {
            glm::vec3 to = m_items[i].Position - eyePos;
            float dist = glm::length(to);
            if (dist > maxDistance || dist < 0.01f) continue;
            float alignment = glm::dot(to / dist, viewDir);
            if (alignment > bestScore) {
                bestScore = alignment;
                best = (int)i;
            }
        }
        return best;
    }

    ItemType Take(int index) {
        if (index < 0 || index >= (int)m_items.size()) return ItemType::None;
        ItemType type = m_items[index].Type;
        m_items.erase(m_items.begin() + index);
        return type;
    }

    const std::vector<TrashItem>& Items() const { return m_items; }

private:
    static constexpr size_t kMaxItems = 14;
    static constexpr float kDriftSpeed = 2.0f; // м/с — "скорость хода корабля"

    void SpawnOne() {
        static const ItemType kinds[] = {
            ItemType::ScrapWood, ItemType::ScrapWood, // дерево чаще — оно нужнее всего
            ItemType::ScrapPlastic, ItemType::ScrapMetal,
            ItemType::Cloth, ItemType::Rope
        };
        TrashItem item;
        item.Type = kinds[std::uniform_int_distribution<int>(0, 5)(m_rng)];
        // Впереди по курсу, разброс по ширине — часть проплывёт вне досягаемости,
        // придётся выбирать, за чем тянуться (лёгкий геймплей без стресса)
        item.Position = {
            m_shipCenter.x + std::uniform_real_distribution<float>(-14.0f, 14.0f)(m_rng),
            m_seaLevel + 0.1f,
            m_shipCenter.z - std::uniform_real_distribution<float>(35.0f, 55.0f)(m_rng)
        };
        item.BobPhase = std::uniform_real_distribution<float>(0.0f, 6.28f)(m_rng);
        m_items.push_back(item);
    }

    float m_seaLevel;
    glm::vec3 m_shipCenter;
    std::vector<TrashItem> m_items;
    float m_spawnTimer = 2.0f;
    std::mt19937 m_rng;
};
