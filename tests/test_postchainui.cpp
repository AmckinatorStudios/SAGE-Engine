// ===========================================================================
//  РЕДАКТОР ТРАКТА ПОСТ-ОБРАБОТКИ (editor/src/PostChainUi.h) — настоящими
//  кадрами ImGui без окна.
//
//  Подпись звена и подпись его параметра бывают одинаковыми («Экспозиция» у
//  галки звена и у его ползунка). В одной области имён ImGui это ОДИН
//  идентификатор на два виджета: ImGui выводил «conflicting ID», а щелчок по
//  одному трогал другой. Конфликт ImGui ловит при наведении: мышь проходит по
//  всему редактору, и ни в одной точке под ней не должно оказаться двух
//  виджетов с одним идентификатором.
// ===========================================================================
#include "TestFramework.h"

#include <algorithm>

#include "imgui.h"
#include "imgui_internal.h"
#include "PostChainUi.h"
#include "sage/render/PostEffect.h"

TEST(PostChainUi_no_duplicate_widget_ids) {
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = ImVec2(700, 2400);
    io.DeltaTime = 1.0f / 60.0f;
    io.ConfigInputTrickleEventQueue = false;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);

    // Все звенья каталога включены — все их параметры на экране.
    sage::render::PostChain chain;
    for (const sage::render::PostEffectKind* kind : sage::render::PostEffectCatalog::Instance().All())
        sage::render::AddPostEffect(chain, kind->Id);

    int worst = 0;
    for (float y = 4.0f; y < 2380.0f; y += 5.0f) {
        for (float x : {30.0f, 200.0f, 400.0f}) {
            io.AddMousePosEvent(x, y);
            for (int f = 0; f < 2; ++f) {
                ImGui::NewFrame();
                ImGui::SetNextWindowPos(ImVec2(0, 0));
                ImGui::SetNextWindowSize(ImVec2(700, 2400));
                ImGui::Begin("Post", nullptr, ImGuiWindowFlags_NoSavedSettings);
                sage::editor::DrawPostChainEditor(nullptr, chain, "camera");
                ImGui::End();
                ImGui::Render();
                worst = std::max(worst, ctx->HoveredIdPreviousFrameItemCount);
            }
        }
    }
    ImGui::DestroyContext(ctx);
    CHECK_TRUE(worst <= 1);   // 2 и больше — два виджета с одним идентификатором
}
