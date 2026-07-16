#include "PluginManager.h"

#include "sage/core/Log.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace fs = std::filesystem;

namespace {

#ifdef _WIN32
constexpr const char* kPluginExt = ".dll";
void* PlatformOpen(const fs::path& path) { return static_cast<void*>(LoadLibraryA(path.string().c_str())); }
void* PlatformSymbol(void* handle, const char* name) {
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle), name));
}
void PlatformClose(void* handle) { FreeLibrary(static_cast<HMODULE>(handle)); }
std::string PlatformLastError() { return "LoadLibrary/GetProcAddress failed (code " + std::to_string(GetLastError()) + ")"; }
#else
constexpr const char* kPluginExt = ".so";
void* PlatformOpen(const fs::path& path) { return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL); }
void* PlatformSymbol(void* handle, const char* name) { return dlsym(handle, name); }
void PlatformClose(void* handle) { dlclose(handle); }
std::string PlatformLastError() {
    const char* err = dlerror();
    return err ? err : "unknown dlopen/dlsym error";
}
#endif

} // namespace

void PluginManager::LoadAll(const fs::path& directory, EditorPluginContext& ctx) {
    std::error_code ec;
    if (!fs::is_directory(directory, ec)) {
        LOG_INFO("Plugins") << "Каталог плагинов не найден (" << directory.string() << "), пропускаю";
        return;
    }

    for (const auto& entry : fs::directory_iterator(directory, ec)) {
        if (ec) break;
        if (entry.path().extension() != kPluginExt) continue;

        void* handle = PlatformOpen(entry.path());
        if (!handle) {
            LOG_ERROR("Plugins") << "Не удалось загрузить " << entry.path().string() << ": " << PlatformLastError();
            continue;
        }

        auto create = reinterpret_cast<CreatePluginFn>(PlatformSymbol(handle, kCreatePluginSymbol));
        auto destroy = reinterpret_cast<DestroyPluginFn>(PlatformSymbol(handle, kDestroyPluginSymbol));
        if (!create || !destroy) {
            LOG_ERROR("Plugins") << entry.path().string() << ": не найдены символы "
                                 << kCreatePluginSymbol << "/" << kDestroyPluginSymbol;
            PlatformClose(handle);
            continue;
        }

        SageEditorPlugin* instance = create();
        if (!instance) {
            LOG_ERROR("Plugins") << entry.path().string() << ": " << kCreatePluginSymbol << " вернул nullptr";
            PlatformClose(handle);
            continue;
        }

        instance->OnLoad(ctx);
        LOG_INFO("Plugins") << "Загружен плагин: " << instance->Name() << " (" << entry.path().filename().string() << ")";

        LoadedPlugin loaded;
        loaded.Handle = handle;
        loaded.Destroy = destroy;
        loaded.Instance = instance;
        loaded.Path = entry.path().string();
        m_plugins.push_back(loaded);
    }
}

void PluginManager::UnloadAll() {
    for (auto& p : m_plugins) {
        p.Instance->OnUnload();
        p.Destroy(p.Instance);
        PlatformClose(p.Handle);
        LOG_INFO("Plugins") << "Выгружен плагин: " << p.Path;
    }
    m_plugins.clear();
}

void PluginManager::UpdateAll(float dt) {
    for (auto& p : m_plugins) p.Instance->OnUpdate(dt);
}

void PluginManager::ImGuiAll() {
    for (auto& p : m_plugins) p.Instance->OnImGui();
}
