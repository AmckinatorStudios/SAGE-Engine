// ===========================================================================
//  ДОКИНГ: пределы размера панелей (патч [SAGE] в external/imgui/imgui.cpp,
//  DockNodeCalcLimit). Панель задаёт SetNextWindowSizeConstraints, а узел дока
//  не даёт ни раскладке, ни изменению окна сжать её ниже минимума или раздуть
//  выше максимума; невыполнимые пределы (окно мало на всех) не ломают док.
// ===========================================================================
#include "TestFramework.h"

#include <cfloat>
#include <functional>

#include "imgui.h"
#include "imgui_internal.h"

namespace {

struct DockHarness {
    ImGuiContext* Ctx = nullptr;
    ImGuiID Space = 0, Left = 0, Center = 0;
    bool Built = false;
    float LeftMin = 0.0f, LeftMax = FLT_MAX;
    float LeftRatio = 0.1f;
    DockHarness() {
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
    ~DockHarness() { ImGui::DestroyContext(Ctx); }

    // Один кадр; возвращает ширину окна «Left» (в доке).
    float Frame(const ImVec2& display) {
        ImGui::GetIO().DisplaySize = display;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(display);
        ImGui::Begin("Host", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                                          ImGuiWindowFlags_NoDocking);
        Space = ImGui::GetID("Space");
        if (!Built) {
            ImGui::DockBuilderRemoveNode(Space);
            ImGui::DockBuilderAddNode(Space, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(Space, display);
            Center = Space;
            Left = ImGui::DockBuilderSplitNode(Center, ImGuiDir_Left, LeftRatio, nullptr, &Center);
            ImGui::DockBuilderDockWindow("Left", Left);
            ImGui::DockBuilderDockWindow("Center", Center);
            ImGui::DockBuilderFinish(Space);
            Built = true;
        }
        ImGui::DockSpace(Space, ImVec2(0, 0));
        ImGui::End();

        ImGui::SetNextWindowSizeConstraints(ImVec2(LeftMin, 0.0f), ImVec2(LeftMax, FLT_MAX));
        ImGui::Begin("Left");
        const float w = ImGui::GetWindowSize().x;
        ImGui::End();
        ImGui::SetNextWindowSizeConstraints(ImVec2(120.0f, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
        ImGui::Begin("Center");
        ImGui::End();
        ImGui::Render();
        return w;
    }
    float Settle(const ImVec2& display) {
        float w = 0.0f;
        for (int i = 0; i < 6; ++i) w = Frame(display);
        return w;
    }
};

} // namespace

TEST(DockLimits_minimum_width_holds_when_the_layout_asks_for_less) {
    DockHarness h;
    h.LeftMin = 240.0f;
    h.LeftRatio = 0.1f;   // 100 точек по раскладке — меньше минимума
    const float w = h.Settle(ImVec2(1000, 600));
    CHECK_TRUE(w >= 240.0f - 1.0f);
}

TEST(DockLimits_maximum_width_holds_when_the_layout_asks_for_more) {
    DockHarness h;
    h.LeftMax = 300.0f;
    h.LeftRatio = 0.6f;   // 600 точек по раскладке — больше максимума
    const float w = h.Settle(ImVec2(1000, 600));
    CHECK_TRUE(w <= 300.0f + 1.0f);
    CHECK_TRUE(w >= 200.0f);   // и не схлопнулась
}

TEST(DockLimits_window_shrink_keeps_the_minimum_and_the_rest_fills) {
    DockHarness h;
    h.LeftMin = 220.0f;
    h.LeftRatio = 0.3f;
    h.Settle(ImVec2(1000, 600));
    const float w = h.Settle(ImVec2(500, 600));   // окно вдвое уже: доля 30 % дала бы 150
    CHECK_TRUE(w >= 220.0f - 1.0f);
}

TEST(DockLimits_impossible_limits_do_not_break_the_dock) {
    DockHarness h;
    h.LeftMin = 900.0f;   // больше, чем вся ширина окна без центра
    const float w = h.Settle(ImVec2(600, 400));
    CHECK_TRUE(w > 0.0f && w <= 600.0f);
}
