#pragma once
#include <filesystem>
#include <string>
#include <vector>

#include "PluginAPI.h"

// ============================================================================
//  PluginManager — сканирует каталог плагинов редактора, грузит .so/.dll,
//  резолвит фабричные функции (см. PluginAPI.h) и держит жизненный цикл
//  загруженных плагинов (OnLoad/OnUpdate/OnImGui/OnUnload).
//
//  Каталог плагинов по умолчанию — plugins/ рядом с бинарником SageEditor;
//  переопределяется переменной окружения SAGE_PLUGINS_DIR (тот же паттерн
//  env-переопределений, что и SAGE_SCREENSHOT_*/SAGE_EDITOR_SELFTEST).
// ============================================================================
class PluginManager {
public:
    ~PluginManager() { UnloadAll(); }

    // Сканирует directory на .so (Linux) / .dll (Windows), грузит каждый,
    // резолвит CreateSageEditorPlugin/DestroySageEditorPlugin, вызывает OnLoad.
    // Ошибки загрузки отдельного файла логируются и не прерывают сканирование.
    void LoadAll(const std::filesystem::path& directory, EditorPluginContext& ctx);

    // ОБЯЗАТЕЛЬНО вызывать до разрушения ImGui-контекста — плагины рисуют
    // через тот же ImGui, что и хост.
    void UnloadAll();

    void UpdateAll(float dt);
    void ImGuiAll();

    size_t Count() const { return m_plugins.size(); }

private:
    struct LoadedPlugin {
        void* Handle = nullptr; // dlopen/LoadLibrary handle
        DestroyPluginFn Destroy = nullptr;
        SageEditorPlugin* Instance = nullptr;
        std::string Path;
    };

    std::vector<LoadedPlugin> m_plugins;
};
