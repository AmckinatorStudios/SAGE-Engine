#pragma once
#include "Scene.h"
#include "SceneSerializer.h"
#include <unordered_map>
#include <memory>
#include <string>
#include <stdexcept>

// Управляет набором сцен (например: "MainMenu", "Level1", "Level2") и тем,
// какая из них сейчас активна и рендерится/обновляется в игровом цикле.
class SceneManager {
public:
    Scene& CreateScene(const std::string& name) {
        auto scene = std::make_unique<Scene>(name);
        Scene& ref = *scene;
        m_scenes[name] = std::move(scene);
        if (!m_active) m_active = &ref;
        return ref;
    }

    // Загружает сцену из файла и регистрирует её под именем, указанным внутри файла
    Scene& LoadScene(const std::string& filePath, bool setActive = true) {
        auto scene = SceneSerializer::Load(filePath);
        std::string name = scene->Name();
        Scene& ref = *scene;
        m_scenes[name] = std::move(scene);
        if (setActive || !m_active) m_active = &ref;
        return ref;
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

    Scene* Active() { return m_active; }

    Scene* Get(const std::string& name) {
        auto it = m_scenes.find(name);
        return it != m_scenes.end() ? it->second.get() : nullptr;
    }

private:
    std::unordered_map<std::string, std::unique_ptr<Scene>> m_scenes;
    Scene* m_active = nullptr;
};
