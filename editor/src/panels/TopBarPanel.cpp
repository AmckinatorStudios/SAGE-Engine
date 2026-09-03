#include "TopBarPanel.h"

#include <algorithm>

#include "imgui.h"

#include "EditorHost.h"
#include "EditorIcons.h"
#include "../Localization.h"

namespace {

// Кнопка окна: иконка + подсказка, подсвечена, когда окно открыто. Щелчок
// переключает. Одним помощником, потому что кнопок десяток и разъехаться в
// поведении они не должны.
void PanelToggle(EditorHost& host, EditorPanel panel, const char* icon, const char* tip) {
    bool& open = host.PanelVisible(panel);
    if (EditorIcons::IconOnlyButton(icon, tip, open)) open = !open;
    ImGui::SameLine();
}

} // namespace

void TopBarPanel::Draw(EditorHost& host, float height) {
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 4));
    ImGui::BeginChild("##topbar", ImVec2(0, height), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar);

    // --- Слева: рабочее место (какие окна показаны) ---
    ImGui::AlignTextToFramePadding();
    PanelToggle(host, EditorPanel::Hierarchy, "layout", T("Hierarchy"));
    PanelToggle(host, EditorPanel::Inspector, "file", T("Inspector"));
    PanelToggle(host, EditorPanel::Assets, "folder", T("Assets"));
    PanelToggle(host, EditorPanel::Console, "debug", T("Console"));
    PanelToggle(host, EditorPanel::Code, "code", T("Code"));
    PanelToggle(host, EditorPanel::Profiler, "info", T("Profiler"));

    ImGui::TextDisabled("|");
    ImGui::SameLine();

    // --- Настройки: два окна и граница между ними ---
    //
    // Они стоят рядом намеренно. Вопрос «где настраивается свет» раньше не имел
    // ответа: часть была в окне Lighting, часть в Game Settings, часть на
    // объекте. Соседство кнопок с внятными подсказками — самая дешёвая форма
    // ответа: среда сцены здесь, цена кадра там, источники — объекты.
    PanelToggle(host, EditorPanel::Environment, "sun",
                T("Environment: sky, air, ambient light (saved with the scene)"));
    PanelToggle(host, EditorPanel::Settings, "gear",
                T("Game Settings: quality and cost of the frame (saved with the project)"));
    PanelToggle(host, EditorPanel::UITools, "rect", T("UI layout tools"));

    ImGui::TextDisabled("|");
    ImGui::SameLine();
    PanelToggle(host, EditorPanel::Viewport, "cube", T("Viewport"));
    PanelToggle(host, EditorPanel::Game, "camera", T("Game (view from the game camera)"));

    // --- По центру: Play / Pause / Stop ---
    //
    // Центрирование считается ОТ ОКНА и зажимается между левым и правым
    // блоками: ImGui::SameLine(x) с координатой левее курсора честно ставит
    // курсор назад, и блок рисуется ПОВЕРХ уже нарисованного — кнопки просто
    // исчезали с экрана.
    const float leftEnd = ImGui::GetCursorPosX();
    const float playBlockW = 220.0f;
    const float rightBlockW = 210.0f;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float windowW = ImGui::GetWindowWidth();
    float playX = windowW * 0.5f - playBlockW * 0.5f;
    const float playMax = windowW - rightBlockW - playBlockW - spacing;
    if (playX > playMax) playX = playMax;
    if (playX < leftEnd + spacing) playX = leftEnd + spacing;
    ImGui::SameLine(playX);

    const EditorPlayState state = host.GetPlayState();
    if (state == EditorPlayState::Editing) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.55f, 0.25f, 1.0f));
        if (EditorIcons::Button("play", T("Play"), T("Run the scene (it is restored on Stop)")))
            host.StartPlay();
        ImGui::PopStyleColor();
    } else {
        if (state == EditorPlayState::Playing) {
            if (EditorIcons::Button("pause", T("Pause"), T("Pause"))) host.PausePlay();
        } else {
            if (EditorIcons::Button("play", T("Resume"), T("Resume"))) host.ResumePlay();
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.62f, 0.20f, 0.20f, 1.0f));
        if (EditorIcons::Button("stop", T("Stop"), T("Stop and restore the scene")))
            host.StopPlay();
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextColored(state == EditorPlayState::Playing ? ImVec4(0.4f, 0.9f, 0.4f, 1.0f)
                                                             : ImVec4(0.9f, 0.8f, 0.3f, 1.0f),
                           state == EditorPlayState::Playing ? "PLAYING" : "PAUSED");
    }

    // --- Справа: что открыто сейчас ---
    //
    // Имя сцены и пометка о несохранённых правках. В статус-баре они тоже есть,
    // но статус-бар внизу, а смотрят при работе — вверх, на кнопку Play.
    const float rightX = std::max(ImGui::GetCursorPosX() + spacing, windowW - rightBlockW);
    ImGui::SameLine(rightX);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s%s", host.CurrentSceneName().c_str(), host.SceneDirty() ? " *" : "");

    ImGui::EndChild();
    ImGui::PopStyleVar(2);
}
