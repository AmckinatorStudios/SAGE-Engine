#include "ui/SagePanelHost.h"

#include "imgui.h"

SageUIIsland& SagePanelHost::IslandFor(EditorPanel& panel) {
    auto it = m_islands.find(&panel);
    if (it != m_islands.end()) return *it->second;
    auto island = std::make_unique<SageUIIsland>();
    SageUIIsland& ref = *island;
    m_islands[&panel] = std::move(island);
    return ref;
}

void SagePanelHost::Draw(EditorPanel& panel, const char* windowTitle, bool* open, float dt,
                         ImVec2 defaultSize) {
    SageUIIsland& island = IslandFor(panel);
    // Дерево собирается при первом показе, а не в конструкторе: до этого
    // момента не известно ни контекста, ни размера, и собранная «вслепую»
    // панель всё равно пересобиралась бы.
    panel.EnsureBuilt(island.Ui(), island.Ui().Content());
    panel.Sync(dt);

    // Без внутренних отступов: остров занимает окно целиком, а свои отступы
    // панель рисует сама — иначе их было бы двое, и они бы не совпадали.
    ImGui::SetNextWindowSize(defaultSize, ImGuiCond_FirstUseEver);
    if (m_focus == &panel) {
        ImGui::SetNextWindowFocus();
        m_focus = nullptr;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    const bool visible = ImGui::Begin(windowTitle, open);
    ImGui::PopStyleVar();
    if (visible) {
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        island.DrawInWindow(dt);
        if (island.KeyboardCaptured()) m_keyboard = true;

        // Ассет из проводника ассетов. Целью объявляется КАРТИНКА острова (её и
        // видит человек как панель), а точка переводится в её координаты: панель
        // обязана отличить «на этот объект» от «в пустое место», и по-другому
        // ей это не узнать.
        if (panel.AcceptsAssets() && ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("SAGE_ASSET_PATH")) {
                std::string dropped((const char*)p->Data, (size_t)p->DataSize);
                if (!dropped.empty() && dropped.back() == '\0') dropped.pop_back();
                const ImVec2 mouse = ImGui::GetMousePos();
                panel.OnAssetDropped(dropped, {mouse.x - origin.x, mouse.y - origin.y});
            }
            ImGui::EndDragDropTarget();
        }
    }
    ImGui::End();
}

void SagePanelHost::SyncHidden(EditorPanel& panel, float dt) {
    if (!panel.Built()) return;   // не собрана — значит и копить ей нечего
    panel.Sync(dt);
}
