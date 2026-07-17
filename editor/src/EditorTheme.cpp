#include "EditorTheme.h"

#include <cstdio>
#include "imgui.h"

namespace EditorTheme {

void LoadFont() {
    ImGuiIO& io = ImGui::GetIO();
    const char* candidates[] = {
        "assets/fonts/editor.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
    };
    for (const char* path : candidates) {
        FILE* probe = std::fopen(path, "rb");
        if (!probe) continue;
        std::fclose(probe);
        io.Fonts->AddFontFromFileTTF(path, 16.0f, nullptr, io.Fonts->GetGlyphRangesCyrillic());
        return;
    }
    // Ничего не нашлось — остаёмся на встроенном ProggyClean (ASCII).
}

void Apply() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::StyleColorsDark();

    // --- метрики: спокойные скругления, чуть больше воздуха ---
    style.WindowRounding = 4.0f;
    style.ChildRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.PopupRounding = 4.0f;
    style.GrabRounding = 3.0f;
    style.TabRounding = 4.0f;
    style.ScrollbarRounding = 6.0f;
    style.WindowPadding = ImVec2(8, 8);
    style.FramePadding = ImVec2(8, 4);
    style.ItemSpacing = ImVec2(8, 5);
    style.ItemInnerSpacing = ImVec2(6, 4);
    style.ScrollbarSize = 12.0f;
    style.GrabMinSize = 10.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.PopupBorderSize = 1.0f;
    style.WindowMenuButtonPosition = ImGuiDir_None; // без кнопки-стрелки в заголовках

    // --- палитра: глубокий нейтральный фон + сдержанный синий акцент ---
    ImVec4* c = style.Colors;
    const ImVec4 bg0(0.114f, 0.122f, 0.137f, 1.00f); // окна
    const ImVec4 bg1(0.145f, 0.153f, 0.173f, 1.00f); // фреймы/заголовки
    const ImVec4 bg2(0.184f, 0.196f, 0.220f, 1.00f); // hover
    const ImVec4 accent(0.259f, 0.443f, 0.780f, 1.00f);       // активный акцент
    const ImVec4 accentHi(0.322f, 0.518f, 0.870f, 1.00f);     // акцент hover
    const ImVec4 text(0.90f, 0.91f, 0.93f, 1.00f);
    const ImVec4 textDim(0.55f, 0.57f, 0.62f, 1.00f);

    c[ImGuiCol_Text] = text;
    c[ImGuiCol_TextDisabled] = textDim;
    c[ImGuiCol_WindowBg] = bg0;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.106f, 0.12f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(0.24f, 0.25f, 0.28f, 0.50f);
    c[ImGuiCol_FrameBg] = bg1;
    c[ImGuiCol_FrameBgHovered] = bg2;
    c[ImGuiCol_FrameBgActive] = ImVec4(0.22f, 0.24f, 0.27f, 1.00f);
    c[ImGuiCol_TitleBg] = bg0;
    c[ImGuiCol_TitleBgActive] = bg1;
    c[ImGuiCol_TitleBgCollapsed] = bg0;
    c[ImGuiCol_MenuBarBg] = ImVec4(0.096f, 0.102f, 0.115f, 1.00f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0.15f);
    c[ImGuiCol_ScrollbarGrab] = bg2;
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.28f, 0.30f, 0.34f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive] = accent;
    c[ImGuiCol_CheckMark] = accentHi;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = accentHi;
    c[ImGuiCol_Button] = bg1;
    c[ImGuiCol_ButtonHovered] = bg2;
    c[ImGuiCol_ButtonActive] = accent;
    c[ImGuiCol_Header] = ImVec4(0.20f, 0.24f, 0.32f, 1.00f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.24f, 0.30f, 0.42f, 1.00f);
    c[ImGuiCol_HeaderActive] = accent;
    c[ImGuiCol_Separator] = c[ImGuiCol_Border];
    c[ImGuiCol_SeparatorHovered] = accent;
    c[ImGuiCol_SeparatorActive] = accentHi;
    c[ImGuiCol_ResizeGrip] = ImVec4(0.26f, 0.28f, 0.32f, 0.6f);
    c[ImGuiCol_ResizeGripHovered] = accent;
    c[ImGuiCol_ResizeGripActive] = accentHi;
    c[ImGuiCol_Tab] = bg1;
    c[ImGuiCol_TabHovered] = accentHi;
    c[ImGuiCol_TabActive] = ImVec4(0.216f, 0.322f, 0.520f, 1.00f);
    c[ImGuiCol_TabUnfocused] = bg1;
    c[ImGuiCol_TabUnfocusedActive] = ImVec4(0.18f, 0.23f, 0.33f, 1.00f);
    c[ImGuiCol_DockingPreview] = ImVec4(accent.x, accent.y, accent.z, 0.55f);
    c[ImGuiCol_DockingEmptyBg] = ImVec4(0.09f, 0.095f, 0.107f, 1.00f);
    c[ImGuiCol_PlotLines] = accentHi;
    c[ImGuiCol_PlotHistogram] = accent;
    c[ImGuiCol_TextSelectedBg] = ImVec4(accent.x, accent.y, accent.z, 0.35f);
    c[ImGuiCol_DragDropTarget] = accentHi;
    c[ImGuiCol_NavHighlight] = accentHi;

    // Мульти-вьюпорт: платформенные окна рисуются без своих скруглений, чтобы
    // вытащенная панель выглядела как обычное OS-окно, а не «плавающий» блок.
    ImGuiIO& io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        style.WindowRounding = 0.0f;
        c[ImGuiCol_WindowBg].w = 1.0f;
    }
}

} // namespace EditorTheme
