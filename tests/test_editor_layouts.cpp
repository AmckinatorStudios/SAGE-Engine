// Пользовательские макеты редактора держатся на ini ImGui в памяти
// (EditorLayer_Layouts.cpp): снимок SaveIniSettingsToMemory, загрузка
// LoadIniSettingsFromMemory между кадрами. Проверяем сам механизм на
// настоящем доке: размер узла и расположение вкладок возвращаются, а
// последующая правка расклада не просачивается в уже снятый снимок.
#include "TestFramework.h"

#include <string>

#include "imgui.h"
#include "imgui_internal.h"

namespace {

struct LayoutHarness {
    ImGuiContext* Ctx = nullptr;
    ImGuiID Space = 0;
    bool Built = false;
    LayoutHarness() {
        Ctx = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(1000, 600);
        io.DeltaTime = 1.0f / 60.0f;
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        unsigned char* px = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    }
    ~LayoutHarness() { ImGui::DestroyContext(Ctx); }

    void Build(float leftRatio) {
        Space = ImGui::GetID("Space");
        ImGui::DockBuilderRemoveNode(Space);
        ImGui::DockBuilderAddNode(Space, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(Space, ImVec2(1000, 600));
        ImGuiID center = Space;
        ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, leftRatio, nullptr, &center);
        ImGui::DockBuilderDockWindow("Left", left);
        ImGui::DockBuilderDockWindow("Center", center);
        ImGui::DockBuilderFinish(Space);
    }

    float Frame(float rebuildRatio = -1.0f) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(1000, 600));
        ImGui::Begin("Host", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                                          ImGuiWindowFlags_NoDocking);
        Space = ImGui::GetID("Space");
        if (rebuildRatio > 0.0f) Build(rebuildRatio);
        ImGui::DockSpace(Space, ImVec2(0, 0));
        ImGui::End();
        ImGui::Begin("Left");
        const float w = ImGui::GetWindowSize().x;
        ImGui::End();
        ImGui::Begin("Center");
        ImGui::End();
        ImGui::Render();
        return w;
    }
    float Settle(float rebuildRatio = -1.0f) {
        float w = Frame(rebuildRatio);
        for (int i = 0; i < 6; ++i) w = Frame();
        return w;
    }
};

std::string Snapshot() {
    size_t len = 0;
    const char* ini = ImGui::SaveIniSettingsToMemory(&len); // длина — ПОСЛЕ вызова
    return std::string(ini, len);
}

} // namespace

TEST(Layouts_ini_in_memory_restores_dock_sizes) {
    LayoutHarness h;
    const float narrow = h.Settle(0.15f);
    const std::string saved = Snapshot();

    const float wide = h.Settle(0.45f); // «другой» макет
    CHECK_TRUE(wide > narrow + 100.0f);

    // Загрузка — между кадрами, как в EditorLayer::ApplyPendingLayout.
    ImGui::LoadIniSettingsFromMemory(saved.c_str(), saved.size());
    float restored = 0.0f;
    for (int i = 0; i < 8; ++i) restored = h.Frame();
    CHECK_TRUE(restored < narrow + 20.0f);
    CHECK_TRUE(restored > narrow - 20.0f);
}

TEST(Layouts_snapshot_is_independent_of_later_changes) {
    LayoutHarness h;
    const float narrow = h.Settle(0.15f);
    const std::string saved = Snapshot();
    h.Settle(0.45f);
    const std::string later = Snapshot();
    CHECK_TRUE(saved != later);   // макет — копия строки, а не ссылка на живое состояние
    (void)narrow;
}
