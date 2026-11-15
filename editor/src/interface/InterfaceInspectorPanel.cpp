#include "InterfaceInspectorPanel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "imgui.h"

#include "../EditorHost.h"
#include "../EditorIcons.h"
#include "InterfaceWidgets.h"
#include "../Localization.h"
#include "../PanelWindows.h"
#include "../Project.h"
#include "../UIElementProperties.h"
#include "../UILayoutOps.h"
#include "../ui/UI.h"
#include "sage/core/Config.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "sage/ui/UI.h"
#include "sage/ui/UISceneSystem.h"
#include "sage/ui/UIPresets.h"

namespace {

namespace ui = sage::ui;

using sage::editor::interfacewidgets::AlignButton;

// Приглушённое пояснение с переносом: колонки узкие, а обычный TextDisabled не
// переносит и обрезает строку посередине слова.
void Hint(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}


// Сколько выделенных элементов интерфейса: от этого зависит, что имеет смысл.
int SelectedUICount(EditorHost& host) {
    Scene& scene = host.CurrentScene();
    entt::registry& reg = scene.Registry();
    int count = 0;
    for (int id : host.Selection().All()) {
        GameObject obj = scene.Get(id);
        if (obj.Valid() && reg.all_of<ui::Element>(obj.Entity())) ++count;
    }
    return count;
}

// Разрешение, в котором интерфейс увидит игрок.
//
// Берётся из настроек ИГРЫ, а не из размера панели: под него считаются якоря,
// растяжения и проценты, и верстать в размер окна редактора значило бы верстать
// под экран, которого у игрока нет.
void GameFrameSize(EditorHost& host, int& outW, int& outH) {
    const sage::EngineConfig& cfg = host.Settings();
    outW = std::max(64, cfg.Width);
    outH = std::max(64, cfg.Height);
}

} // namespace

// ARRANGE — инструменты ВЫДЕЛЕНИЯ, а не свойства элемента: одной компактной
// строкой (выравнивание значками, распределение, быстрые действия списком),
// чтобы не занимать половину инспектора.
void InterfaceInspectorPanel::DrawAlignTools(EditorHost& host) {
    const int count = SelectedUICount(host);
    struct AlignDef { const char* Id; ui::AlignEdge Edge; const char* Tip; };
    const AlignDef aligns[6] = {
        {"al", ui::AlignEdge::Left, T("Left edges")},
        {"ac", ui::AlignEdge::CenterX, T("Centers horizontally")},
        {"ar", ui::AlignEdge::Right, T("Right edges")},
        {"at", ui::AlignEdge::Top, T("Top edges")},
        {"am", ui::AlignEdge::CenterY, T("Centers vertically")},
        {"ab", ui::AlignEdge::Bottom, T("Bottom edges")},
    };
    ImGui::Spacing();
    for (int i = 0; i < 6; ++i) {
        if (i == 3) ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x * 2.0f);
        else if (i > 0) ImGui::SameLine();
        if (AlignButton(aligns[i].Id, aligns[i].Edge, aligns[i].Tip, count >= 1))
            uiops::Align(host, aligns[i].Edge);
    }
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x * 2.0f);
    ImGui::BeginDisabled(count < 3);
    if (ImGui::Button(T("Across"))) uiops::Distribute(host, true, false);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", T("Distribute across: needs three elements or more"));
    ImGui::SameLine();
    if (ImGui::Button(T("Down"))) uiops::Distribute(host, false, false);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", T("Distribute down: needs three elements or more"));
    ImGui::EndDisabled();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::BeginDisabled(count == 0);
    if (ImGui::BeginCombo("##quick", T("Quick actions"))) {
        if (ImGui::Selectable(T("Fill the parent"))) uiops::StretchToParent(host, 0.0f);
        if (ImGui::Selectable(T("Fill with a margin"))) uiops::StretchToParent(host, 16.0f);
        if (ImGui::Selectable(T("Round to the grid"))) uiops::SnapSelectionToGrid(host);
        if (ImGui::Selectable(T("Bring back on screen"))) uiops::BringIntoView(host);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Pushes the selected elements back inside the screen."));
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
}


void InterfaceInspectorPanel::Draw(EditorHost& host, bool& open) {
    if (!open) return;
    m_browser.SetPreview(&m_preview);
    if (m_focusFrames > 0) {
        ImGui::SetNextWindowFocus();
        --m_focusFrames;
    }

    // Файловый диалог качается ДО окна: он живёт дольше одного кадра, а его
    // результат надо положить в поле, о котором знает только эта панель.
    if (m_browser.Draw() && m_browseTarget) {
        // Ссылка ОТНОСИТЕЛЬНО ПРОЕКТА: абсолютный путь уехал бы в файл и не
        // открылся бы ни на другой машине, ни в собранной игре.
        *m_browseTarget = host.CurrentProject().AssetRef(m_browser.Result());
        m_browseTarget = nullptr;
    }

    ImGui::SetNextWindowSize(ImVec2(380.0f, 720.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(EditorIcons::WindowTitle("inspector", T("Element"), "InterfaceInspector").c_str(), &open,
                      panelwindows::WindowFlags("InterfaceInspector"))) {
        ImGui::End();
        return;
    }


    GameObject obj = host.SelectedObject();
    entt::registry& reg = host.CurrentScene().Registry();
    const bool isElement = obj.Valid() && reg.all_of<ui::Element>(obj.Entity());

    const bool isInterface = obj.Valid() && reg.all_of<ui::InterfaceComponent>(obj.Entity());
    const bool isMain = isElement && ui::IsMainContainer(host.CurrentScene(), obj.Entity());

    if (isMain) {
        // ГЛАВНЫЙ КОНТЕЙНЕР — это сам интерфейс: его область и холст. Места,
        // размера и якоря у него нет (он всегда во весь экран), поэтому и
        // полей элемента здесь нет — только имя интерфейса и холст.
        GameObject iface = host.CurrentScene().Get(host.CurrentInterfaceId());
        ImGui::TextDisabled("%s", T("Interface"));
        ImGui::SameLine();
        ImGui::TextUnformatted(obj.Name().c_str());
        ImGui::Separator();
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s", obj.Name().c_str());
        if (ImGui::InputText(T("Name"), buf, sizeof(buf))) {
            obj.SetName(buf);
            if (iface.Valid()) iface.SetName(buf);   // имя ресурса — то же
        }
        host.TrackLastImGuiItem();
        ImGui::PushItemWidth(-118.0f);
        ImGui::SeparatorText(T("Canvas"));
        sage::editor::DrawInterfaceCanvas(host, iface);
        ImGui::PopItemWidth();
        Hint(T("The main container is the whole interface area: everything else lives inside it."));
    } else if (isInterface) {
        // ВЫБРАН САМ ИНТЕРФЕЙС — показываем ЕГО свойства, а не пустоту и не
        // поля элемента: это разные вещи, и окно обязано их различать.
        ImGui::TextDisabled("%s", T("Interface"));
        ImGui::SameLine();
        ImGui::TextUnformatted(obj.Name().c_str());
        ImGui::Separator();
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s", obj.Name().c_str());
        if (ImGui::InputText(T("Name"), buf, sizeof(buf))) obj.SetName(buf);
        host.TrackLastImGuiItem();
        ImGui::PushItemWidth(-118.0f);
        sage::editor::DrawInterfaceProperties(host, obj);
        ImGui::PopItemWidth();
    } else if (!isElement) {
        ImGui::TextDisabled("%s", T("Element"));
        ImGui::Separator();
        Hint(T("Select an element on the canvas or in the list on the left."));
    } else {
        // Подписи у ImGui стоят СПРАВА от поля, и в узкой колонке они
        // обрезались посередине слова. Отдаём им фиксированную долю ширины.
        ImGui::PushItemWidth(-118.0f);
        // ТЕ ЖЕ свойства, что в инспекторе — общий модуль, а не вторая копия.
        sage::editor::UIPropsContext ctx;
        ctx.Preview = &m_preview;
        ctx.Browser = &m_browser;
        ctx.BrowseTarget = &m_browseTarget;
        ctx.ShowEvents = true;
        ctx.ShowIdentity = true;
        ctx.Arrange = [this, &host] { DrawAlignTools(host); };
        sage::editor::DrawUIElementProperties(host, obj, ctx);
        ImGui::PopItemWidth();
    }


    ImGui::End();
}
