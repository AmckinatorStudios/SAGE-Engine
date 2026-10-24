#include "PluginManager.h"

#include <algorithm>

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

void PluginManager::LoadAll(const fs::path& directory, EditorPluginContext& ctx,
                            const std::function<bool(const fs::path&)>& wanted) {
    std::error_code ec;
    if (!fs::is_directory(directory, ec)) {
        LOG_INFO("Plugins") << "Каталог плагинов не найден (" << directory.string() << "), пропускаю";
        return;
    }
    for (const fs::path& file : Find(directory))
        if (!wanted || wanted(file)) LoadOne(file, ctx);
}

std::vector<fs::path> PluginManager::Find(const fs::path& directory) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(directory, ec)) {
        if (ec) break;
        if (entry.is_regular_file(ec) && entry.path().extension() == kPluginExt) out.push_back(entry.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

const char* PluginManager::Extension() { return kPluginExt; }

bool PluginManager::IsLoaded(const fs::path& file) const {
    for (const auto& p : m_plugins)
        if (p.Path == file.string()) return true;
    return false;
}

std::string PluginManager::NameOf(const fs::path& file) const {
    for (const auto& p : m_plugins)
        if (p.Path == file.string()) return p.Instance->Name();
    return {};
}

bool PluginManager::LoadOne(const fs::path& file, EditorPluginContext& ctx) {
    if (IsLoaded(file)) return true;
    void* handle = PlatformOpen(file);
    if (!handle) {
        LOG_ERROR("Plugins") << "Не удалось загрузить " << file.string() << ": " << PlatformLastError();
        return false;
    }
    auto create = reinterpret_cast<CreatePluginFn>(PlatformSymbol(handle, kCreatePluginSymbol));
    auto destroy = reinterpret_cast<DestroyPluginFn>(PlatformSymbol(handle, kDestroyPluginSymbol));
    if (!create || !destroy) {
        LOG_ERROR("Plugins") << file.string() << ": не найдены символы "
                             << kCreatePluginSymbol << "/" << kDestroyPluginSymbol;
        PlatformClose(handle);
        return false;
    }
    SageEditorPlugin* instance = create();
    if (!instance) {
        LOG_ERROR("Plugins") << file.string() << ": " << kCreatePluginSymbol << " вернул nullptr";
        PlatformClose(handle);
        return false;
    }
    instance->OnLoad(ctx);
    LOG_INFO("Plugins") << "Загружен плагин: " << instance->Name() << " (" << file.filename().string() << ")";
    LoadedPlugin loaded;
    loaded.Handle = handle;
    loaded.Destroy = destroy;
    loaded.Instance = instance;
    loaded.Path = file.string();
    m_plugins.push_back(loaded);
    return true;
}

void PluginManager::UnloadOne(const fs::path& file) {
    for (size_t i = 0; i < m_plugins.size(); ++i) {
        LoadedPlugin& p = m_plugins[i];
        if (p.Path != file.string()) continue;
        p.Instance->OnUnload();
        p.Destroy(p.Instance);
        PlatformClose(p.Handle);
        LOG_INFO("Plugins") << "Выгружен плагин: " << p.Path;
        m_plugins.erase(m_plugins.begin() + (long)i);
        return;
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
