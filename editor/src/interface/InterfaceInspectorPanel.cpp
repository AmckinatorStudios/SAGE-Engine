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
#include "sage/ui/UIPresets.h"
#include "sage/ui/UISceneSystem.h"

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

void InterfaceInspectorPanel::DrawAlignTools(EditorHost& host) {
    const int count = SelectedUICount(host);

    ImGui::SeparatorText(T("Align"));
    if (count == 0) {
        Hint(T("Select an interface element."));
    } else {
        Hint(count == 1 ? T("One element — aligned to its parent")
                        : T("Aligned to the last clicked element"));
    }
    // Шесть кнопок с РИСУНКОМ, а не с подписью: полоска у края квадратика
    // читается быстрее слова «Left», а шесть слов заняли бы три строки.
    struct AlignDef { const char* Id; ui::AlignEdge Edge; const char* Tip; };
    const AlignDef aligns[6] = {
        {"al", ui::AlignEdge::Left, T("Left edges")},
        {"ac", ui::AlignEdge::CenterX, T("Centers horizontally")},
        {"ar", ui::AlignEdge::Right, T("Right edges")},
        {"at", ui::AlignEdge::Top, T("Top edges")},
        {"am", ui::AlignEdge::CenterY, T("Centers vertically")},
        {"ab", ui::AlignEdge::Bottom, T("Bottom edges")},
    };
    for (int i = 0; i < 6; ++i) {
        if (i == 3) ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x * 2.0f);
        else if (i > 0) ImGui::SameLine();
        if (AlignButton(aligns[i].Id, aligns[i].Edge, aligns[i].Tip, count >= 1))
            uiops::Align(host, aligns[i].Edge);
    }

    ImGui::SeparatorText(T("Distribute"));
    ImGui::BeginDisabled(count < 3);
    if (ImGui::Button(T("Across"))) uiops::Distribute(host, true, false);
    ImGui::SameLine();
    if (ImGui::Button(T("Down"))) uiops::Distribute(host, false, false);
    ImGui::EndDisabled();
    if (count < 3) Hint(T("Needs three elements or more."));

    ImGui::SeparatorText(T("Quick actions"));
    ImGui::BeginDisabled(count == 0);
    if (ImGui::Button(T("Fill the parent"), ImVec2(-1.0f, 0.0f))) uiops::StretchToParent(host, 0.0f);
    if (ImGui::Button(T("Fill with a margin"), ImVec2(-1.0f, 0.0f)))
        uiops::StretchToParent(host, 16.0f);
    if (ImGui::Button(T("Round to the grid"), ImVec2(-1.0f, 0.0f)))
        uiops::SnapSelectionToGrid(host);
    if (ImGui::Button(T("Bring back on screen"), ImVec2(-1.0f, 0.0f))) uiops::BringIntoView(host);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("Pushes the selected elements back inside the screen."));
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
    if (!ImGui::Begin(T("Element" "###InterfaceInspector"), &open,
                      panelwindows::WindowFlags("InterfaceInspector"))) {
        ImGui::End();
        return;
    }


    GameObject obj = host.SelectedObject();
    entt::registry& reg = host.CurrentScene().Registry();
    const bool isElement = obj.Valid() && reg.all_of<ui::Element>(obj.Entity());

    if (!isElement) {
        ImGui::TextDisabled("%s", T("Element"));
        ImGui::Separator();
        Hint(T("Select an element on the canvas or in the list on the left."));
    } else {
        ImGui::TextDisabled("%s", T("Element"));
        ImGui::SameLine();
        ImGui::TextUnformatted(obj.Name().c_str());
        ImGui::Separator();

        // Имя правится здесь же: в дереве слева его читают, а переименовывают
        // там, где смотрят на свойства.
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s", obj.Name().c_str());
        if (ImGui::InputText(T("Name"), buf, sizeof(buf))) obj.SetName(buf);
        host.TrackLastImGuiItem();

        // Инструменты вёрстки — ВЫШЕ свойств: они занимают три строки и нужны
        // постоянно, а свойств три десятка, и уехав под них, выравнивание
        // оказалось бы за пределами экрана.
        DrawAlignTools(host);

        ImGui::SeparatorText(T("Properties"));
        // Подписи у ImGui стоят СПРАВА от поля, и в узкой колонке они
        // обрезались посередине слова. Отдаём им фиксированную долю ширины.
        ImGui::PushItemWidth(-118.0f);
        // ТЕ ЖЕ свойства, что в инспекторе — общий модуль, а не вторая копия.
        sage::editor::UIPropsContext ctx;
        ctx.Preview = &m_preview;
        ctx.Browser = &m_browser;
        ctx.BrowseTarget = &m_browseTarget;
        sage::editor::DrawUIElementProperties(host, obj, ctx);
        ImGui::PopItemWidth();
    }


    ImGui::End();
}
