#pragma once
#include <filesystem>
#include <functional>
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
    // wanted — грузить ли этот файл (окно «Плагины»: нативный плагин включают
    // по одному, потому что код C++ работает без всякой песочницы). Пусто — все.
    void LoadAll(const std::filesystem::path& directory, EditorPluginContext& ctx,
                 const std::function<bool(const std::filesystem::path&)>& wanted = {});
    bool LoadOne(const std::filesystem::path& file, EditorPluginContext& ctx);
    void UnloadOne(const std::filesystem::path& file);
    bool IsLoaded(const std::filesystem::path& file) const;
    // Имя загруженного плагина (Name()), пусто — не загружен.
    std::string NameOf(const std::filesystem::path& file) const;
    // Файлы нативных плагинов в каталоге (.so/.dll/.dylib), загруженные и нет.
    static std::vector<std::filesystem::path> Find(const std::filesystem::path& directory);
    static const char* Extension();

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
