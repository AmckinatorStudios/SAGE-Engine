#pragma once

// ============================================================================
//  PluginAPI.h — контракт плагинов редактора SAGE (v1).
//
//  Плагин — динамическая библиотека (.so/.dll), которую PluginManager грузит
//  из каталога plugins/ рядом с SageEditor. Умышленно НЕ пробрасывает сырые
//  Scene&/entt::registry& через границу библиотеки: разные версии
//  компилятора/STL у хоста и плагина делают это ABI-небезопасным. Вместо
//  этого — узкий facade EditorPluginContext, расширяемый по мере появления
//  реальных плагинов, а не заранее.
//
//  v1: без гарантий бинарной совместимости между версиями движка/редактора —
//  плагин пересобирается вместе с редактором. Только редактор (панели,
//  инструменты); рантайм игры плагины пока не касается.
// ============================================================================

class EditorPluginContext {
public:
    virtual ~EditorPluginContext() = default;

    virtual void Log(const char* message) = 0;
    virtual const char* SelectedEntityName() const = 0; // "" если ничего не выбрано
    virtual void SetStatusMessage(const char* message) = 0;
};

class SageEditorPlugin {
public:
    virtual ~SageEditorPlugin() = default;

    virtual const char* Name() const = 0;
    virtual void OnLoad(EditorPluginContext& ctx) {}
    virtual void OnUnload() {}
    virtual void OnUpdate(float dt) {}
    virtual void OnImGui() {} // рисует свои ImGui::Begin(...) панели/пункты меню
};

// Плагин экспортирует ровно две C-функции с этими сигнатурами
// (extern "C", чтобы избежать C++ name mangling между хостом и плагином):
//
//   extern "C" SageEditorPlugin* CreateSageEditorPlugin() { return new MyPlugin(); }
//   extern "C" void DestroySageEditorPlugin(SageEditorPlugin* p) { delete p; }
using CreatePluginFn = SageEditorPlugin* (*)();
using DestroyPluginFn = void (*)(SageEditorPlugin*);

constexpr const char* kCreatePluginSymbol = "CreateSageEditorPlugin";
constexpr const char* kDestroyPluginSymbol = "DestroySageEditorPlugin";
