#pragma once
#include "Scene.h"
#include "SceneSerializer.h"
#include <algorithm>
#include <unordered_map>
#include <memory>
#include <string>
#include <stdexcept>
#include <vector>

// Управляет набором сцен (например: "MainMenu", "Level1", "Level2") и тем,
// какая из них сейчас активна и рендерится/обновляется в игровом цикле.
//
// Сцена живёт в unique_ptr — адрес у неё стабильный, и указатель Active()
// можно держать между кадрами. Но только пока сцену не ЗАМЕНИЛИ: загрузка
// файла с тем же именем, что у живой сцены, раньше молча уничтожала прежнюю,
// а Active() продолжал указывать на освободившуюся память. Теперь замена
// переносит и «активную», если активной была заменяемая.
class SceneManager {
public:
    Scene& CreateScene(const std::string& name) {
        return Put(std::make_unique<Scene>(name), false);
    }

    // Загружает сцену из файла и регистрирует её под именем, указанным внутри файла
    Scene& LoadScene(const std::string& filePath, bool setActive = true) {
        return Put(SceneSerializer::Load(filePath), setActive);
    }

    void SaveScene(const std::string& sceneName, const std::string& filePath) {
        auto it = m_scenes.find(sceneName);
        if (it == m_scenes.end())
            throw std::runtime_error("Сцена не найдена: " + sceneName);
        SceneSerializer::Save(*it->second, filePath);
    }

    void SaveActiveScene(const std::string& filePath) {
        if (!m_active) throw std::runtime_error("Нет активной сцены для сохранения");
        SceneSerializer::Save(*m_active, filePath);
    }

    bool SetActive(const std::string& name) {
        auto it = m_scenes.find(name);
        if (it == m_scenes.end()) return false;
        m_active = it->second.get();
        return true;
    }

    // Выгрузить сцену. Активную — тоже: активной тогда становится любая
    // оставшаяся (или никакой), а не висячий указатель.
    bool Unload(const std::string& name) {
        auto it = m_scenes.find(name);
        if (it == m_scenes.end()) return false;
        const bool wasActive = m_active == it->second.get();
        m_scenes.erase(it);
        if (wasActive) m_active = m_scenes.empty() ? nullptr : m_scenes.begin()->second.get();
        return true;
    }

    // Переименовать сцену вместе с ключом, по которому её ищут.
    bool Rename(const std::string& from, const std::string& to) {
        if (from == to) return Has(from);
        auto it = m_scenes.find(from);
        if (it == m_scenes.end() || Has(to)) return false;
        std::unique_ptr<Scene> scene = std::move(it->second);
        m_scenes.erase(it);
        scene->SetName(to);
        m_scenes[to] = std::move(scene);
        return true;
    }

    bool Has(const std::string& name) const { return m_scenes.count(name) != 0; }
    size_t Count() const { return m_scenes.size(); }

    // Имена всех сцен — по алфавиту, чтобы список не зависел от хэш-таблицы.
    std::vector<std::string> Names() const {
        std::vector<std::string> out;
        out.reserve(m_scenes.size());
        for (const auto& [name, scene] : m_scenes) out.push_back(name);
        std::sort(out.begin(), out.end());
        return out;
    }

    Scene* Active() { return m_active; }

    Scene* Get(const std::string& name) {
        auto it = m_scenes.find(name);
        return it != m_scenes.end() ? it->second.get() : nullptr;
    }

private:
    Scene& Put(std::unique_ptr<Scene> scene, bool setActive) {
        const std::string name = scene->Name();
        Scene& ref = *scene;
        auto it = m_scenes.find(name);
        // Заменяем живую сцену с тем же именем: если активной была она —
        // активной становится новая, иначе Active() смотрел бы в пустоту.
        const bool replacingActive = it != m_scenes.end() && m_active == it->second.get();
        m_scenes[name] = std::move(scene);
        if (setActive || replacingActive || !m_active) m_active = &ref;
        return ref;
    }

    std::unordered_map<std::string, std::unique_ptr<Scene>> m_scenes;
    Scene* m_active = nullptr;
};
